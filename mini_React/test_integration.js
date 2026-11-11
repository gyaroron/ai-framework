"use strict";

/*
 * End-to-end gateway/UI checks use small RESP-compatible service fixtures so
 * `node mini_react/test_integration.js` needs no browser, compiler, or Docker.
 * They validate the wire protocol and orchestration; they do not claim to run
 * the compiled C daemons or exercise a real on-disk WAL.
 */
const assert = require("node:assert/strict");
const net = require("node:net");
const { createElement } = require("./element");
const { render } = require("./render");
const { App, CACHE_KEY, parseSqlResult } = require("./app");
const { createRespClient, startGateway, sqlCacheKey } = require("./integration_gateway");

function findCrlf(buffer, offset) {
  for (let i = offset; i + 1 < buffer.length; i += 1) {
    if (buffer[i] === 13 && buffer[i + 1] === 10) return i;
  }
  return -1;
}

function parseCommand(buffer) {
  if (buffer.length === 0 || buffer[0] !== 42) return null;
  const countEnd = findCrlf(buffer, 1);
  if (countEnd < 0) return null;
  const count = Number(buffer.toString("ascii", 1, countEnd));
  if (!Number.isInteger(count) || count < 1 || count > 16) {
    throw new Error("Bad request array length");
  }
  let cursor = countEnd + 2;
  const args = [];
  for (let i = 0; i < count; i += 1) {
    if (buffer[cursor] !== 36) throw new Error("Expected RESP bulk string");
    const lengthEnd = findCrlf(buffer, cursor + 1);
    if (lengthEnd < 0) return null;
    const length = Number(buffer.toString("ascii", cursor + 1, lengthEnd));
    if (!Number.isInteger(length) || length < 0) throw new Error("Bad bulk length");
    const start = lengthEnd + 2;
    const end = start + length;
    if (buffer.length < end + 2) return null;
    if (buffer[end] !== 13 || buffer[end + 1] !== 10) {
      throw new Error("Bad bulk terminator");
    }
    args.push(buffer.toString("utf8", start, end));
    cursor = end + 2;
  }
  return { args, consumed: cursor };
}

function bulkReply(value) {
  if (value === null) return Buffer.from("$-1\r\n");
  const body = Buffer.from(String(value), "utf8");
  return Buffer.concat([
    Buffer.from(`$${body.length}\r\n`, "ascii"), body, Buffer.from("\r\n"),
  ]);
}

// Mini DBMS SQL 서버를 흉내 내는 아주 작은 users 테이블(id, name, email, age).
// 실제 엔진과 같은 모양의 결과 글자를 돌려준다. 모르는 문장에는 오류를 던진다.
function createFakeUsersTable() {
  const rows = [];
  return function executeSql(statement) {
    const text = statement.trim();
    const insert = /^insert into users values \('([^']*)', '([^']*)', (\d+)\);?$/i.exec(text);
    if (insert !== null) {
      rows.push([String(rows.length + 1), insert[1], insert[2], insert[3]]);
      return `INSERT 1 (id=${rows.length})\n`;
    }
    const select = /^select \* from users where id = (\d+);?$/i.exec(text);
    if (select !== null) {
      const row = rows[Number(select[1]) - 1];
      return "id\tname\temail\tage\n"
        + (row ? `${row.join("\t")}\n(1 row, index lookup)\n` : "(0 rows, index lookup)\n");
    }
    throw new Error("unsupported statement (only INSERT and SELECT)");
  };
}

