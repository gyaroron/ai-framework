# Mini React Phase 1 — 한계(Limitation) 수정 지시서

대상: `mini_react/diff.js`, `mini_react/element.js`, `mini_react/phase1_playground.html`
작성일: 2026-11-07
용도: 다른 AI에게 그대로 넘겨 위 파일의 한계 3개를 고치게 하는 요구사항 지시서예요. 각 한계는 **현상 → 재현 입력 → 원인 줄 → 기대 동작 → 수정 지시 → 완료 조건** 순서로 적었어요. 판정은 모두 "이 입력을 넣으면 이 값이 나와야 한다"는 형식이에요.

## 0. 상태 표기와 공통 제약

상태 표기는 세 가지예요.

| 표기 | 뜻 |
|---|---|
| 측정 | 실제 엔진을 브라우저에서 직접 돌려 얻은 값 |
| 코드 확인 | 소스를 읽어 확인한 사실 |
| 설명 | 읽은 코드에서 추론한 것(직접 재지 않음) |

공통 제약은 모든 수정에 적용해요.

1. `diff(parentDom, currentDom, oldVNode, newVNode)`의 인자 모양을 유지한다. 반환값은 반영 뒤의 최상위 DOM 노드다.
2. 외부 프레임워크(React, Vue 등)를 쓰지 않는다. HTML · CSS · JavaScript만 쓴다(과제 기술 스택 제한).
3. 기존 단위 테스트(`test_diff.js`, `test_dom_to_vnode.js`, `test_render.js`)를 깨뜨리지 않는다. 새 동작마다 테스트를 하나 이상 추가한다.
4. "변경된 부분만 실제 영역에 렌더링한다"는 과제 요구를 기준으로 삼는다. 결과 DOM이 맞는지와 **손댄 노드 수**(`MutationObserver` 기록 수)를 함께 판정한다.

## 1. 한계 1 — 최상위 요소가 여러 개여도 첫 번째만 처리한다

| 항목 | 내용 |
|---|---|
| 현상 | 테스트 영역에 최상위 요소를 2개 이상 두고 Patch를 눌러도 둘째 요소 이후를 읽지 않아 "변경 없음"으로 끝난다. (코드 확인 + 측정) |
| 재현 입력 | 테스트 영역 끝에 `<p>둘째</p>`를 추가한 뒤 Patch한다. |
| 원인 줄 | `phase1_playground.html` 190~193행 `testSurface.firstElementChild ? domToVnode(testSurface.firstElementChild) : null`<br>143행 `actualRoot = actualSurface.firstElementChild`(실제 영역도 첫 요소 하나만 추적) |
| 기대 동작 | 과제는 "현재 테스트 영역 상태를 다시 Virtual DOM으로 만든다"고 요구한다. 상태의 단위는 첫 요소가 아니라 **영역 전체**다. |
| 수정 지시 | ① 영역의 모든 최상위 요소를 VNode 배열로 바꾸는 `domToVnodeList(container)`를 만든다.<br>② `history` 한 칸을 이 배열 전체로 한다.<br>③ 배열의 같은 위치끼리 기존 `diff`를 부르고, 남는 쪽은 추가 또는 삭제한다.<br>④ 최상위 요소가 0개인 상태(빈 영역)도 유효한 상태로 처리한다. |
| 완료 조건 | (a) 둘째 요소를 추가한 Patch 뒤 실제 영역에 최상위 요소가 2개이고 `실제.innerHTML === 테스트.innerHTML`이다.<br>(b) 이 Patch로 `history.length`가 1만큼 늘고, 뒤로가기로 1개짜리 상태가 돌아온다.<br>(c) 영역을 비우고 Patch해도 예외가 없고 실제 영역도 비워진다. |

## 2. 한계 2 — 자식을 위치(index)로만 비교한다 (key 없음)

