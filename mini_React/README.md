**확인 범위:** `mini_react/`의 소스와 테스트 파일을 읽고 테스트를 실행했습니다. `node test_all_in_one.js`는 1~7단계를 통과했으나 8단계는 `127.0.0.1:8223` 연결 시 `EACCES`로 실패했습니다. 8단계는 실제 C 서버가 아닌 RESP 시험 서버를 사용합니다. `node mini_react/test_phase1_regression.js`는 jsdom 환경에서 통과했습니다. 실제 브라우저 성능 측정은 하지 않았으며, 아래 예시는 별도 실행 결과가 아닙니다.

---
## Mini React — 가상 DOM 엔진과 컴포넌트 시스템

Mini React는 외부 프레임워크 없이 JavaScript로 가상 DOM(Virtual DOM), 실제 DOM 생성, 트리 재조정(reconciliation), 함수형 컴포넌트와 훅(hook)을 구현한 학습용 엔진입니다. 이전 트리와 새 트리를 비교해 해당 DOM을 갱신하며, 별도 패치 목록을 먼저 만드는 구조가 아니라 `diff`가 실제 DOM을 직접 변경합니다.

- **핵심 기능:** VNode 생성, 속성·이벤트 갱신, `key` 기반 자식 재배치, `useState`·`useEffect`·`useMemo`.
- **검증 화면:** `phase1_playground.html`에서 편집한 테스트 영역을 실제 영역에 반영하고 기록을 앞뒤로 이동합니다.

### 실행 방법

프로젝트 루트에서 Node.js를 사용합니다.

```bash
# 요소, 렌더러, 비교, 컴포넌트, 훅, 통합 테스트의 8단계 실행
node test_all_in_one.js

# 위 8단계에 포함되지 않은 Phase 1 회귀 테스트 실행
node mini_react/test_phase1_regression.js
```

회귀 테스트는 `_chat_bot_server/node_modules/jsdom`을 불러오므로 해당 의존성이 설치되어 있어야 합니다. 화면은 `mini_react/phase1_playground.html`을 브라우저에서 열어 확인할 수 있습니다. 실행 결과는 각 명령의 출력과 화면 상태를 대조해야 합니다.

### 폴더 구조

```text
mini_react/
├── element.js                 # VNode 생성, DOM → VNode 변환, DOM 정규화
├── render.js                  # VNode → 실제 DOM 생성과 속성·이벤트 반영
├── diff.js                    # 기존 VNode와 새 VNode 비교, DOM 직접 갱신
├── component.js               # 함수형 컴포넌트 인스턴스와 생명주기
├── hook.js                    # useState, useEffect, useMemo
├── phase1_playground.html     # 편집·반영·기록 이동 검증 화면
├── app.js                     # server:ip 조회·저장·삭제 예제 UI
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

컴포넌트 인스턴스는 자신이 마지막으로 만든 VNode와 훅 배열을 보관합니다. `useState`는 저장된 값이 `Object.is` 기준으로 바뀔 때 재실행을 예약하고, `useEffect`는 의존성 배열을 비교해 효과와 정리 함수를 관리합니다. `useMemo`도 의존성을 비교해 계산 결과를 재사용합니다. 훅은 호출 순서의 인덱스로 저장되므로 컴포넌트 렌더링마다 같은 순서로 호출해야 합니다.

#### 5. DOM 변경과 브라우저 화면

DOM 변경은 브라우저의 레이아웃(layout)이나 페인트(paint)에 영향을 줄 수 있지만, 변경 기록 수만으로 실제 레이아웃 횟수나 속도 향상을 판정할 수는 없습니다. 현재 `phase1_playground.html` 소스에서는 `MutationObserver` 측정 코드를 확인하지 못했으므로, 첨부 예시의 수치를 이 구현의 측정값으로 옮기지 않습니다.

### 입력과 오류 처리

| 상황 | 소스에서 확인한 처리 |
|---|---|
| 자식 여러 개 삭제 | `childNodes`를 배열로 복사하고 초과 항목을 역순 삭제 |
| 최상위 요소 여러 개 또는 빈 영역 | `domToVnodeList`가 영역의 자식 전체를 VNode 배열로 변환 |
| 중복 형제 `key` | `Duplicate sibling key` 예외 발생 |
| 화면 반영 실패 | 이전 VNode와 기록 위치로 복구를 시도하며, 복구 자체가 실패하면 별도 상태 표시 |
| `script`와 인라인 `on*` | DOM 변환과 정규화의 기본 설정에서 제거 |
| 변경 없는 Patch | VNode 배열 비교 후 기록을 추가하지 않음 |

HTML은 브라우저의 `innerHTML`로 읽으므로, 이 화면에 별도의 엄격한 HTML 문법 검사기가 있다고 볼 수 없습니다. 기본 제거 규칙도 범용 보안 필터의 완전한 대체물로 취급하지 않습니다.

### 한계와 검증 과제

- 비교와 컴포넌트 갱신은 동기 실행입니다. 큰 트리에서 화면 응답에 미치는 영향은 별도 측정이 필요합니다.
- 이벤트는 브라우저의 `addEventListener`로 연결합니다. React의 합성 이벤트 시스템이나 전체 API 호환성을 제공한다는 근거는 없습니다.
- `domToVnode`는 DOM 속성에서 `key` 또는 `data-key`를 읽지만, 이 변환만으로 함수형 이벤트 핸들러를 복원하지는 않습니다.
- [이전 한계 지시서](mini_react/phase1_한계_수정_지시서.md)는 작성 당시의 관찰과 요구입니다. 현재 코드에 관련 변경이 보이더라도 그 문서의 과거 측정값을 현재 테스트 통과로 간주하지 않습니다.

**직접 확인:** 테스트 화면에서 자식 추가·삭제와 순서 변경을 적용한 뒤 실제 영역과 기록 이동 결과를 대조합니다. 이어 위 두 테스트 명령의 출력과 종료 상태를 확인합니다.
