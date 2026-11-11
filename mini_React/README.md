**확인 범위 (2026-11-11 갱신):** Mini React 폴더의 소스와 테스트 파일을 읽고 테스트를 실행했습니다. 단위 테스트 7개(`test_element` · `test_dom_to_vnode` · `test_render` · `test_diff` · `test_component` · `test_hook` · `test_memo`)와 통합 테스트 `test_integration.js`가 모두 통과했습니다. 통합 테스트는 실제 C 서버가 아닌 RESP 시험 서버를 사용합니다. `test_phase1_regression.js`는 jsdom이 있는 환경에서 통과했고, jsdom이 없으면 건너뛴다는 안내를 출력합니다. 실제 브라우저 성능 측정은 하지 않았으며, 아래 예시는 별도 실행 결과가 아닙니다.

---
## Mini React — 가상 DOM 엔진과 컴포넌트 시스템

Mini React는 외부 프레임워크 없이 JavaScript로 가상 DOM(Virtual DOM), 실제 DOM 생성, 트리 재조정(reconciliation), 함수형 컴포넌트와 훅(hook)을 구현한 학습용 엔진입니다. 이전 트리와 새 트리를 비교해 해당 DOM을 갱신하며, 별도 패치 목록을 먼저 만드는 구조가 아니라 `diff`가 실제 DOM을 직접 변경합니다.

- **핵심 기능:** VNode 생성, 속성·이벤트 갱신, `key` 기반 자식 재배치, `useState`·`useEffect`·`useMemo`.
- **컴포넌트 제약:** 훅은 최상위(루트) 컴포넌트에서만 쓸 수 있고, 자식 컴포넌트는 상태 없이 props만 받습니다. 자식 컴포넌트가 훅을 부르면 오류가 납니다.
- **검증 화면:** `phase1_playground.html`에서 편집한 테스트 영역을 실제 영역에 반영하고 기록을 앞뒤로 이동합니다. Patch마다 `MutationObserver`가 센 실제 영역의 DOM 변경 건수를 상태 줄에 보여 줍니다.

### 실행 방법

Mini React 폴더(`mini_React` 또는 `mini_react`)에서 Node.js를 사용합니다.

```bash
# 단위 테스트 7개와 통합 테스트를 하나씩 실행
node test_element.js
node test_dom_to_vnode.js
node test_render.js
node test_diff.js
node test_component.js
node test_hook.js
node test_memo.js
node test_integration.js

# Phase 1 회귀 테스트 실행 (jsdom 필요)
node test_phase1_regression.js
```

프로젝트 루트에 `test_all_in_one.js`가 있는 배치에서는 `node test_all_in_one.js`가 위 8개를 차례로 실행합니다. 회귀 테스트는 환경 변수 `JSDOM_PATH`, 설치된 `jsdom`, `../_chat_bot_server/node_modules/jsdom` 순서로 jsdom을 찾고, 찾지 못하면 건너뛴다는 안내를 출력하고 끝납니다. 화면은 `phase1_playground.html`을 브라우저에서 열어 확인할 수 있습니다. 실행 결과는 각 명령의 출력과 화면 상태를 대조해야 합니다.

### 폴더 구조

```text
mini_React/
├── element.js                 # VNode 생성, DOM → VNode 변환, DOM 정규화
├── render.js                  # VNode → 실제 DOM 생성과 속성·이벤트 반영
├── diff.js                    # 기존 VNode와 새 VNode 비교, DOM 직접 갱신
├── component.js               # 함수형 컴포넌트 인스턴스와 생명주기
├── hook.js                    # useState, useEffect, useMemo
├── phase1_playground.html     # 편집·반영·기록 이동 검증 화면
├── app.js                     # server:ip 조회·저장·삭제 예제 UI (루트 App + props만 받는 자식 3개)
├── integration_gateway.js     # Redis와 DBMS를 연결하는 HTTP 게이트웨이
├── test_*.js                  # 단위·통합·Phase 1 회귀 테스트
└── phase1_한계_수정_지시서.md # 이전 상태의 문제와 수정 요구 기록
```

### 데이터 흐름

```text
[최초 화면] createElement(type, props, ...children) → VNode
            render(VNode, container) → 실제 DOM 생성

[상태 변경] setState → 컴포넌트 함수 재실행 → 새 VNode
            diff(parent, currentDOM, 이전 VNode, 새 VNode)
            → 기존 DOM의 필요한 부분을 직접 갱신

[검증 화면] 테스트 영역 DOM → domToVnodeList → 새 VNode 배열
            → diffChildren으로 실제 영역 갱신 → 성공 시 기록 저장
            → 뒤로/앞으로: 기록의 VNode 배열과 현재 배열을 다시 비교
```

### 핵심 개념과 설계 원리

#### 1. 실제 DOM과 가상 DOM

실제 DOM은 브라우저가 관리하는 노드이며, VNode는 화면의 형태를 설명하는 JavaScript 객체입니다. `createElement`는 자식 배열을 평탄화하고 문자열·숫자를 `TEXT_ELEMENT`로 감싸므로 렌더러가 텍스트와 요소를 같은 트리로 순회할 수 있습니다. `null`, `undefined`, `false` 자식은 제외합니다.