async function startRespFixture({ port = 0, values, events, name, executeSql }) {
  const store = values || new Map();
  // key → SET ... EX 로 받은 보관 기간(초). TTL이 실제로 전달됐는지 확인하는 데 쓴다.
  const ttls = new Map();
  const counters = { get: 0, set: 0, del: 0, sql: 0 };
  const sockets = new Set();
  const server = net.createServer((socket) => {
    sockets.add(socket);
    socket.on("close", () => sockets.delete(socket));
    let pending = Buffer.alloc(0);
    socket.on("data", (chunk) => {
      pending = Buffer.concat([pending, chunk]);
      try {
        for (;;) {
          const parsed = parseCommand(pending);
          if (parsed === null) break;
          pending = pending.subarray(parsed.consumed);
          const [verb, key, value] = parsed.args;
          let reply;
          if (verb.toUpperCase() === "PING") {
            reply = Buffer.from("+PONG\r\n");
          } else if (verb.toUpperCase() === "GET" && parsed.args.length === 2) {
            counters.get += 1;
            events.push(`${name}:GET:${key}`);
            reply = bulkReply(store.has(key) ? store.get(key) : null);
          } else if (verb.toUpperCase() === "SET" && parsed.args.length === 3) {
            counters.set += 1;
            events.push(`${name}:SET:${key}:${value}`);
            store.set(key, value);
            ttls.delete(key);
            reply = Buffer.from("+OK\r\n");
          } else if (verb.toUpperCase() === "SET" && parsed.args.length === 5
              && parsed.args[3].toUpperCase() === "EX") {
            counters.set += 1;
            events.push(`${name}:SETEX:${key}:${parsed.args[4]}`);
            store.set(key, value);
            ttls.set(key, Number(parsed.args[4]));
            reply = Buffer.from("+OK\r\n");
          } else if (verb.toUpperCase() === "SQL" && parsed.args.length === 2
              && typeof executeSql === "function") {
            counters.sql += 1;
            events.push(`${name}:SQL:${key}`);
            try {
              reply = bulkReply(executeSql(key));
            } catch (error) {
              reply = Buffer.from(`-ERR ${error.message}\r\n`);
            }
          } else if (verb.toUpperCase() === "DEL" && parsed.args.length === 2) {
            counters.del += 1;
            events.push(`${name}:DEL:${key}`);
            reply = Buffer.from(`:${store.delete(key) ? 1 : 0}\r\n`);
          } else {
            reply = Buffer.from("-ERR unsupported test command\r\n");
          }
          socket.write(reply);
        }
      } catch (error) {
        socket.destroy(error);
      }
    });
  });

  await new Promise((resolve, reject) => {
    server.once("error", reject);
    server.listen(port, "127.0.0.1", () => {
      server.off("error", reject);
      resolve();
    });
  });
  return {
    store,
    ttls,
    counters,
    port: server.address().port,
    close: () => new Promise((resolve, reject) => {
      for (const socket of sockets) socket.destroy();
      server.close((error) => error ? reject(error) : resolve());
    }),
  };
}

class MockNode {
  constructor(type, nodeType = 1) {
    this.type = type;
    this.nodeType = nodeType;
    this.children = [];
    this.nodeValue = "";
    this.listeners = new Map();
    this.props = {};
    this.value = "";
  }

  appendChild(node) { this.children.push(node); return node; }
  removeChild(node) {
    const index = this.children.indexOf(node);
    assert.notEqual(index, -1);
    this.children.splice(index, 1);
    return node;
  }
  replaceChild(next, previous) {
    const index = this.children.indexOf(previous);
    assert.notEqual(index, -1);
    this.children[index] = next;
    return previous;
  }
  addEventListener(type, callback) {
    if (!this.listeners.has(type)) this.listeners.set(type, new Set());
    this.listeners.get(type).add(callback);
  }
  removeEventListener(type, callback) { this.listeners.get(type)?.delete(callback); }
  dispatch(type, event) {
    for (const callback of this.listeners.get(type) || []) callback(event);
  }
  set id(value) { this.props.id = value; }
  get id() { return this.props.id || ""; }
  set className(value) { this.props.className = value; }
  get className() { return this.props.className || ""; }
  get textContent() {
    return this.nodeType === 3
      ? this.nodeValue
      : this.children.map((child) => child.textContent).join("");
  }
}

global.document = {
  createElement(type) { return new MockNode(type); },
  createTextNode(text) {
    const node = new MockNode("#text", 3);
    node.nodeValue = text;
    return node;
  },
  createDocumentFragment() { return new MockNode("#fragment", 11); },
};

