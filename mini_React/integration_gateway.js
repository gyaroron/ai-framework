"use strict";

const http = require("node:http");
const net = require("node:net");

const CACHE_KEY = "server:ip";
// SELECT 결과를 Mini Redis에 넣어 둘 때 쓰는 key 앞머리.
const SQL_CACHE_PREFIX = "sql:";
// SELECT 결과를 cache에 두는 시간(초). 이 시간이 지나면 Mini Redis가 스스로 지운다(TTL).
const DEFAULT_SQL_CACHE_TTL_SECONDS = 30;
// 이보다 큰 결과는 cache에 넣지 않는다. Mini Redis 서버가 요청 하나를 64KiB까지만 받기 때문이다.
const SQL_CACHE_MAX_BYTES = 32 * 1024;
// 요청 본문으로 받을 SQL 문장의 최대 길이(byte).
const SQL_STATEMENT_MAX_BYTES = 8 * 1024;

function encodeCommand(args) {
  const parts = [Buffer.from(`*${args.length}\r\n`, "ascii")];
  for (const arg of args) {
    const value = Buffer.from(String(arg), "utf8");
    parts.push(Buffer.from(`$${value.length}\r\n`, "ascii"), value,
      Buffer.from("\r\n", "ascii"));
  }
  return Buffer.concat(parts);
}

function findCrlf(buffer, offset) {
  for (let i = offset; i + 1 < buffer.length; i += 1) {
    if (buffer[i] === 13 && buffer[i + 1] === 10) return i;
  }
  return -1;
}

function decodeReply(buffer) {
  if (buffer.length === 0) return null;
  const marker = String.fromCharCode(buffer[0]);
  const lineEnd = findCrlf(buffer, 1);
  if (lineEnd < 0) return null;
  const line = buffer.toString("utf8", 1, lineEnd);
  const consumedLine = lineEnd + 2;

  if (marker === "+") return { value: line, consumed: consumedLine };
  if (marker === "-") {
    const error = new Error(line);
    error.respError = true;
    throw error;
  }
  if (marker === ":") {
    const value = Number(line);
    if (!Number.isSafeInteger(value)) throw new Error("Invalid RESP integer");
    return { value, consumed: consumedLine };
  }
  if (marker === "$") {
    const length = Number(line);
    if (!Number.isInteger(length) || length < -1) {
      throw new Error("Invalid RESP bulk length");
    }
    if (length === -1) return { value: null, consumed: consumedLine };
    const start = consumedLine;
    const end = start + length;
    if (buffer.length < end + 2) return null;
    if (buffer[end] !== 13 || buffer[end + 1] !== 10) {
      throw new Error("Invalid RESP bulk terminator");
    }
    return { value: buffer.toString("utf8", start, end), consumed: end + 2 };
  }
  throw new Error(`Unsupported RESP reply marker: ${marker}`);
}

function createRespClient(options = {}) {
  const host = options.host || "127.0.0.1";
  const port = options.port;
  const timeoutMs = options.timeoutMs === undefined ? 2000 : options.timeoutMs;

  function command(args) {
    return new Promise((resolve, reject) => {
      const socket = net.createConnection({ host, port });
      let response = Buffer.alloc(0);
      let settled = false;
      const finish = (error, value) => {
        if (settled) return;
        settled = true;
        socket.destroy();
        if (error) reject(error);
        else resolve(value);
      };

      socket.setTimeout(timeoutMs, () => {
        socket.destroy(new Error(`RESP ${host}:${port} timed out`));
      });
      socket.once("connect", () => socket.write(encodeCommand(args)));
      socket.on("data", (chunk) => {
        response = Buffer.concat([response, chunk]);
        try {
          const decoded = decodeReply(response);
          if (decoded !== null) finish(null, decoded.value);
        } catch (error) {
          finish(error);
        }
      });
      socket.once("error", (error) => finish(error));
      socket.once("end", () => {
        if (!settled) finish(new Error("RESP peer closed before replying"));
      });
    });
  }

  return {
    command,
    ping: () => command(["PING"]),
    get: (key) => command(["GET", key]),
    set: (key, value) => command(["SET", key, value]),
    // 보관 기간(초)을 함께 주는 SET. 기간이 지나면 Mini Redis가 값을 지운다.
    setWithTtl: (key, value, seconds) => command(["SET", key, value, "EX", String(seconds)]),
    // Mini DBMS SQL 서버에 SQL 문장 하나를 보낸다.
    sql: (statement) => command(["SQL", statement]),
    del: (key) => command(["DEL", key]),
  };
}