| 항목 | 내용 |
|---|---|
| 현상 | (측정) `li` 4개 `A B C D`에서 맨 앞 A를 지우면 결과 DOM은 맞지만 `MutationObserver` 기록이 4건이다. 필요한 최소는 1건이다.<br>맨 앞에 A를 끼워 넣는 경우도 4건이다. 위치 0 · 1 · 2의 글자를 모두 덮어쓰고 마지막 하나를 지우기 때문이다. |
| 재현 입력 | 1) `ul`에 `li` A B C D를 두고 A만 삭제한 뒤 Patch한다.<br>2) A B C를 B A C로 순서만 바꾸고 Patch한다(노드가 재사용되지 않고 글자만 덮어써지는 것은 코드 확인). |
| 원인 줄 | `diff.js` 110~114행 `for (i = 0; i < count; i += 1) diff(currentDom, domChildren[i], oldChildren[i], newChildren[i])`<br>같은 위치(i)의 두 자식만 짝지으며 식별자를 보지 않는다. |
| 기대 동작 | 과제의 중점 포인트는 "최소 변경을 찾기 위한 5가지 핵심 케이스"다. key 지원은 과제에 명시된 의무가 아니므로 **선택 구현**이다. key가 없는 기존 동작은 그대로 유지한다. |
| 수정 지시 | ① 자식 VNode의 `props.key`를 식별자로 허용하고, `domToVnode`가 `key` 속성을 읽어 VNode에 담게 한다.<br>② key가 있는 자식은 **같은 key끼리** 짝지어 `diff`를 부르고, 순서가 다르면 `insertBefore`로 DOM 노드를 옮긴다.<br>③ key가 없는 자식은 지금처럼 위치로 비교한다.<br>④ `key`는 DOM 속성으로 렌더링하지 않는다. |
| 완료 조건 | (a) key 있는 `li` A B C D에서 A를 지우면 `MutationObserver` 기록이 1건이다.<br>(b) A B C를 B A C로 바꿀 때 `li` DOM 노드 3개의 참조(`===`)가 Patch 전후 같다.<br>(c) key 없는 기존 테스트의 결과는 그대로다. |

## 3. 한계 3 — 자식을 2개 이상 지우면 예외가 나고 실제 영역이 망가진다

| 항목 | 내용 |
|---|---|
| 현상 | (측정) `li` 4개 `A B C D`를 `A B`로 줄이면 `TypeError: Failed to execute 'removeChild' ... parameter 1 is not of type 'Node'`가 난다. 예외 뒤 실제 DOM은 기대한 `A B`가 아니라 **`A B D`** 로 남는다.<br>`A B` → 빈 `ul`도 같은 예외가 나고 `B`가 남는다.<br>`A B C` → `A`도 같은 예외가 난다.<br>마지막 1개만 지우는 경우는 성공하고 변경 기록은 1건이다. |
| 재현 입력 | `ul`의 `li`를 2개 이상 지우고 Patch한다. |
| 원인 줄 | `diff.js` 109행 `const domChildren = childNodes(currentDom)`가 **살아 있는 목록**(live `NodeList`)을 그대로 들고 있다(코드 확인).<br>112~114행 루프에서 `i = 2`의 삭제(84행 `parentDom.removeChild`)가 목록을 즉시 줄이면 `i = 3`의 `domChildren[3]`이 `undefined`가 된다(측정으로 일치 확인). |
| 기대 동작 | 과제의 품질 항목은 "엣지 케이스를 최대한 고려한다"이고, 5가지 핵심 케이스에 노드 삭제가 들어 있다. 삭제 수와 관계없이 결과가 같아야 하고, 실패하더라도 화면과 `history`가 어긋나면 안 된다. |
| 수정 지시 | ① 자식 루프 전에 `Array.from(currentDom.childNodes)`로 **스냅샷**을 만들어 그 배열만 인덱싱한다.<br>② 공통 길이까지는 위치별로 `diff`를 부르고, `old`가 더 길면 초과 자식을 **뒤에서 앞으로** 제거한다.<br>③ `patchFromTestArea`는 `diff`를 `try/catch`로 감싸고, 예외가 나면 `history`와 `currentVNode`를 바꾸지 않은 채 실제 영역을 `currentVNode`로 다시 그려 복구한다.<br>④ 복구가 일어났음을 `status`에 표시한다. |
| 완료 조건 | (a) `A B C D → A B`, `A B C D → 빈 ul`, `A B C → A` 세 경우 모두 예외 0이고 `실제.innerHTML === 기대 HTML`이다.<br>(b) 각 경우 `MutationObserver` 기록 수가 지운 노드 수와 같다.<br>(c) 강제로 예외를 낸 경우에도 `history.length`와 `historyIndex`가 Patch 전과 같다. |

