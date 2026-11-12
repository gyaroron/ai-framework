# miniReact — diff 지연을 줄이다

> 외부 프레임워크 의존성 없이 순수 Vanilla JS로 직접 구현한 miniReact(Virtual DOM · Diff · Patch · Hooks · 히스토리)의 4분 발표 자료입니다.

## 사용 방법

1. 브라우저에서 <kbd>F11</kbd>을 눌러 전체 화면으로 바꿉니다.
2. 아래 `시작 ›`을 누릅니다.
3. 그림을 누르면 다음 장으로 넘어갑니다. 그림 아래 오른쪽의 `‹ 이전` · `다음 ›`으로도 이동합니다.

## 구성

| 장 | 내용 | 시간 |
| --- | --- | --- |
| [1](#1--9) | 표지: `UI = f(state)` | 6초 |
| [2](#2--9) | 왜 React인가: 명령형(imperative)과 선언형(declarative) | 40초 |
| [3](#3--9) | 이점 ①: setState를 3번 불러도 화면은 한 번만 바뀜 | 40초 |
| [4](#4--9) | 이점 ②: 컴포넌트(component)로 나누고 props로 연결 | 30초 |
| [5](#5--9) | miniReact가 화면을 바꾸는 길 (stack-diagram 방식) | 26초 |
| [6](#6--9) | 가상의 할 일 목록으로 보는 한 바퀴 | 30초 |
| [7](#7--9) | 코드 폴더 구조 | 18초 |
| [8](#8--9) | diff 지연을 줄이다: 알고리즘과 수치 (전체의 15%) | 36초 |
| [9](#9--9) | 마무리 | 14초 |

## 코드 출처

| 자료 | 브랜치 | 이 발표에서 쓰는 곳 |
| --- | --- | --- |
| [`mini_React/app.js`](https://github.com/kimtaeg/ai-framework/blob/gyaroron_kjs/mini_React/app.js) | `gyaroron_kjs` | 2 · 4장: React를 쓰는 이유와 이점 |
| [`phase2/app.js`](https://github.com/kimtaeg/ai-framework/blob/sikhesung/phase2/app.js) | `sikhesung` | 3장: mini React만 쓰는 Batching 데모 |
| [`src/lib/fenwick.js`](https://github.com/kimtaeg/ai-framework/blob/sikhesung/src/lib/fenwick.js) | `sikhesung` | 8장: 알고리즘 코드 |
| [`README.md`](https://github.com/kimtaeg/ai-framework/blob/gyaroron_kjs/README.md) | `gyaroron_kjs` | 제목, 7장의 폴더 구조, 8장의 측정값. 원본은 [백업 브랜치](https://github.com/gyaroron/ai-framework/blob/backup/gyaroron_kjs-original/README.md)에 보관 |

> [!NOTE]
> 2 · 4장의 코드는 `gyaroron_kjs` 브랜치의 구현이고, 3장의 코드와 5장의 구조, 7장의 폴더 구조, 8장의 알고리즘 · 측정값은 `sikhesung` 브랜치의 구현입니다. 6장의 페이지와 코드는 설명을 위해 만든 가상의 예시입니다. 두 브랜치는 diff를 구현한 방식이 다르므로, 8장의 측정값을 2 · 4장의 코드에 그대로 적용하지 않습니다.

<a href="#1--9">시작 ›</a>

<picture><img src="spacer.svg" alt="" width="1" height="1500"></picture>

###### 1 / 9

<a href="#2--9"><img src="01_hero.svg" alt="miniReact 표지: 상태가 컴포넌트 함수를 거쳐 화면이 된다" width="100%"></a>

<p align="right"><sub><a href="#2--9">다음 ›</a></sub></p>

> 화면은 현재 상태(state)를 바탕으로 만든 결과입니다.

<picture><img src="spacer.svg" alt="" width="1" height="1500"></picture>

###### 2 / 9

<a href="#3--9"><img src="02_imperative_vs_declarative.svg" alt="명령형과 선언형 비교: 명령형은 개발자가 실제 DOM을 직접 고치고, 선언형은 miniReact가 바뀐 곳을 찾아 반영한다" width="100%"></a>

<p align="right"><sub><a href="#1--9">‹ 이전</a> &nbsp;·&nbsp; <a href="#3--9">다음 ›</a></sub></p>

> 명령형에서는 개발자가 DOM을 고치는 순서를 관리하고, 선언형에서는 개발자가 상태와 화면의 관계만 표현합니다.

<picture><img src="spacer.svg" alt="" width="1" height="1500"></picture>

###### 3 / 9

<a href="#4--9"><img src="03_batching.svg" alt="왼쪽은 mini React의 phase2/app.js 코드, 오른쪽은 Batching 데모 화면. setState × 3 버튼을 누르면 setCount 줄이 세 번 켜지고 화면의 카운트는 0에서 3으로 한 번에 바뀐다" width="100%"></a>

<p align="right"><sub><a href="#2--9">‹ 이전</a> &nbsp;·&nbsp; <a href="#4--9">다음 ›</a></sub></p>

> 개발자가 `setCount`를 3번 불러도, miniReact가 변경을 모아 화면의 숫자를 한 번만 고칩니다.

<picture><img src="spacer.svg" alt="" width="1" height="1500"></picture>

###### 4 / 9

<a href="#5--9"><img src="04_components.svg" alt="App이 state를 갖고 자식 컴포넌트 네 개에 props를 내려 주며, SqlPanel은 SqlResultTable에 table을 내려 준다" width="100%"></a>

<p align="right"><sub><a href="#3--9">‹ 이전</a> &nbsp;·&nbsp; <a href="#5--9">다음 ›</a></sub></p>

> `App`만 state를 갖고, 자식 컴포넌트는 받은 props만 읽어 화면의 모습을 표현합니다.

<picture><img src="spacer.svg" alt="" width="1" height="1500"></picture>

###### 5 / 9

<a href="#6--9"><img src="05_architecture.svg" alt="miniReact 구조: 브라우저의 이벤트가 component.js, vdom.js, diff.js, patch.js를 차례로 거쳐 다시 브라우저의 DOM에 반영된다" width="100%"></a>

<p align="right"><sub><a href="#4--9">‹ 이전</a> &nbsp;·&nbsp; <a href="#6--9">다음 ›</a></sub></p>

> 상태가 바뀌면 `component.js` → `vdom.js` → `diff.js` → `patch.js`가 차례로 일하고, 바뀐 곳만 브라우저의 DOM에 반영합니다.

<picture><img src="spacer.svg" alt="" width="1" height="1500"></picture>

###### 6 / 9

<a href="#7--9"><img src="06_todo_example.svg" alt="가상의 할 일 목록 페이지와 그 코드. 추가 버튼을 누르면 화면에는 기존 두 줄은 그대로 둔 채 새 줄 하나만 추가된다" width="100%"></a>

<p align="right"><sub><a href="#5--9">‹ 이전</a> &nbsp;·&nbsp; <a href="#7--9">다음 ›</a></sub></p>

> 개발자의 코드는 할 일 3개를 모두 적은 목록을 miniReact에 넘깁니다. miniReact는 화면에 이미 있는 2줄은 건드리지 않고, 화면에 없던 1줄만 새로 넣습니다.

<picture><img src="spacer.svg" alt="" width="1" height="1500"></picture>

###### 7 / 9

<a href="#8--9"><img src="07_folder_structure.svg" alt="코드 폴더 구조: 라이브러리 본체에서 vdom.js, diff.js, patch.js가 차례로 일하고, 나머지는 측정, 테스트, 시연과 학습 페이지, 서버와 도구 묶음이다" width="100%"></a>

<p align="right"><sub><a href="#6--9">‹ 이전</a> &nbsp;·&nbsp; <a href="#8--9">다음 ›</a></sub></p>

> miniReact 코드는 라이브러리 본체, 측정, 테스트, 시연 · 학습 페이지, 서버 · 도구 다섯 묶음으로 나뉩니다.

<picture><img src="spacer.svg" alt="" width="1" height="1500"></picture>

###### 8 / 9

<a href="#9--9"><img src="08_numbers.svg" alt="측정값 세 가지: 줄을 옮긴 횟수 999번에서 1번, 계산 시간 2,978ms에서 23ms, 기록 메모리 206.9MB에서 22.7MB" width="100%"></a>

<p align="right"><sub><a href="#7--9">‹ 이전</a> &nbsp;·&nbsp; <a href="#9--9">다음 ›</a></sub></p>

> 상태가 바뀔 때마다 diff는 옛 tree와 새 tree를 비교합니다. 이 구현은 그 지연을 재고, 세 곳에서 줄였습니다.

| 줄인 곳 | 고친 코드 | 무엇을 바꿨나 |
| --- | --- | --- |
| 줄을 옮긴 횟수<br>999번 → 1번 | [`lis.js`](https://github.com/kimtaeg/ai-framework/blob/sikhesung/src/lib/lis.js)<br>[`diff.js`](https://github.com/kimtaeg/ai-framework/blob/sikhesung/src/lib/diff.js)의 `place` | `place`가 `longestIncreasingSubsequence`로 서로의 앞뒤 순서가 그대로인 줄을 고르고, 그 줄은 옮기지 않습니다. |
| 계산에 걸린 시간<br>2,978 ms → 23 ms | [`fenwick.js`](https://github.com/kimtaeg/ai-framework/blob/sikhesung/src/lib/fenwick.js)의 `FenwickSum` | 줄의 현재 위치를 `indexOf` · `splice`로 목록을 훑어 찾던 것을, `countBefore`가 적은 횟수의 덧셈으로 구하게 바꿨습니다. |
| 기록에 쓰는 메모리<br>206.9 MB → 22.7 MB | [`history.js`](https://github.com/kimtaeg/ai-framework/blob/sikhesung/src/lib/history.js)의 `DeltaHistory` | 기록마다 화면 tree 전체를 저장하던 것을, 10번에 한 번만 tree를 저장하고 그 사이에는 패치(patch)만 저장하게 바꿨습니다. |

<picture><img src="spacer.svg" alt="" width="1" height="1500"></picture>

###### 9 / 9

<a href="#1--9"><img src="09_closing.svg" alt="마무리: miniReact는 상태로 화면을 설명하고, 필요한 변경을 찾아 적용하며, 그 과정의 비용을 측정해 줄인 구현이다" width="100%"></a>

<p align="right"><sub><a href="#8--9">‹ 이전</a> &nbsp;·&nbsp; <a href="#1--9">처음 ↺</a></sub></p>

> miniReact는 상태로 화면을 설명하고, 필요한 변경을 찾아 적용하며, 그 과정의 비용을 측정해 줄인 구현입니다.

<picture><img src="spacer.svg" alt="" width="1" height="1500"></picture>