function sendJson(response, statusCode, value) {
  const body = Buffer.from(JSON.stringify(value));
  response.writeHead(statusCode, {
    "content-type": "application/json; charset=utf-8",
    "content-length": body.length,
    "access-control-allow-origin": "*",
    "access-control-allow-methods": "GET, PUT, POST, DELETE, OPTIONS",
    "access-control-allow-headers": "content-type",
    "cache-control": "no-store",
  });
  response.end(body);
}

function readJson(request, limit = 16 * 1024) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let length = 0;
    request.on("data", (chunk) => {
      length += chunk.length;
      if (length > limit) {
        reject(Object.assign(new Error("Request body is too large"), { statusCode: 413 }));
        request.destroy();
        return;
      }
      chunks.push(chunk);
    });
    request.on("end", () => {
      try {
        resolve(JSON.parse(Buffer.concat(chunks).toString("utf8")));
      } catch (_error) {
        reject(Object.assign(new Error("Request body must be valid JSON"), {
          statusCode: 400,
        }));
      }
    });
    request.on("error", reject);
  });
}

// SQL 문장을 "읽기(SELECT) / 쓰기(INSERT) / 그 밖"으로 나누고 대상 테이블 이름을 뽑는다.
// 정확한 문법 검사는 Mini DBMS의 parser가 한다. 여기서는 cache를 어떻게 다룰지만 정한다.
function classifySql(statement) {
  const select = /^\s*select\b[\s\S]*?\bfrom\s+([A-Za-z0-9_]+)/i.exec(statement);
  if (select !== null) return { kind: "read", table: select[1].toLowerCase() };
  const insert = /^\s*insert\s+into\s+([A-Za-z0-9_]+)/i.exec(statement);
  if (insert !== null) return { kind: "write", table: insert[1].toLowerCase() };
  return { kind: "other", table: null };
}

// 같은 뜻의 문장이 같은 cache key를 쓰도록 앞뒤 공백과 끝의 ';'을 떼어 낸다.
function sqlCacheKey(statement) {
  return SQL_CACHE_PREFIX + statement.trim().replace(/;\s*$/, "").trim();
}

/*
 * POST /api/sql 처리.
 *   SELECT: Mini Redis에 결과가 있으면 그것으로 답한다(source: "cache").
 *           없으면 Mini DBMS에 물어보고, 결과를 TTL과 함께 Mini Redis에 넣는다(source: "database").
 *   INSERT: Mini DBMS에 먼저 쓴 뒤, 그 테이블의 SELECT 결과 cache를 모두 지운다(무효화).
 *           지우지 못한 cache도 TTL이 지나면 사라지므로 낡은 결과가 영원히 남지 않는다.
 */
async function handleSql(context, statement) {
  const { redis, sqlServer, cachedKeysByTable, cacheTtlSeconds } = context;
  const { kind, table } = classifySql(statement);

  if (kind === "read") {
    const key = sqlCacheKey(statement);
    let isCacheAvailable = true;
    try {
      const cached = await redis.get(key);
      if (cached !== null) return { result: cached, source: "cache" };
    } catch (_cacheError) {
      isCacheAvailable = false;
    }
    const result = await sqlServer.sql(statement);
    let isCached = false;
    if (isCacheAvailable && Buffer.byteLength(key) + Buffer.byteLength(result)
        <= SQL_CACHE_MAX_BYTES) {
      try {
        await redis.setWithTtl(key, result, cacheTtlSeconds);
        if (!cachedKeysByTable.has(table)) cachedKeysByTable.set(table, new Set());
        cachedKeysByTable.get(table).add(key);
        isCached = true;
      } catch (_cacheError) {
        isCacheAvailable = false;
      }
    }
    return {
      result,
      source: isCacheAvailable ? "database" : "database_cache_unavailable",
      cached: isCached,
      ttlSeconds: isCached ? cacheTtlSeconds : null,
    };
  }

  const result = await sqlServer.sql(statement);
  let invalidated = 0;
  if (kind === "write") {
    const keys = cachedKeysByTable.get(table) || new Set();
    cachedKeysByTable.delete(table);
    for (const key of keys) {
      try {
        invalidated += await redis.del(key);
      } catch (_cacheError) {
        // cache를 지우지 못해도 쓰기는 이미 끝났다. 남은 cache는 TTL이 지나면 사라진다.
      }
    }
  }
  return { result, source: "database", invalidated };
}

