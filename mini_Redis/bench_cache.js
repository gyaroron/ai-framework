"use strict";

/*
 * 작은 HTTP API 하나를 만들어, Mini Redis를 쓸 때와 안 쓸 때의 응답 시간을 비교한다.
 *
 *   /origin  요청마다 Mini DBMS에서 값을 읽는다(cache 없음)
 *   /cached  Mini Redis에 먼저 묻는다. 없으면 Mini DBMS에서 읽고, 그 API 응답을
 *            보관 기간과 함께 Mini Redis에 넣는다(SET key value EX 초)
 *
 * 먼저 두 서버를 띄운다.
 *   mini_Redis: ./mini_redis --server        (port 6379)
 *   mini_DBMS:  ./mini_dbms_server           (port 6380)
 *
 * 실행: node bench_cache.js
 * 환경 변수: REDIS_HOST, REDIS_PORT, DBMS_HOST, DBMS_PORT, REQUESTS, CACHE_TTL_SECONDS,
 *           ORIGIN_DELAY_MS(원본을 읽을 때마다 더 기다릴 시간. 느린 원본을 흉내 낸다. 기본 0)
 *
 * SQL_PORT를 주면 원본(origin)을 Mini DBMS SQL 서버로 바꾼다.
 *   ORIGIN_SQL 문장(기본값: name 컬럼으로 찾는 선형 탐색)을 매 요청마다 실행한다.
 *   예) mini_dbms_sql <dir> --bench users 1000000 으로 100만 건을 넣은 뒤
 *       ./mini_dbms_sql_server <dir> 6381 4 를 띄우고
 *       SQL_PORT=6381 REQUESTS=200 node bench_cache.js
 */
const http = require("node:http");
const { connect } = require("./resp_client");

const REDIS_HOST = process.env.REDIS_HOST || "127.0.0.1";
const REDIS_PORT = Number(process.env.REDIS_PORT || 6379);
const DBMS_HOST = process.env.DBMS_HOST || "127.0.0.1";
const DBMS_PORT = Number(process.env.DBMS_PORT || 6380);
const REQUESTS = Number(process.env.REQUESTS || 2000);
const CACHE_TTL_SECONDS = Number(process.env.CACHE_TTL_SECONDS || 60);
const ORIGIN_DELAY_MS = Number(process.env.ORIGIN_DELAY_MS || 0);
// SQL 서버를 원본으로 쓸 때의 port와 문장. SQL_PORT가 없으면 Mini DBMS bridge(GET server:ip)를 쓴다.
const SQL_PORT = process.env.SQL_PORT ? Number(process.env.SQL_PORT) : null;
const ORIGIN_SQL = process.env.ORIGIN_SQL || "SELECT * FROM users WHERE name = 'user_777777';";
const WARMUP_REQUESTS = SQL_PORT === null ? 50 : 3;
const DATA_KEY = "server:ip";
const CACHE_KEY = "bench:api:server-ip";
const SEED_VALUE = "192.168.0.1";

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function report(message) {
  process.stdout.write(`${message}\n`);
}

function startApi(redis, dbms, counters) {
  async function readOrigin() {
    counters.originReads += 1;
    if (ORIGIN_DELAY_MS > 0) await sleep(ORIGIN_DELAY_MS);
    if (SQL_PORT !== null) {
      const result = await dbms.command(["SQL", ORIGIN_SQL]);
      return JSON.stringify({ sql: ORIGIN_SQL, result });
    }
    const value = await dbms.command(["GET", DATA_KEY]);
    return JSON.stringify({ key: DATA_KEY, value });
  }

  const server = http.createServer(async (request, response) => {
    try {
      let body;
      if (request.url === "/origin") {
        body = await readOrigin();
      } else if (request.url === "/cached") {
        body = await redis.command(["GET", CACHE_KEY]);
        if (body === null) {
          counters.cacheMisses += 1;
          body = await readOrigin();
          await redis.command(["SET", CACHE_KEY, body, "EX", CACHE_TTL_SECONDS]);
        } else {
          counters.cacheHits += 1;
        }
      } else {
        response.writeHead(404).end();
        return;
      }
      response.writeHead(200, { "content-type": "application/json" });
      response.end(body);
    } catch (error) {
      response.writeHead(502, { "content-type": "text/plain" });
      response.end(String(error.message));
    }
  });

  return new Promise((resolve, reject) => {
    server.once("error", reject);
    server.listen(0, "127.0.0.1", () => resolve(server));
  });
}