```js
createElement('p', { className: 'title' }, '안녕')
// { type: 'p', props: { className: 'title', children: [텍스트 VNode] } }
```

이 코드는 구조를 설명하는 예시입니다. 실제 브라우저 실행 결과를 기록한 것은 아닙니다.

#### 2. 비교와 반영의 다섯 경우

`diff`는 비교 결과를 별도 배열로 내보내지 않고 비교 도중 실제 DOM에 반영합니다.

| 경우 | 비교 조건 | 처리 |
|---|---|---|
| 추가 | 이전 VNode 없음 | 새 노드를 생성해 부모에 추가 |
| 삭제 | 새 VNode 없음 | 컴포넌트를 정리한 뒤 노드 제거 |
| 교체 | 두 VNode의 `type`이 다름 | 새 노드를 만들고 기존 노드 교체 |
| 텍스트 변경 | 같은 텍스트 타입의 `nodeValue`가 다름 | 텍스트 노드의 값 갱신 |
| 속성 변경 | 같은 타입의 속성이나 이벤트가 다름 | 이전 값을 제거하거나 새 값 반영 |

같은 함수 타입의 컴포넌트는 인스턴스를 찾아 다시 실행합니다. 자식은 공통 부분을 비교하고, `key`가 없는 목록의 초과 삭제분은 뒤에서부터 지웁니다.

#### 3. `key`와 노드 재사용

위치 기준 비교는 목록의 순서가 바뀌면 같은 데이터를 다른 노드와 비교할 수 있습니다. 반면 `key`가 있는 자식은 키를 기준으로 이전 노드를 찾아 재사용하고 필요하면 `insertBefore`로 옮깁니다. 형제 사이 중복 키는 예외로 처리하며, 키가 없는 목록은 위치 기준으로 비교합니다. `key`는 렌더링할 DOM 속성에서 제외합니다.

이 방식은 노드 재사용을 돕지만 모든 입력에서 이동 횟수가 최솟값이라는 보장은 확인하지 않았습니다.

#### 4. 컴포넌트와 훅

컴포넌트 인스턴스는 자신이 마지막으로 만든 VNode와 훅 배열을 보관합니다. `useState`의 setter는 저장된 값이 `Object.is` 기준으로 바뀌면 그 자리에서 `update()`를 실행해 화면을 다시 그립니다. 여러 setter를 모아 한 번만 그리는 배칭(batching)은 구현하지 않았습니다. 다만 effect가 실행되는 도중에 불린 setter는 진행 중인 effect 묶음이 끝난 뒤로 미룹니다. `useEffect`는 의존성 배열을 비교해 효과와 정리 함수를 관리하고, 컴포넌트가 화면에서 사라지면(삭제 · 교체 · key 제거) `unmount`가 정리 함수를 실행합니다. `useMemo`도 의존성을 비교해 계산 결과를 재사용합니다. 훅은 호출 순서의 인덱스로 저장되므로 컴포넌트 렌더링마다 같은 순서로 호출해야 합니다.

훅은 최상위(루트) 컴포넌트에서만 쓸 수 있습니다. `component.js`가 다른 컴포넌트의 렌더링 · 갱신 도중에 만들어진 인스턴스를 자식(`isRoot = false`)으로 표시하고, `hook.js`의 `requireInstance`가 자식에서 훅이 불리면 오류를 냅니다. 그래서 상태는 루트에만 있고 자식 컴포넌트는 props만 받는 순수 함수가 됩니다. `app.js`는 루트 `App`이 상태 세 개를 갖고 `StatusMessage` · `ServerIpForm` · `ServerIpValue`에 props로 내려 줍니다.

#### 5. DOM 변경과 브라우저 화면

DOM 변경은 브라우저의 레이아웃(layout)이나 페인트(paint)에 영향을 줄 수 있지만, 변경 기록 수만으로 실제 레이아웃 횟수나 속도 향상을 판정할 수는 없습니다. `phase1_playground.html`은 `MutationObserver`로 실제 영역을 지켜보다가 Patch · 뒤로 · 앞으로 이동 때마다 `takeRecords()`로 그 사이의 DOM 변경 건수를 세어 상태 줄에 표시합니다. 이 숫자는 "바뀐 노드만 고쳤는가"를 확인하는 값이고, 레이아웃 횟수나 속도의 측정값은 아닙니다.

### 입력과 오류 처리

| 상황 | 소스에서 확인한 처리 |
|---|---|
| 자식 여러 개 삭제 | `childNodes`를 배열로 복사하고 초과 항목을 역순 삭제 |
| 최상위 요소 여러 개 또는 빈 영역 | `domToVnodeList`가 영역의 자식 전체를 VNode 배열로 변환 |
| 중복 형제 `key` | `Duplicate sibling key` 예외 발생 |
| 화면 반영 실패 | 이전 VNode와 기록 위치로 복구를 시도하며, 복구 자체가 실패하면 별도 상태 표시 |
| `script`와 인라인 `on*` | DOM 변환과 정규화의 기본 설정에서 제거 |
| 변경 없는 Patch | VNode 배열 비교 후 기록을 추가하지 않음 |
| 자식 컴포넌트에서 훅 호출 | `useState can only be used in the root component` 형태의 오류 발생 |