### 3-1. 수정 1 방향의 검증 결과 (구현해서 돌려 본 값)

위 수정 지시 ①②를 적용한 `diff`를 같은 엔진 위에서 실제로 돌려 얻은 값이에요(2026-11-07, 브라우저 실행).

| 입력 | 원본 diff | 수정 diff |
|---|---|---|
| `A B C D` → `A B` | 예외, DOM 손상 | 예외 0, DOM 일치, 변경 기록 2건 |
| `A B C D` → 빈 `ul` | 예외 | 예외 0, DOM 일치, 변경 기록 4건 |
| `A B C` → `A` | 예외 | 예외 0, DOM 일치, 변경 기록 2건 |
| `A` → `A B C` | 해당 없음 | 예외 0, DOM 일치, 변경 기록 2건 |

변경 기록 수가 지운 노드 수와 같아 완료 조건 (a)(b)를 통과했어요. 완료 조건 (c)(`try/catch` 복구와 `history` 유지)는 아직 구현해 돌려 보지 않았어요(미측정).

## 4. 우선순위

| 순서 | 한계 | 이유 |
|---|---|---|
| 1 | 한계 3 (다중 삭제) | 예외와 화면 손상이 일어나는 **올바름(correctness)** 오류다. 과제 필수 케이스인 노드 삭제에 걸린다. |
| 2 | 한계 1 (최상위 여러 개) | 결과가 조용히 무시되는 **누락**이다. 요구한 "영역 상태"의 정의와 어긋난다. |
| 3 | 한계 2 (key) | 결과는 맞고 **효율**만 나쁘다. 과제가 요구한 의무가 아니라 차별화 요소로 다룬다. |

한계 1 · 2의 "손댄 노드 수"는 `MutationObserver` 기록 수 기준이에요. 이 값은 DOM 변경 기록 수이고 브라우저의 reflow · repaint 횟수가 아니에요(미측정).

## 5. 붙여넣기용 지시문

아래 블록을 그대로 복사해 다른 AI에게 전달해요.

````text
[역할] 너는 mini_react(Vanilla JS Virtual DOM + Diff + Patch)의 수정 담당이다.
[대상 파일] mini_react/diff.js, mini_react/element.js, mini_react/phase1_playground.html
[공통 제약]
1. diff(parentDom, currentDom, oldVNode, newVNode)의 인자 모양과 반환값(반영 뒤 최상위 DOM 노드)을 유지한다.
2. 외부 프레임워크를 쓰지 않는다. HTML, CSS, JavaScript만 쓴다.
3. 기존 단위 테스트(test_diff.js, test_dom_to_vnode.js, test_render.js)를 깨뜨리지 않고, 수정마다 테스트를 하나 이상 추가한다.
4. 판정은 결과 DOM과 손댄 노드 수(MutationObserver 기록 수) 둘 다로 한다.