function requestOnce(agent, port, path) {
  return new Promise((resolve, reject) => {
    const startedAt = process.hrtime.bigint();
    const request = http.get({ host: "127.0.0.1", port, path, agent }, (response) => {
      const chunks = [];
      response.on("data", (chunk) => chunks.push(chunk));
      response.on("end", () => {
        const elapsedMs = Number(process.hrtime.bigint() - startedAt) / 1e6;
        if (response.statusCode !== 200) {
          reject(new Error(`${path} answered ${response.statusCode}: ${Buffer.concat(chunks)}`));
          return;
        }
        resolve({ elapsedMs, body: Buffer.concat(chunks).toString("utf8") });
      });
    });
    request.once("error", reject);
  });
}

async function measure(agent, port, path, count) {
  const samples = [];
  let lastBody = "";
  const startedAt = process.hrtime.bigint();
  for (let i = 0; i < count; i += 1) {
    const result = await requestOnce(agent, port, path);
    samples.push(result.elapsedMs);
    lastBody = result.body;
  }
  const totalMs = Number(process.hrtime.bigint() - startedAt) / 1e6;
  samples.sort((left, right) => left - right);
  const at = (ratio) => samples[Math.min(samples.length - 1, Math.floor(samples.length * ratio))];
  return {
    totalMs,
    averageMs: totalMs / count,
    medianMs: at(0.5),
    p95Ms: at(0.95),
    requestsPerSecond: (count / totalMs) * 1000,
    lastBody,
  };
}

function formatRow(label, result) {
  return [
    label.padEnd(22),
    `${result.averageMs.toFixed(3)} ms`.padStart(12),
    `${result.medianMs.toFixed(3)} ms`.padStart(12),
    `${result.p95Ms.toFixed(3)} ms`.padStart(12),
    `${result.requestsPerSecond.toFixed(0)} req/s`.padStart(14),
  ].join(" ");
}

async function main() {
  const redis = await connect({ host: REDIS_HOST, port: REDIS_PORT });
  let dbms;
  try {
    dbms = await connect({ host: DBMS_HOST, port: SQL_PORT === null ? DBMS_PORT : SQL_PORT });
  } catch (error) {
    redis.close();
    throw error;
  }
  const counters = { originReads: 0, cacheHits: 0, cacheMisses: 0 };
  const agent = new http.Agent({ keepAlive: true, maxSockets: 1 });
  let server = null;

  try {
    if (SQL_PORT === null) await dbms.command(["SET", DATA_KEY, SEED_VALUE]);
    await redis.command(["DEL", CACHE_KEY]);
    server = await startApi(redis, dbms, counters);
    const { port } = server.address();

    await measure(agent, port, "/origin", WARMUP_REQUESTS);
    await measure(agent, port, "/cached", WARMUP_REQUESTS);
    counters.originReads = 0;
    counters.cacheHits = 0;
    counters.cacheMisses = 0;
    await redis.command(["DEL", CACHE_KEY]);

    const withoutCache = await measure(agent, port, "/origin", REQUESTS);
    const originReadsWithoutCache = counters.originReads;
    counters.originReads = 0;
    const withCache = await measure(agent, port, "/cached", REQUESTS);

    if (withoutCache.lastBody !== withCache.lastBody) {
      throw new Error("cached response differs from the origin response");
    }

    report(`origin: ${SQL_PORT === null ? "Mini DBMS bridge GET server:ip" : ORIGIN_SQL}`);
    report(`requests per run: ${REQUESTS}, origin delay: ${ORIGIN_DELAY_MS} ms, cache TTL: ${CACHE_TTL_SECONDS} s`);
    report(`${"".padEnd(22)} ${"average".padStart(12)} ${"median".padStart(12)} ${"p95".padStart(12)} ${"throughput".padStart(14)}`);
    report(formatRow("without Mini Redis", withoutCache));
    report(formatRow("with Mini Redis", withCache));
    report(`origin reads: ${originReadsWithoutCache} without cache, ${counters.originReads} with cache`);
    report(`cache hits: ${counters.cacheHits}, cache misses: ${counters.cacheMisses}`);
    report(`average time ratio (without / with): ${(withoutCache.averageMs / withCache.averageMs).toFixed(2)}`);
  } finally {
    agent.destroy();
    if (server !== null) server.close();
    await redis.command(["DEL", CACHE_KEY]).catch(() => {});
    redis.close();
    dbms.close();
  }
}

main().catch((error) => {
  process.stderr.write(`${error.stack || error}\n`);
  process.exitCode = 1;
});