async function warmCache(redis, dbms) {
  const value = await dbms.get(CACHE_KEY);
  if (value === null) {
    await redis.del(CACHE_KEY);
    return { warmed: false, value: null };
  }
  await redis.set(CACHE_KEY, value);
  return { warmed: true, value };
}

function createGatewayServer(options = {}) {
  const redis = options.redis || createRespClient({
    host: options.redisHost,
    port: options.redisPort === undefined ? 6379 : options.redisPort,
    timeoutMs: options.timeoutMs,
  });
  const dbms = options.dbms || createRespClient({
    host: options.dbmsHost,
    port: options.dbmsPort === undefined ? 6380 : options.dbmsPort,
    timeoutMs: options.timeoutMs,
  });

  // Mini DBMS의 SQL API 서버(thread pool 서버, 기본 port 6381).
  const sqlServer = options.sqlServer || createRespClient({
    host: options.sqlHost,
    port: options.sqlPort === undefined ? 6381 : options.sqlPort,
    timeoutMs: options.timeoutMs,
  });
  const sqlContext = {
    redis,
    sqlServer,
    // 테이블 이름 → 그 테이블의 SELECT 결과를 담고 있는 cache key들. INSERT 때 지울 대상이다.
    cachedKeysByTable: new Map(),
    cacheTtlSeconds: options.sqlCacheTtlSeconds === undefined
      ? DEFAULT_SQL_CACHE_TTL_SECONDS : options.sqlCacheTtlSeconds,
  };

  const server = http.createServer(async (request, response) => {
    if (request.method === "OPTIONS") {
      response.writeHead(204, {
        "access-control-allow-origin": "*",
        "access-control-allow-methods": "GET, PUT, POST, DELETE, OPTIONS",
        "access-control-allow-headers": "content-type",
      });
      response.end();
      return;
    }

    const url = new URL(request.url, "http://localhost");
    if (url.pathname === "/health" && request.method === "GET") {
      try {
        await Promise.all([redis.ping(), dbms.ping()]);
        sendJson(response, 200, { ok: true });
      } catch (error) {
        sendJson(response, 503, { error: "dependency_unavailable", message: error.message });
      }
      return;
    }

    if (url.pathname === "/api/sql") {
      if (request.method !== "POST") {
        response.setHeader("allow", "POST, OPTIONS");
        sendJson(response, 405, { error: "method_not_allowed" });
        return;
      }
      try {
        const body = await readJson(request);
        if (typeof body.sql !== "string" || body.sql.trim() === ""
            || Buffer.byteLength(body.sql) > SQL_STATEMENT_MAX_BYTES) {
          sendJson(response, 400, { error: "invalid_sql", message: "sql must be a non-empty string" });
          return;
        }
        sendJson(response, 200, await handleSql(sqlContext, body.sql));
      } catch (error) {
        // Mini DBMS가 "-ERR ..."로 답한 것은 문장이 틀린 것이므로 400, 그 밖은 서버 쪽 문제다.
        const statusCode = error.respError ? 400 : (error.statusCode || 502);
        sendJson(response, statusCode, {
          error: error.respError ? "sql_error"
            : (statusCode === 400 ? "bad_request" : "upstream_failure"),
          message: error.message.replace(/^ERR\s+/, ""),
        });
      }
      return;
    }

    if (url.pathname !== "/api/server-ip") {
      sendJson(response, 404, { error: "not_found" });
      return;
    }

    try {
      if (request.method === "GET") {
        let cacheAvailable = true;
        try {
          const cached = await redis.get(CACHE_KEY);
          if (cached !== null) {
            sendJson(response, 200, { value: cached, source: "cache" });
            return;
          }
        } catch (_cacheError) {
          cacheAvailable = false;
        }

        const value = await dbms.get(CACHE_KEY);
        if (cacheAvailable) {
          if (value === null) await redis.del(CACHE_KEY);
          else await redis.set(CACHE_KEY, value);
        }
        sendJson(response, 200, {
          value,
          source: cacheAvailable ? "database" : "database_cache_unavailable",
        });
        return;
      }

      if (request.method === "PUT") {
        const body = await readJson(request);
        if (typeof body.value !== "string" || net.isIP(body.value) !== 4) {
          sendJson(response, 400, { error: "invalid_ipv4" });
          return;
        }
        await dbms.set(CACHE_KEY, body.value);
        try {
          await redis.set(CACHE_KEY, body.value);
        } catch (error) {
          sendJson(response, 503, {
            error: "cache_update_failed",
            persisted: true,
            message: error.message,
          });
          return;
        }
        sendJson(response, 200, { value: body.value, persisted: true, cached: true });
        return;
      }

      if (request.method === "DELETE") {
        await dbms.del(CACHE_KEY);
        try {
          await redis.del(CACHE_KEY);
        } catch (error) {
          sendJson(response, 503, {
            error: "cache_invalidation_failed",
            persisted: true,
            message: error.message,
          });
          return;
        }
        sendJson(response, 200, { deleted: true, persisted: true, cacheInvalidated: true });
        return;
      }

      response.setHeader("allow", "GET, PUT, DELETE, OPTIONS");
      sendJson(response, 405, { error: "method_not_allowed" });
    } catch (error) {
      const statusCode = error.statusCode || 502;
      sendJson(response, statusCode, {
        error: statusCode === 400 ? "bad_request" : "upstream_failure",
        message: error.message,
      });
    }
  });

  return {
    server,
    redis,
    dbms,
    sqlServer,
    warmCache: () => warmCache(redis, dbms),
  };
}