function findNode(root, type, id) {
  if (root.type === type && (id === undefined || root.id === id)) return root;
  for (const child of root.children || []) {
    const found = findNode(child, type, id);
    if (found) return found;
  }
  return null;
}

async function waitFor(predicate, label) {
  const deadline = Date.now() + 3000;
  while (Date.now() < deadline) {
    if (predicate()) return;
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
  assert.fail(`Timed out waiting for ${label}`);
}

async function httpJson(baseUrl, path, init) {
  const response = await fetch(`${baseUrl}${path}`, init);
  const body = await response.json();
  return { response, body };
}

async function run() {
  const events = [];
  const durableValues = new Map([[CACHE_KEY, "192.168.0.1"]]);
  let redisService;
  let dbmsService;
  let sqlService;
  let gateway;

  try {
    redisService = await startRespFixture({ values: new Map(), events, name: "redis" });
    dbmsService = await startRespFixture({
      values: durableValues, events, name: "dbms",
    });
    sqlService = await startRespFixture({
      events, name: "sql", executeSql: createFakeUsersTable(),
    });
    const gatewayOptions = {
      host: "127.0.0.1",
      port: 0,
      sqlHost: "127.0.0.1",
      sqlPort: sqlService.port,
      sqlCacheTtlSeconds: 30,
      redisHost: "127.0.0.1",
      redisPort: redisService.port,
      dbmsHost: "127.0.0.1",
      dbmsPort: dbmsService.port,
      warmOnStart: false,
    };
    gateway = await startGateway(gatewayOptions);
    const baseUrl = `http://127.0.0.1:${gateway.address.port}`;
    const redis = createRespClient({ host: "127.0.0.1", port: redisService.port });
    const dbms = createRespClient({ host: "127.0.0.1", port: dbmsService.port });

    // 1. A cache miss reads DBMS and warms Redis.
    assert.equal(redisService.store.has(CACHE_KEY), false);
    const miss = await httpJson(baseUrl, "/api/server-ip");
    assert.equal(miss.response.status, 200);
    assert.deepEqual(miss.body, { value: "192.168.0.1", source: "database" });
    assert.equal(await redis.get(CACHE_KEY), "192.168.0.1");
    console.log("PASS 1: cache miss read Mini DBMS and warmed Mini Redis");

    // 2. A second read is served from Redis without another DBMS read.
    const dbmsReadsBeforeHit = dbmsService.counters.get;
    const hit = await httpJson(baseUrl, "/api/server-ip");
    assert.deepEqual(hit.body, { value: "192.168.0.1", source: "cache" });
    assert.equal(dbmsService.counters.get, dbmsReadsBeforeHit);
    console.log("PASS 2: cache hit returned the value without a DBMS read");

    // 3. The gateway commits to DBMS before refreshing the Redis cache.
    const previousSetEventCount = events.length;
    const update = await httpJson(baseUrl, "/api/server-ip", {
      method: "PUT",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ value: "10.0.0.1" }),
    });
    assert.equal(update.response.status, 200);
    assert.deepEqual(update.body, {
      value: "10.0.0.1", persisted: true, cached: true,
    });
    assert.equal(await dbms.get(CACHE_KEY), "10.0.0.1");
    assert.equal(await redis.get(CACHE_KEY), "10.0.0.1");
    const writeEvents = events.slice(previousSetEventCount)
      .filter((event) => event.includes(":SET:"));
    assert.deepEqual(writeEvents.slice(0, 2), [
      "dbms:SET:server:ip:10.0.0.1",
      "redis:SET:server:ip:10.0.0.1",
    ]);
    console.log("PASS 3: write-through committed DBMS before updating Redis");

    // 4. Delete records absence in DBMS and invalidates the cache.
    const deletion = await httpJson(baseUrl, "/api/server-ip", { method: "DELETE" });
    assert.equal(deletion.response.status, 200);
    assert.equal(deletion.body.persisted, true);
    assert.equal(redisService.store.has(CACHE_KEY), false);
    assert.equal(durableValues.has(CACHE_KEY), false);
    const deletedRead = await httpJson(baseUrl, "/api/server-ip");
    assert.deepEqual(deletedRead.body, { value: null, source: "database" });
    console.log("PASS 4: delete removed the DBMS value and invalidated Redis");

    // 5. Restart DBMS with its persistent store, clear cache, then warm on gateway startup.
    const recoveryWrite = await httpJson(baseUrl, "/api/server-ip", {
      method: "PUT",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ value: "172.16.0.1" }),
    });
    assert.equal(recoveryWrite.response.status, 200);
    await gateway.close();
    gateway = null;
    await dbmsService.close();
    dbmsService = await startRespFixture({
      port: dbmsService.port,
      values: durableValues,
      events,
      name: "dbms-restarted",
    });
    await redis.del(CACHE_KEY);
    gateway = await startGateway({ ...gatewayOptions, dbmsPort: dbmsService.port,
      warmOnStart: true });
    const warmedValue = await redis.get(CACHE_KEY);
    assert.equal(warmedValue, "172.16.0.1");
    console.log("PASS 5: gateway startup warmed Redis from the restarted persistent DBMS fixture");

    // 6. App loads and updates through HTTP; the existing DOM nodes are patched.
    const uiContainer = new MockNode("#container");
    const apiBase = `http://127.0.0.1:${gateway.address.port}/api`;
    const root = render(createElement(App, { apiBase, fetch: globalThis.fetch }), uiContainer);
    await waitFor(() => findNode(root, "p", "server-ip-value")?.textContent
      === "현재 저장 값: 172.16.0.1", "initial UI fetch");
    const valueNode = findNode(root, "p", "server-ip-value");
    const inputNode = findNode(root, "input", "server-ip-input");
    const buttons = [];
    (function collect(node) {
      if (node.type === "button") buttons.push(node);
      for (const child of node.children || []) collect(child);
    }(root));
    assert.ok(inputNode);
    // 저장 · 삭제 · SQL 실행, 세 개다.
    assert.equal(buttons.length, 3);
    inputNode.dispatch("input", { target: { value: "203.0.113.8" } });
    buttons[0].dispatch("click", {});
    await waitFor(() => valueNode.textContent === "현재 저장 값: 203.0.113.8",
      "UI write-through update");
    assert.equal(findNode(root, "p", "server-ip-value"), valueNode);
    assert.equal(await redis.get(CACHE_KEY), "203.0.113.8");
    assert.equal(await dbms.get(CACHE_KEY), "203.0.113.8");
    console.log("PASS 6: Mini React fetched and updated through HTTP with DOM node reuse");

    // 7. SQL 읽기: 첫 SELECT는 Mini DBMS에서 읽어 TTL과 함께 cache에 넣고, 두 번째는 cache에서 읽는다.
    // 5번에서 gateway를 다시 띄웠으므로 지금 떠 있는 gateway의 주소를 쓴다.
    const liveUrl = `http://127.0.0.1:${gateway.address.port}`;
    const postSql = (sql) => httpJson(liveUrl, "/api/sql", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ sql }),
    });
    const insertKim = await postSql("INSERT INTO users VALUES ('Kim', 'kim@example.com', 30);");
    assert.equal(insertKim.response.status, 200);
    assert.deepEqual(insertKim.body,
      { result: "INSERT 1 (id=1)\n", source: "database", invalidated: 0 });
    const selectSql = "SELECT * FROM users WHERE id = 1;";
    const firstSelect = await postSql(selectSql);
    assert.equal(firstSelect.response.status, 200);
    assert.equal(firstSelect.body.source, "database");
    assert.equal(firstSelect.body.cached, true);
    assert.equal(firstSelect.body.ttlSeconds, 30);
    assert.match(firstSelect.body.result, /^id\tname\temail\tage\n1\tKim\t/);
    assert.equal(redisService.store.get(sqlCacheKey(selectSql)), firstSelect.body.result);
    assert.equal(redisService.ttls.get(sqlCacheKey(selectSql)), 30);
    const sqlCallsBeforeHit = sqlService.counters.sql;
    const secondSelect = await postSql(`  ${selectSql}  `);
    assert.deepEqual(secondSelect.body, { result: firstSelect.body.result, source: "cache" });
    assert.equal(sqlService.counters.sql, sqlCallsBeforeHit);
    console.log("PASS 7: SELECT was cached in Mini Redis with a TTL and then served from cache");

    // 8. SQL 쓰기: INSERT는 Mini DBMS에 쓴 뒤 그 테이블의 SELECT cache를 지운다.
    const insertLee = await postSql("INSERT INTO users VALUES ('Lee', 'lee@example.com', 25);");
    assert.deepEqual(insertLee.body,
      { result: "INSERT 1 (id=2)\n", source: "database", invalidated: 1 });
    assert.equal(redisService.store.has(sqlCacheKey(selectSql)), false);
    assert.equal((await postSql(selectSql)).body.source, "database");
    const badSql = await postSql("DROP TABLE users;");
    assert.equal(badSql.response.status, 400);
    assert.equal(badSql.body.error, "sql_error");
    assert.match(badSql.body.message, /unsupported statement/);
    assert.equal((await postSql("   ")).response.status, 400);
    assert.equal((await httpJson(liveUrl, "/api/sql")).response.status, 405);
    console.log("PASS 8: INSERT invalidated the cached SELECT; bad requests were rejected");

    // 9. 화면: SQL 입력 → 실행 버튼 → 결과 표. 결과 글자는 useMemo(parseSqlResult)를 거친다.
    assert.deepEqual(parseSqlResult("id\tname\n1\tKim\n(1 row, index lookup)\n"),
      { columns: ["id", "name"], rows: [["1", "Kim"]], summary: "(1 row, index lookup)" });
    assert.deepEqual(parseSqlResult("INSERT 1 (id=3)\n"),
      { columns: [], rows: [], summary: "INSERT 1 (id=3)" });
    assert.deepEqual(parseSqlResult(""), { columns: [], rows: [], summary: "" });
    const sqlInputNode = findNode(root, "input", "sql-input");
    assert.ok(sqlInputNode);
    sqlInputNode.dispatch("input", { target: { value: "SELECT * FROM users WHERE id = 2;" } });
    buttons[2].dispatch("click", {});
    await waitFor(() => findNode(root, "div", "sql-result") !== null, "SQL result table");
    const cells = [];
    (function collectCells(node) {
      if (node.type === "td") cells.push(node.textContent);
      for (const child of node.children || []) collectCells(child);
    }(findNode(root, "div", "sql-result")));
    assert.deepEqual(cells, ["2", "Lee", "lee@example.com", "25"]);
    assert.equal(findNode(root, "p", "sql-summary").textContent, "(1 row, index lookup)");
    assert.match(findNode(root, "p", "sql-source").textContent, /30초 동안 캐시/);
    // 같은 문장을 다시 실행하면 cache에서 왔다고 표시한다.
    buttons[2].dispatch("click", {});
    await waitFor(() => /캐시에서 읽었습니다/.test(findNode(root, "p", "sql-source").textContent),
      "SQL cache hit label");
    assert.equal(findNode(root, "p", "server-ip-value"), valueNode);
    console.log("PASS 9: Mini React ran SQL through the gateway and rendered the result table");
    console.log("E2E mode: RESP-compatible in-process Redis/DBMS fixtures (C daemons/WAL not started)");
    console.log("All 9 integration scenarios passed.");
  } finally {
    if (gateway !== null) await gateway.close();
    if (sqlService !== undefined) await sqlService.close();
    if (dbmsService !== undefined) await dbmsService.close();
    if (redisService !== undefined) await redisService.close();
  }
}

run().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
