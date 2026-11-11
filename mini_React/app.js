(function () {
"use strict";

const elementApi = typeof module !== "undefined" && module.exports
  ? require("./element")
  : globalThis.MiniReactElement;
const hookApi = typeof module !== "undefined" && module.exports
  ? require("./hook")
  : globalThis.MiniReactHook;
const { createElement } = elementApi;
const { useState, useEffect, useMemo, batch } = hookApi;

const CACHE_KEY = "server:ip";
// SQL 입력 칸에 처음 보여 줄 문장.
const DEFAULT_SQL = "SELECT * FROM users WHERE id = 1;";

/*
 * Mini DBMS가 돌려준 결과 글자를 표로 그릴 수 있는 모양으로 바꾼다.
 *   SELECT 결과: 첫 줄 = 컬럼 이름(tab으로 나눔), 가운데 줄들 = 레코드, 마지막 줄 = "(N rows, ...)" 요약
 *   INSERT 결과: "INSERT 1 (id=N)" 한 줄 → 표 없이 요약만
 * 결과 글자가 길 수 있으므로 App은 이 함수를 useMemo로 감싸, 결과가 바뀔 때만 다시 계산한다.
 */
function parseSqlResult(text) {
  if (typeof text !== "string" || text.trim() === "") {
    return { columns: [], rows: [], summary: "" };
  }
  const lines = text.replace(/\n$/, "").split("\n");
  const lastLine = lines[lines.length - 1];
  const hasSummary = lastLine.startsWith("(");
  if (lines.length === 1 && !hasSummary) {
    return { columns: [], rows: [], summary: lastLine };
  }
  const bodyLines = hasSummary ? lines.slice(1, -1) : lines.slice(1);
  return {
    columns: lines[0].split("\t"),
    rows: bodyLines.map((line) => line.split("\t")),
    summary: hasSummary ? lastLine : "",
  };
}

// 아래 component들은 stateless 자식이다: hook을 부르지 않고, root가 내려 준 props만 읽는다.
function StatusMessage(props) {
  return createElement("p", { id: "cache-status", role: "status" }, props.message);
}

// server:ip 입력 칸과 저장 · 삭제 버튼.
function ServerIpForm(props) {
  return createElement("div", { className: "server-ip-form" },
    createElement("label", { htmlFor: "server-ip-input" }, "server:ip 값"),
    createElement("input", {
      id: "server-ip-input",
      value: props.draft,
      onInput: props.onInput,
    }),
    createElement("button", { type: "button", onClick: props.onSave }, "저장"),
    createElement("button", { type: "button", onClick: props.onDelete }, "삭제"),
  );
}

function ServerIpValue(props) {
  return createElement("p", { id: "server-ip-value" }, `현재 저장 값: ${props.value}`);
}

// SQL 결과 표. props.table은 parseSqlResult가 만든 { columns, rows, summary }다.
function SqlResultTable(props) {
  const { columns, rows, summary } = props.table;
  if (columns.length === 0) {
    return createElement("p", { id: "sql-summary" }, summary);
  }
  return createElement("div", { id: "sql-result" },
    createElement("table", null,
      createElement("thead", null,
        createElement("tr", null,
          ...columns.map((column, columnIndex) =>
            createElement("th", { key: `col-${columnIndex}` }, column)))),
      createElement("tbody", null,
        ...rows.map((row, rowIndex) => createElement("tr", { key: `row-${rowIndex}` },
          ...row.map((cell, cellIndex) =>
            createElement("td", { key: `cell-${cellIndex}` }, cell)))))),
    createElement("p", { id: "sql-summary" }, summary),
  );
}

// SQL 입력 칸 · 실행 버튼 · 결과가 어디서 왔는지(cache / database) · 결과 표.
function SqlPanel(props) {
  return createElement("section", { className: "sql-panel" },
    createElement("h2", null, "Mini DBMS SQL 실행"),
    createElement("label", { htmlFor: "sql-input" }, "SQL 문장 (INSERT · SELECT)"),
    createElement("input", {
      id: "sql-input",
      value: props.sql,
      onInput: props.onInput,
    }),
    createElement("button", { type: "button", id: "sql-run", onClick: props.onRun }, "SQL 실행"),
    createElement("p", { id: "sql-source" }, props.sourceText),
    createElement(SqlResultTable, { table: props.table }),
  );
}

// gateway 응답을 JSON으로 읽고, 실패 응답이면 오류로 바꾼다.
async function readJsonResponse(response) {
  const body = await response.json();
  if (!response.ok) throw new Error(body.message || body.error || "HTTP request failed");
  return body;
}

// root component: state를 갖고 hook을 부르는 유일한 곳이다(Lifting State Up).
function App(props) {
  const apiBase = (props.apiBase || "/api").replace(/\/$/, "");
  const fetcher = props.fetch || globalThis.fetch;
  const [serverIp, setServerIp] = useState("불러오는 중...");
  const [draft, setDraft] = useState("");
  const [status, setStatus] = useState("Redis 캐시를 조회하고 있습니다.");
  const [sqlDraft, setSqlDraft] = useState(DEFAULT_SQL);
  const [sqlResult, setSqlResult] = useState("");
  const [sqlSource, setSqlSource] = useState("아직 실행하지 않았습니다.");

  // 결과 글자 → 표 데이터. sqlResult가 같으면 이전에 계산한 값을 그대로 쓴다.
  const sqlTable = useMemo(() => parseSqlResult(sqlResult), [sqlResult]);

  // 처음 그린 뒤 한 번, 저장된 server:ip 값을 gateway에서 읽어 온다.
  useEffect(() => {
    let active = true;
    fetcher(`${apiBase}/server-ip`).then(readJsonResponse).then(({ value, source }) => {
      if (!active) return;
      // 세 state를 한 묶음으로 바꿔 화면을 한 번만 다시 그린다.
      batch(() => {
        setServerIp(value === null ? "(값 없음)" : value);
        setDraft(value === null ? "" : value);
        setStatus(source === "cache" ? "Redis 캐시에서 읽었습니다." : "Mini DBMS에서 읽었습니다.");
      });
    }).catch((error) => {
      if (active) setStatus(`조회 실패: ${error.message}`);
    });
    return () => { active = false; };
  }, [apiBase, fetcher]);

  function handleInput(event) {
    setDraft(event.target.value);
  }

  // 저장: gateway가 Mini DBMS에 먼저 쓰고 Mini Redis cache를 갱신한다.
  function handleSave() {
    setStatus("저장 중...");
    fetcher(`${apiBase}/server-ip`, {
      method: "PUT",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ value: draft }),
    }).then(readJsonResponse).then(({ value }) => {
      batch(() => {
        setServerIp(value);
        setStatus("Mini DBMS 커밋 후 Redis 캐시를 갱신했습니다.");
      });
    }).catch((error) => {
      setStatus(`저장 실패: ${error.message}`);
    });
  }

  // 삭제: gateway가 Mini DBMS에서 지우고 Mini Redis cache를 무효화한다.
  function handleDelete() {
    setStatus("삭제 중...");
    fetcher(`${apiBase}/server-ip`, { method: "DELETE" })
      .then(readJsonResponse).then(() => {
        batch(() => {
          setServerIp("(값 없음)");
          setDraft("");
          setStatus("Mini DBMS에 삭제를 기록하고 Redis 캐시를 무효화했습니다.");
        });
      }).catch((error) => {
        setStatus(`삭제 실패: ${error.message}`);
      });
  }

  function handleSqlInput(event) {
    setSqlDraft(event.target.value);
  }

  // SQL 실행: gateway의 /api/sql로 보내고, 결과와 "어디서 온 결과인지"를 함께 보여 준다.
  function handleSqlRun() {
    setSqlSource("실행 중...");
    fetcher(`${apiBase}/sql`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ sql: sqlDraft }),
    }).then(readJsonResponse).then((body) => {
      let sourceText;
      if (body.source === "cache") {
        sourceText = "Mini Redis 캐시에서 읽었습니다 (Mini DBMS에 묻지 않음).";
      } else if (body.invalidated !== undefined) {
        sourceText = `Mini DBMS에 썼습니다. 무효화한 캐시 ${body.invalidated}건.`;
      } else if (body.cached) {
        sourceText = `Mini DBMS에서 읽어 ${body.ttlSeconds}초 동안 캐시에 넣었습니다.`;
      } else {
        sourceText = "Mini DBMS에서 읽었습니다 (캐시에 넣지 않음).";
      }
      batch(() => {
        setSqlResult(body.result);
        setSqlSource(sourceText);
      });
    }).catch((error) => {
      batch(() => {
        setSqlResult("");
        setSqlSource(`SQL 실패: ${error.message}`);
      });
    });
  }

  return createElement("main", { className: "cache-app" },
    createElement("h1", null, "Mini React · Redis · DBMS 통합 관리"),
    createElement(StatusMessage, { message: status }),
    createElement(ServerIpForm, {
      draft,
      onInput: handleInput,
      onSave: handleSave,
      onDelete: handleDelete,
    }),
    createElement(ServerIpValue, { value: serverIp }),
    createElement(SqlPanel, {
      sql: sqlDraft,
      onInput: handleSqlInput,
      onRun: handleSqlRun,
      sourceText: sqlSource,
      table: sqlTable,
    }),
  );
}

const api = {
  App, StatusMessage, ServerIpForm, ServerIpValue, SqlPanel, SqlResultTable,
  parseSqlResult, CACHE_KEY,
};
if (typeof module !== "undefined" && module.exports) module.exports = api;
if (typeof globalThis !== "undefined") globalThis.MiniReactApp = api;
})();
