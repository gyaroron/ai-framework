"use strict";

/*
 * 실행 중인 Mini Redis 서버(./mini_redis --server)에 실제로 접속해 보는 기능 테스트.
 * 저장 · 조회 · 삭제, 보관 기간(TTL) 만료, 잘못된 인자 거절을 확인한다.
 * 실행: node test_server_ttl.js            (127.0.0.1:6379)
 *       REDIS_PORT=46379 node test_server_ttl.js
 */
const assert = require("node:assert/strict");
const { connect } = require("./resp_client");

const HOST = process.env.REDIS_HOST || "127.0.0.1";
const PORT = Number(process.env.REDIS_PORT || 6379);
const SHORT_TTL_MS = 300;
/* 서버는 만료된 key를 1초에 한 번 청소한다. 그보다 조금 더 기다린다. */
const SWEEP_WAIT_MS = 1500;

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function report(message) {
  process.stdout.write(`${message}\n`);
}

async function rejectsWith(promise, pattern) {
  await assert.rejects(promise, (error) => {
    assert.equal(error.isRespError, true);
    assert.match(error.message, pattern);
    return true;
  });
}

async function main() {
  const client = await connect({ host: HOST, port: PORT });
  const key = `test:ttl:${process.pid}`;
  const run = (...args) => client.command(args);

  try {
    assert.equal(await run("PING"), "PONG");
    await run("DEL", key);

    assert.equal(await run("SET", key, "plain"), "OK");
    assert.equal(await run("GET", key), "plain");
    assert.equal(await run("TTL", key), -1);
    assert.equal(await run("DEL", key), 1);
    assert.equal(await run("GET", key), null);
    assert.equal(await run("TTL", key), -2);
    report("PASS 1: SET, GET, DEL and TTL of a key without a deadline");

    assert.equal(await run("SET", key, "short", "PX", SHORT_TTL_MS), "OK");
    assert.equal(await run("GET", key), "short");
    const remainingMs = await run("PTTL", key);
    assert.ok(remainingMs > 0 && remainingMs <= SHORT_TTL_MS, `PTTL was ${remainingMs}`);
    assert.equal(await run("TTL", key), 1);
    await sleep(SHORT_TTL_MS + 100);
    assert.equal(await run("GET", key), null);
    assert.equal(await run("TTL", key), -2);
    assert.equal(await run("DEL", key), 0);
    report("PASS 2: a value requested after its deadline is answered as missing");

    assert.equal(await run("EXPIRE", key, 10), 0);
    assert.equal(await run("SET", key, "kept"), "OK");
    assert.equal(await run("EXPIRE", key, 10), 1);
    assert.equal(await run("TTL", key), 10);
    assert.equal(await run("PERSIST", key), 1);
    assert.equal(await run("PERSIST", key), 0);
    assert.equal(await run("TTL", key), -1);
    assert.equal(await run("PEXPIRE", key, SHORT_TTL_MS), 1);
    assert.equal(await run("SET", key, "rewritten"), "OK");
    assert.equal(await run("TTL", key), -1);
    await sleep(SHORT_TTL_MS + 100);
    assert.equal(await run("GET", key), "rewritten");
    report("PASS 3: EXPIRE, PERSIST and a plain SET change the deadline");

    assert.equal(await run("SET", key, "seconds", "EX", 1), "OK");
    assert.equal(await run("GET", key), "seconds");
    await sleep(SWEEP_WAIT_MS);
    assert.equal(await run("GET", key), null);
    report("PASS 4: SET ... EX 1 expires after one second");

    await rejectsWith(run("SET", key, "v", "EX", 0), /invalid expire time/);
    await rejectsWith(run("SET", key, "v", "EX", "abc"), /invalid expire time/);
    await rejectsWith(run("SET", key, "v", "PX", "-5"), /invalid expire time/);
    await rejectsWith(run("SET", key, "v", "XX", 5), /syntax error/);
    await rejectsWith(run("SET", key, "v", "EX"), /wrong number of arguments/);
    await rejectsWith(run("EXPIRE", key), /wrong number of arguments/);
    await rejectsWith(run("EXPIRE", key, "1.5"), /invalid expire time/);
    await rejectsWith(run("TTL"), /wrong number of arguments/);
    assert.equal(await run("GET", key), null);
    report("PASS 5: wrong TTL arguments are rejected and store nothing");
  } finally {
    await run("DEL", key).catch(() => {});
    client.close();
  }
  report("All Mini Redis server TTL tests passed.");
}

main().catch((error) => {
  process.stderr.write(`${error.stack || error}\n`);
  process.exitCode = 1;
});
