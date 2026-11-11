(function () {
"use strict";

const elementApi = typeof module !== "undefined" && module.exports
  ? require("./element")
  : globalThis.MiniReactElement;
const hookApi = typeof module !== "undefined" && module.exports
  ? require("./hook")
  : globalThis.MiniReactHook;
const { createElement } = elementApi;
const { useState, useEffect } = hookApi;

const CACHE_KEY = "server:ip";

// Stateless children: they call no hooks and only read the props the root passes down.
function StatusMessage(props) {
  return createElement("p", { id: "cache-status", role: "status" }, props.message);
}

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

// Root component: the only place that owns state and calls hooks (Lifting State Up).
function App(props) {
  const apiBase = (props.apiBase || "/api").replace(/\/$/, "");
  const fetcher = props.fetch || globalThis.fetch;
  const [serverIp, setServerIp] = useState("불러오는 중...");
  const [draft, setDraft] = useState("");
  const [status, setStatus] = useState("Redis 캐시를 조회하고 있습니다.");

  useEffect(() => {
    let active = true;
    fetcher(`${apiBase}/server-ip`).then(async (response) => {
      const body = await response.json();
      if (!response.ok) throw new Error(body.message || body.error || "HTTP request failed");
      return body;
    }).then(({ value, source }) => {
      if (!active) return;
      setServerIp(value === null ? "(값 없음)" : value);
      setDraft(value === null ? "" : value);
      setStatus(source === "cache" ? "Redis 캐시에서 읽었습니다." : "Mini DBMS에서 읽었습니다.");
    }).catch((error) => {
      if (active) setStatus(`조회 실패: ${error.message}`);
    });
    return () => { active = false; };
  }, [apiBase, fetcher]);

  function handleInput(event) {
    setDraft(event.target.value);
  }

  function handleSave() {
    setStatus("저장 중...");
    fetcher(`${apiBase}/server-ip`, {
      method: "PUT",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ value: draft }),
    }).then(async (response) => {
      const body = await response.json();
      if (!response.ok) throw new Error(body.message || body.error || "HTTP request failed");
      return body;
    }).then(({ value }) => {
      setServerIp(value);
      setStatus("Mini DBMS 커밋 후 Redis 캐시를 갱신했습니다.");
    }).catch((error) => {
      setStatus(`저장 실패: ${error.message}`);
    });
  }

  function handleDelete() {
    setStatus("삭제 중...");
    fetcher(`${apiBase}/server-ip`, { method: "DELETE" })
      .then(async (response) => {
        const body = await response.json();
        if (!response.ok) throw new Error(body.message || body.error || "HTTP request failed");
        return body;
      }).then(() => {
        setServerIp("(값 없음)");
        setDraft("");
        setStatus("Mini DBMS에 삭제를 기록하고 Redis 캐시를 무효화했습니다.");
      }).catch((error) => {
        setStatus(`삭제 실패: ${error.message}`);
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
  );
}

const api = { App, StatusMessage, ServerIpForm, ServerIpValue, CACHE_KEY };
if (typeof module !== "undefined" && module.exports) module.exports = api;
if (typeof globalThis !== "undefined") globalThis.MiniReactApp = api;
})();