[수정 1 — 필수, 최우선] 자식을 2개 이상 지우면 예외가 난다.
- 현상: ul의 li A,B,C,D를 A,B로 줄이면 TypeError(removeChild)가 나고 실제 DOM이 A,B,D로 남는다. A,B를 빈 ul로 줄여도 예외가 난다.
- 원인: diff.js 109행이 살아 있는 childNodes를 그대로 쓰고, 112~114행 루프 중 삭제가 목록을 줄여 domChildren[i]가 undefined가 된다.
- 지시: 루프 전에 Array.from(currentDom.childNodes)로 스냅샷을 만든다. 공통 길이까지 위치별 diff, old가 더 길면 초과분을 뒤에서 앞으로 제거한다. patchFromTestArea는 diff를 try/catch로 감싸 예외 시 history와 currentVNode를 바꾸지 않고 실제 영역을 currentVNode로 다시 그린 뒤 status에 복구 사실을 표시한다.
- 완료: A B C D→A B, A B C D→빈 ul, A B C→A 모두 예외 0, 실제.innerHTML === 기대 HTML, MutationObserver 기록 수 = 지운 노드 수. 예외를 강제로 내도 history.length와 historyIndex가 Patch 전과 같다.

[수정 2 — 필수] 최상위 요소가 여러 개여도 첫 번째만 처리한다.
- 현상: 테스트 영역에 최상위 요소를 둘 두고 Patch하면 "변경 없음"으로 끝난다.
- 원인: phase1_playground.html 190~193행의 testSurface.firstElementChild, 143행의 actualRoot = actualSurface.firstElementChild.
- 지시: 영역의 모든 최상위 요소를 VNode 배열로 바꾸는 domToVnodeList(container)를 만들고, history 한 칸을 이 배열 전체로 한다. 배열 위치별로 기존 diff를 부르고 남는 쪽은 추가 또는 삭제한다. 최상위 0개(빈 영역)도 유효한 상태로 처리한다.
- 완료: 둘째 요소 추가 Patch 뒤 실제 영역 최상위 요소 2개, 실제.innerHTML === 테스트.innerHTML, history.length가 1만큼 증가, 뒤로가기 시 1개짜리 상태 복원. 영역을 비워 Patch해도 예외 0.

[수정 3 — 선택, 마지막] 자식을 위치로만 비교한다(key 없음).
- 현상: li A,B,C,D에서 A만 지워도 MutationObserver 기록이 4건이다(최소 1건).
- 원인: diff.js 110~114행의 위치(i) 기준 짝짓기.
- 지시: props.key를 식별자로 허용하고 domToVnode가 key 속성을 VNode에 담게 한다. key가 있는 자식은 같은 key끼리 diff하고 순서가 다르면 insertBefore로 노드를 옮긴다. key가 없는 자식은 지금처럼 위치로 비교한다. key는 DOM 속성으로 렌더링하지 않는다.
- 완료: key 있는 li A,B,C,D에서 A 삭제 시 기록 1건. A,B,C→B,A,C 때 li DOM 노드 3개의 참조가 Patch 전후 같다. key 없는 기존 테스트 결과는 그대로다.

[보고 형식] 수정마다 (1) 바꾼 파일과 줄, (2) 추가한 테스트 이름, (3) 완료 조건 각 항목의 실제 측정값을 표로 적는다. 측정하지 않은 항목은 "미측정"이라고 쓴다.
````

## 6. 이 지시서의 검증 범위

| 항목 | 상태 |
|---|---|
| 한계 3의 현상(`A B C D → A B` 예외, DOM이 `A B D`로 남음) | 측정 |
| 한계 2의 현상(앞 삭제·앞 삽입 각 4건, 마지막 1개 삭제 1건) | 측정 |
| 한계 1의 현상(`firstElementChild`만 읽어 "변경 없음") | 코드 확인 + 측정 |
| 수정 1의 방향(스냅샷 + 뒤에서 앞 제거) | 구현해서 측정, 완료 조건 (a)(b) 통과 |
| 수정 1의 (c), 수정 2, 수정 3 | 미검증(수정 방향만 제시) |
| 줄 번호(109, 110~114, 143, 190~193) | 파일을 직접 읽어 확인 |

줄 번호는 2026-11-07 기준 파일이에요. 파일이 바뀌면 함수 이름으로 위치를 다시 찾으세요.