function listen(server, port, host) {
  return new Promise((resolve, reject) => {
    const onError = (error) => {
      server.off("listening", onListening);
      reject(error);
    };
    const onListening = () => {
      server.off("error", onError);
      resolve(server.address());
    };
    server.once("error", onError);
    server.once("listening", onListening);
    server.listen(port, host);
  });
}

async function startGateway(options = {}) {
  const gateway = createGatewayServer(options);
  if (options.warmOnStart !== false) await gateway.warmCache();
  const port = options.port === undefined ? 3000 : options.port;
  const host = options.host || "0.0.0.0";
  const address = await listen(gateway.server, port, host);
  return {
    ...gateway,
    address,
    close: () => new Promise((resolve, reject) => {
      gateway.server.close((error) => error ? reject(error) : resolve());
    }),
  };
}

if (require.main === module) {
  startGateway({
    port: Number(process.env.PORT || 3000),
    redisHost: process.env.REDIS_HOST || "127.0.0.1",
    redisPort: Number(process.env.REDIS_PORT || 6379),
    dbmsHost: process.env.DBMS_HOST || "127.0.0.1",
    dbmsPort: Number(process.env.DBMS_PORT || 6380),
    sqlHost: process.env.SQL_HOST || process.env.DBMS_HOST || "127.0.0.1",
    sqlPort: Number(process.env.SQL_PORT || 6381),
    sqlCacheTtlSeconds: Number(process.env.SQL_CACHE_TTL_SECONDS
      || DEFAULT_SQL_CACHE_TTL_SECONDS),
  }).then(({ address }) => {
    console.log(`Mini integration gateway listening on ${address.address}:${address.port}`);
  }).catch((error) => {
    console.error(`Gateway startup failed: ${error.message}`);
    process.exitCode = 1;
  });
}

module.exports = { CACHE_KEY, SQL_CACHE_PREFIX, encodeCommand, decodeReply, createRespClient,
  createGatewayServer, warmCache, startGateway, classifySql, sqlCacheKey };