HTML은 브라우저의 `innerHTML`로 읽으므로, 이 화면에 별도의 엄격한 HTML 문법 검사기가 있다고 볼 수 없습니다. 기본 제거 규칙도 범용 보안 필터의 완전한 대체물로 취급하지 않습니다.

### 한계와 검증 과제

- 비교와 컴포넌트 갱신은 동기 실행입니다. 큰 트리에서 화면 응답에 미치는 영향은 별도 측정이 필요합니다.
- 이벤트는 브라우저의 `addEventListener`로 연결합니다. React의 합성 이벤트 시스템이나 전체 API 호환성을 제공한다는 근거는 없습니다.
- `domToVnode`는 DOM 속성에서 `key` 또는 `data-key`를 읽지만, 이 변환만으로 함수형 이벤트 핸들러를 복원하지는 않습니다.
- [이전 한계 지시서](phase1_한계_수정_지시서.md)는 작성 당시의 관찰과 요구입니다. 현재 코드에 관련 변경이 보이더라도 그 문서의 과거 측정값을 현재 테스트 통과로 간주하지 않습니다.

**직접 확인:** 테스트 화면에서 자식 추가·삭제와 순서 변경을 적용한 뒤 실제 영역과 기록 이동 결과를 대조합니다. 이어 위 두 테스트 명령의 출력과 종료 상태를 확인합니다.

## Batching · useMemo · SQL 화면 (2026-11-11 추가)

### Batching (선택 과제)

- `hook.js`의 `batch(work)`: `work` 안에서 부른 여러 `setState`는 바로 다시 그리지 않습니다. setter가 다시 그릴 component를 `batchedUpdates`에 넣어 두고, 가장 바깥 `batch`가 끝날 때 `batch`가 `update()`를 한 번만 부릅니다.
- event handler 자동 batching: `render.js`와 `diff.js`가 DOM에 handler를 붙일 때 `toBatchedListener(handler)`로 감쌉니다. 사용자가 한 번 클릭했을 때 handler가 `setState`를 세 번 불러도 화면은 한 번만 다시 그립니다.
- `batch` 밖(예: `fetch`의 `then` 안)에서 부른 `setState`는 이전과 같이 바로 다시 그립니다. 그래서 `app.js`는 `then` 안에서 여러 state를 바꿀 때 `batch(() => { ... })`로 직접 묶습니다.
- 실제 React와 다른 점: 실제 React 18은 `then` · `setTimeout` 안의 `setState`도 자동으로 묶습니다. 이 구현은 event handler와 `batch()` 안만 묶습니다.

### useMemo를 화면에서 쓰는 곳

`app.js`의 `App`이 `useMemo(() => parseSqlResult(sqlResult), [sqlResult])`를 부릅니다. `parseSqlResult`는 Mini DBMS가 돌려준 결과 글자를 표 데이터(`{ columns, rows, summary }`)로 바꿉니다. 사용자가 입력 칸에 글자를 쳐서 `App`이 다시 실행되어도 `sqlResult`가 같으면 `useMemo`가 이전 값을 그대로 돌려주므로 결과 글자를 다시 자르지 않습니다.

### SQL 화면과 gateway의 `/api/sql`

- 화면: `SqlPanel`(stateless 자식)이 SQL 입력 칸 · 실행 버튼 · 결과 출처 · 결과 표를 그립니다. state는 모두 root인 `App`에 있습니다.
- `POST /api/sql` `{ "sql": "..." }`:
  - SELECT: gateway가 Mini Redis에 `sql:<문장>` key로 먼저 묻습니다. 있으면 `source: "cache"`로 답합니다. 없으면 Mini DBMS SQL 서버(port 6381)에 묻고, 결과를 `SET ... EX 30`으로 넣은 뒤 `source: "database"`로 답합니다.
  - INSERT: gateway가 Mini DBMS에 먼저 쓰고, 그 테이블의 SELECT cache key를 `DEL`로 지웁니다(무효화). 지우지 못한 cache도 TTL이 지나면 Mini Redis가 지웁니다.
  - 잘못된 SQL: Mini DBMS가 `-ERR`로 답하면 gateway가 400 `sql_error`로 바꿉니다.
- 환경 변수: `SQL_HOST`, `SQL_PORT`(기본 6381), `SQL_CACHE_TTL_SECONDS`(기본 30).

### 테스트

```bash
node test_hook.js          # PASS 9 · 10: batch()와 event handler 자동 batching
node test_integration.js   # PASS 7 ~ 9: SQL cache · 무효화 · 화면의 결과 표 (가짜 서버 사용)
REDIS_PORT=6379 DBMS_PORT=6380 SQL_PORT=6381 node test_real_stack.js   # 실제 C 서버 3개로 7개 사례
```
