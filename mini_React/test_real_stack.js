"use strict";

/*
 * 실제 C 서버 세 개(Mini Redis · Mini DBMS bridge · Mini DBMS SQL 서버)에 gateway를 붙여
 * 한 바퀴를 끝까지 돌려 보는 기능 테스트.
 *
 * 먼저 세 서버를 띄운다.
 *   mini_Redis: ./mini_redis --server [snapshot 파일]          (port 6379)
 *   mini_DBMS:  ./mini_dbms_server                             (port 6380)
 *   mini_DBMS:  ./mini_dbms_sql_server <data_dir> 6381 4       (port 6381, data_dir에 users.schema)
 *
 * 실행: REDIS_PORT=6379 DBMS_PORT=6380 SQL_PORT=6381 node test_real_stack.js
 * 세 환경 변수 가운데 하나라도 없으면 건너뛴다(SKIP).
 */
const assert = require("node:assert/strict");
const { startGateway } = require("./integration_gateway");

const HOST = process.env.STACK_HOST || "127.0.0.1";
// 이 테스트에서 SELECT cache를 두는 시간(초). 만료를 기다려 볼 수 있게 짧게 잡는다.
const CACHE_TTL_SECONDS = 1;
// 한꺼번에 보내는 INSERT 개수. thread pool이 여러 요청을 동시에 처리하는지 본다.
const PARALLEL_INSERTS = 40;

function report(message) {
  process.stdout.write(`${message}\n`);
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function main() {
  if (!process.env.REDIS_PORT || !process.env.DBMS_PORT || !process.env.SQL_PORT) {
    report("SKIP: set REDIS_PORT, DBMS_PORT and SQL_PORT to run against the real servers.");
    return;
  }
  const gateway = await startGateway({
    host: "127.0.0.1",
    port: 0,
    redisHost: HOST,
    redisPort: Number(process.env.REDIS_PORT),
    dbmsHost: HOST,
    dbmsPort: Number(process.env.DBMS_PORT),
    sqlHost: HOST,
    sqlPort: Number(process.env.SQL_PORT),
    sqlCacheTtlSeconds: CACHE_TTL_SECONDS,
    timeoutMs: 10000,
  });
  const baseUrl = `http://127.0.0.1:${gateway.address.port}`;

  async function call(path, init) {
    const response = await fetch(`${baseUrl}${path}`, init);
    return { status: response.status, body: await response.json() };
  }
  const postSql = (sql) => call("/api/sql", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ sql }),
  });

  try {
    assert.deepEqual((await call("/health")).body, { ok: true });

    // 1. server:ip: 저장 → cache에서 읽기 → 삭제.
    const saved = await call("/api/server-ip", {
      method: "PUT",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ value: "10.1.2.3" }),
    });
    assert.deepEqual(saved.body, { value: "10.1.2.3", persisted: true, cached: true });
    assert.deepEqual((await call("/api/server-ip")).body, { value: "10.1.2.3", source: "cache" });
    assert.equal((await call("/api/server-ip", { method: "DELETE" })).status, 200);
    assert.deepEqual((await call("/api/server-ip")).body, { value: null, source: "database" });
    report("PASS 1: server:ip went through Mini DBMS (WAL) and the Mini Redis cache");

    // 2. SQL: INSERT가 id를 자동으로 붙이고, WHERE id = ? 는 인덱스를 쓴다.
    const name = `real_${process.pid}_${Date.now()}`;
    const inserted = await postSql(`INSERT INTO users VALUES ('${name}', 'real@example.com', 31);`);
    assert.equal(inserted.status, 200);
    const idMatch = /^INSERT 1 \(id=(\d+)\)\n$/.exec(inserted.body.result);
    assert.ok(idMatch, `unexpected INSERT result: ${inserted.body.result}`);
    const selectById = `SELECT * FROM users WHERE id = ${idMatch[1]};`;
    const first = await postSql(selectById);
    assert.equal(first.body.source, "database");
    assert.equal(first.body.cached, true);
    assert.ok(first.body.result.includes(`${idMatch[1]}\t${name}\treal@example.com\t31\n`));
    assert.ok(first.body.result.endsWith("(1 row, index lookup)\n"));
    report("PASS 2: INSERT assigned an id and SELECT WHERE id used the B+ tree index");

    // 3. cache: 두 번째는 Mini Redis에서, TTL이 지나면 다시 Mini DBMS에서 읽는다.
    assert.deepEqual((await postSql(selectById)).body,
      { result: first.body.result, source: "cache" });
    await sleep(CACHE_TTL_SECONDS * 1000 + 300);
    assert.equal((await postSql(selectById)).body.source, "database");
    report("PASS 3: cached SELECT expired in Mini Redis after its TTL");

    // 4. 무효화: 같은 테이블에 INSERT하면 아직 살아 있는 SELECT cache가 지워진다.
    assert.equal((await postSql(selectById)).body.source, "cache");
    const second = await postSql(`INSERT INTO users VALUES ('${name}', 'second@example.com', 32);`);
    assert.equal(second.body.invalidated, 1);
    assert.equal((await postSql(selectById)).body.source, "database");
    report("PASS 4: INSERT invalidated the cached SELECT of the same table");

    // 5. 선형 탐색: id가 아닌 컬럼 조건은 파일 전체를 읽는다.
    const scan = await postSql(`SELECT id, email FROM users WHERE name = '${name}';`);
    assert.match(scan.body.result, /\(2 rows, full scan of \d+ rows\)\n$/);
    report("PASS 5: SELECT on another column did a full scan and found both rows");

    // 6. 동시 요청: INSERT 여러 개를 한꺼번에 보내도 하나도 사라지지 않고 id가 겹치지 않는다.
    const bulkName = `bulk_${process.pid}_${Date.now()}`;
    const results = await Promise.all(Array.from({ length: PARALLEL_INSERTS }, (_unused, index) =>
      postSql(`INSERT INTO users VALUES ('${bulkName}', 'bulk@example.com', ${index});`)));
    const ids = new Set(results.map((result) => {
      assert.equal(result.status, 200);
      return /id=(\d+)/.exec(result.body.result)[1];
    }));
    assert.equal(ids.size, PARALLEL_INSERTS);
    const counted = await postSql(`SELECT COUNT(*) FROM users WHERE name = '${bulkName}';`);
    assert.ok(counted.body.result.startsWith(`count\n${PARALLEL_INSERTS}\n`));
    report(`PASS 6: ${PARALLEL_INSERTS} parallel INSERTs kept every row with a unique id`);

    // 7. 잘못된 요청은 400으로 거절한다.
    const dropped = await postSql("DROP TABLE users;");
    assert.equal(dropped.status, 400);
    assert.equal(dropped.body.error, "sql_error");
    const unknown = await postSql("SELECT * FROM no_such_table;");
    assert.equal(unknown.status, 400);
    assert.match(unknown.body.message, /unknown table/);
    report("PASS 7: unsupported SQL and unknown tables were rejected with 400");
    report("All real-stack scenarios passed.");
  } finally {
    await gateway.close();
  }
}

main().catch((error) => {
  process.stderr.write(`${error.stack || error}\n`);
  process.exitCode = 1;
});
