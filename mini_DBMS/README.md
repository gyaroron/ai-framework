## Mini DBMS — 영속 저장 엔진과 트랜잭션 시스템

Mini DBMS는 C99로 작성한 학습용 저장 엔진(storage engine)입니다. 4096바이트 페이지(page)를 파일에 읽고 쓰는 Pager, 메모리 프레임을 관리하는 Buffer Pool, 정수 키를 찾는 B+ Tree, 선행 기록(write-ahead logging, WAL)과 트랜잭션(transaction) 복구를 연결합니다. 별도의 RESP 브리지는 `server:ip`라는 한 키에 IPv4 값을 저장하고 읽는 통합 사례를 제공합니다.

- **저장 경로**: 페이지 단위 파일 접근 → 버퍼 풀 → B+ Tree → 트랜잭션과 WAL.
- **복구 경로**: 시작 시 로그를 읽고 커밋된 쓰기를 다시 적용하며, 완료되지 않은 쓰기는 이전 값으로 되돌립니다.
- **범위**: 두 부분으로 되어 있습니다. ① 정수 키 · 값 저장 엔진(pager · buffer pool · 디스크 B+ tree · WAL · transaction)과 `server:ip` 통신 브리지 ② SQL 처리기(INSERT · SELECT), 메모리 B+ tree 인덱스, thread pool API 서버. ②는 아래 "SQL 처리기 · 인덱스 · API 서버" 절을 봅니다.

### 실행 방법

아래 명령은 프로젝트 루트에서 실행합니다. Docker 이미지가 없다면 먼저 빌드합니다. 테스트는 `mini_dbms` 폴더의 `test_db.bin`, `test_buffer_pool.bin`, `test_bplus_tree.bin`, `test_transaction.db`, `test_transaction.wal`을 삭제하거나 새로 만듭니다. 같은 이름의 보관할 파일이 있다면 먼저 다른 곳에 보관하거나 프로젝트 사본에서 실행하세요.

```powershell
# Docker 이미지 생성
docker build -t mini-redis-dev .

# DBMS 빌드와 네 종류의 단위 테스트 실행
docker run --rm -v "${PWD}:/workspace" -w /workspace/mini_dbms mini-redis-dev bash -lc "make all && make test"

# 별도로 메모리 검사 실행: 각 테스트의 Valgrind 결과와 종료 상태를 확인
docker run --rm -v "${PWD}:/workspace" -w /workspace/mini_dbms mini-redis-dev make valgrind

# 통합 브리지 실행. 데이터와 WAL 파일은 mini_dbms 폴더에 남음
docker run --rm -it -p 6380:6380 -v "${PWD}:/workspace" -w /workspace/mini_dbms mini-redis-dev ./mini_dbms_server
```

테스트 명령의 성공 여부는 실행 당시 출력과 종료 상태로 확인합니다. `make valgrind`가 있다는 사실만으로 누수가 없다고 판정할 수 없습니다. 서버는 선택적으로 `./mini_dbms_server 데이터파일 WAL파일` 경로를 받으며, 기본값은 `mini_dbms.db`와 `mini_dbms.wal`입니다.

### 폴더 구조

```text
mini_dbms/
├── include/
│   ├── page.h              # 4096바이트 페이지 정의
│   ├── pager.h             # 페이지 파일 읽기와 쓰기 인터페이스
│   ├── buffer_pool.h       # 프레임, 핀, 플러시 인터페이스
│   ├── bplus_tree.h        # 정수 키·값 B+ Tree 노드와 연산
│   ├── wal.h               # WAL 레코드와 체크섬
│   ├── transaction.h       # 시작, 삽입, 커밋, 중단, 복구
│   └── server_bridge.h     # 통합 브리지 진입점
├── src/
│   ├── pager.c / buffer_pool.c / bplus_tree.c
│   ├── wal.c / transaction.c
│   └── main.c / server_bridge.c
├── tests/
│   ├── test_pager.c / test_buffer_pool.c
│   └── test_bplus_tree.c / test_transaction.c
└── Makefile                # 빌드와 테스트 대상
```

### 데이터 흐름

```text
[브리지 SET server:ip 값] → IPv4 검사와 정수 변환 → 트랜잭션 시작
                                ↓
[쓰기] 변경 전·후 값을 WAL에 추가 → WAL fsync → B+ Tree 삽입
                                ↓
[커밋] COMMIT 레코드 추가 → WAL fsync → 성공 응답
                                ↓
[페이지 저장] 더티 프레임 교체 또는 flush → Pager → 데이터 파일

[재시작] 데이터 파일과 WAL 열기 → 로그 검사 → 커밋 재적용 / 미완료 작업 되돌리기
[읽기] GET server:ip → B+ Tree 탐색 → 정수를 IPv4 문자열로 변환 → 응답
```

이 그림은 주요 경로만 나타냅니다. 오류 처리, 페이지 분할, 프레임 획득과 반환은 생략했습니다. 브리지의 `DEL`은 해당 키에 예약된 값 `0`을 트랜잭션으로 쓰는 방식이며, 범용 삭제 요청을 노출하지 않습니다.

### 핵심 개념과 설계 원리

#### 1. Pager와 고정 페이지

파일은 4096바이트 페이지로 나뉘며 Pager는 페이지 번호에 크기를 곱해 파일 위치(offset)를 구합니다. 읽기와 쓰기는 한 페이지 전체를 처리하고, 파일 크기가 페이지 크기의 배수가 아니면 열기를 거부합니다. 짧은 입출력은 반복 처리하고 위치 계산의 범위를 검사합니다.

서버는 B+ Tree의 루트 페이지로 0번을 사용합니다. 이 구현의 0번을 별도의 메타데이터 전용 페이지라고 해석하면 안 됩니다. 근거는 `src/main.c`의 `bplus_tree_create(pool, 0)`과 `src/bplus_tree.c`의 루트 초기화입니다.

#### 2. Buffer Pool과 페이지 교체

페이지를 요청할 때 메모리에 있으면 해당 프레임을 다시 사용하고, 없으면 Pager에서 읽어 옵니다. 기본 프레임 수는 8개이며 호출자가 다른 크기를 정할 수 있습니다. 핀 수(pin count)가 0인 프레임 가운데 최근 사용 시각이 가장 오래된 것을 교체 후보로 고릅니다. 변경된 페이지는 더티(dirty)로 표시하고 교체 전 또는 명시적 flush 때 파일에 씁니다.

핀은 페이지를 사용하는 동안 프레임이 교체 후보가 되지 않도록 하는 장치입니다. 모든 프레임이 핀으로 잡혀 있으면 새 페이지 요청은 실패할 수 있으므로 사용 후 `buffer_pool_unpin_page`를 호출해야 합니다. 데이터 파일 쓰기와 장치에 대한 동기화는 서로 다른 작업이며, Pager의 페이지 쓰기 자체가 `fsync`를 호출하지는 않습니다.

#### 3. B+ Tree와 정수 키

내부 노드(internal node)는 검색할 자식 페이지를 가리키고, 리프 노드(leaf node)는 `uint64_t` 키와 값을 보관합니다. 페이지가 차면 삽입 과정에서 노드를 나누고 부모를 갱신하며, 리프에는 다음 리프 번호도 저장합니다. `bplus_tree_find`, `bplus_tree_insert`, `bplus_tree_delete`가 제공되지만 공개 범위 검색 API는 확인되지 않습니다.

예를 들어 키 `10`에 값 `20`을 넣고 같은 키로 다시 찾으면 값 `20`을 기대합니다. 이는 동작을 설명하는 예시이며 이번 작업에서 실행한 결과는 아닙니다. 삭제 코드는 리프에서 항목을 지우지만 삭제 후 노드 병합과 재균형은 구현하지 않습니다.

#### 4. WAL과 복구

WAL은 변경 이전·이후 값과 작업 번호를 데이터 변경보다 먼저 기록하는 로그입니다. `txn_insert`는 INSERT 레코드를 추가하고 `fsync`한 뒤 트리를 바꾸며, `txn_commit`은 COMMIT 레코드를 추가하고 `fsync`한 뒤 성공을 반환합니다. 커밋 시 모든 데이터 페이지를 곧바로 디스크에 쓰는 방식과는 다릅니다.

재시작 때 `txn_recover`는 로그를 읽어 커밋된 삽입을 다시 적용하고, 완료되지 않았거나 중단된 작업은 이전 이미지(before-image)를 이용해 되돌립니다. WAL 레코드는 CRC32 체크섬을 지니며, 불완전한 끝부분은 잘라내고 완성된 레코드의 체크섬 불일치는 오류로 처리합니다. 이는 코드에 있는 복구 절차이며 전체 ARIES 구현이나 모든 장애 상황에서의 무손실 보장을 뜻하지 않습니다.

### 입력 검사와 예외 상황

| 상황 | 코드에 구현된 처리 |
|---|---|
| 페이지 크기의 배수가 아닌 데이터 파일 | `pager_open` 실패 |
| 핀으로 잡힌 프레임만 남음 | 교체할 프레임을 찾지 못해 페이지 요청 실패 |
| 기존 키에 다시 삽입 | 해당 키의 값을 갱신 |
| 존재하지 않는 키 조회 | `bplus_tree_find`가 실패를 반환 |
| 진행 중인 트랜잭션에서 다시 시작 | `txn_begin`이 `EBUSY`로 실패 |
| 로그의 불완전한 끝부분 | WAL 열기 과정에서 마지막 완성 레코드 뒤를 잘라냄 |
| 로그의 완성 레코드 체크섬 불일치 | 오류를 반환하며 WAL 열기 실패 |
| 브리지에 다른 키 또는 잘못된 IPv4 값 전달 | 오류 응답 반환 |

### 알려진 한계와 다음 단계

- **동시 트랜잭션**: 활성 트랜잭션은 한 번에 하나입니다. 다중 트랜잭션 격리와 잠금 관리자는 제공하지 않습니다.
- **저장 범위**: 저장 엔진의 트리는 정수 키와 값을 다루며, 브리지는 `server:ip`와 IPv4 값에 한정됩니다. 문자열 레코드는 SQL 처리기가 별도의 테이블 파일(`<테이블>.tbl`)에 저장합니다. SQL 처리기는 WAL을 쓰지 않습니다.
- **트리 삭제**: 리프 항목 삭제 뒤 병합과 재균형은 없습니다. 리프 연결은 있으나 공개 범위 검색 API는 확인되지 않습니다.
- **로그 관리**: 체크포인트와 로그 축소 기능은 확인되지 않아 계속 쓰면 로그가 커집니다.
- **내구성 검증**: 장애 주입, 전원 차단, 파일 시스템별 동작은 이번에 검증하지 않았습니다.

`make test`를 실행한 뒤 네 테스트의 출력과 종료 상태를 확인하고, 복구를 더 살펴보려면 `tests/test_transaction.c`의 시나리오와 `txn_recover`를 함께 대조하세요.

## SQL 처리기 · 인덱스 · API 서버 (2026-11-11 추가)

요구사항의 Phase 1 · 2 · 3에 해당합니다. 세 단계가 같은 엔진(`sql_engine.c`)을 씁니다.

```text
Phase 1  SQL 파일 / -e / 표준 입력 → mini_dbms_sql(CLI) → sql_parser.c → sql_engine.c → <테이블>.tbl
Phase 2  INSERT 때 자동 id → mem_bptree.c 인덱스에 (id → 파일 위치) 등록 → WHERE id = ? 가 인덱스 사용
Phase 3  client → mini_dbms_sql_server(accept) → thread_pool.c 대기열 → worker thread → sql_engine.c
```

### 실행

```bash
make all                                         # mini_dbms_sql, mini_dbms_sql_server 포함
mkdir -p /tmp/db && cp data/users.schema /tmp/db/
./mini_dbms_sql /tmp/db examples/sample.sql      # SQL 파일 실행
./mini_dbms_sql /tmp/db -e "SELECT * FROM users WHERE id = 2;"
make bench                                       # 100만 건 INSERT 후 인덱스 vs 선형 탐색 비교
./mini_dbms_sql_server /tmp/db 6381 4            # port 6381, worker thread 4개
make test                                        # 기존 4개 + test_sql (5개 사례)
```

### Phase 1 — SQL 처리기

| 파일 | 하는 일 |
|---|---|
| `src/sql_parser.c` | SQL 문장을 토큰으로 나누고 `SqlStatement` 구조체로 바꾼다. script를 `;`에서 문장 단위로 자른다 |
| `src/sql_engine.c` | `SqlStatement`를 실행한다. INSERT는 파일 끝에 한 줄을 쓰고, SELECT는 파일에서 읽는다 |
| `src/sql_cli.c` | 명령줄에서 SQL 파일 · `-e` 문장 · 표준 입력을 받아 엔진에 넘긴다 |

- 지원 문장: `INSERT INTO t VALUES (...)`, `INSERT INTO t (컬럼, ...) VALUES (...)`, `SELECT * | 컬럼, ... | COUNT(*) FROM t [WHERE 컬럼 = 값]`.
- 테이블과 schema는 이미 있다고 가정합니다. `<data_dir>/<테이블>.schema`에 컬럼 이름을 쉼표로 적습니다. 첫 컬럼은 `id`여야 합니다(예: `data/users.schema`).
- 저장 형식: `<테이블>.tbl`의 한 줄이 레코드 한 건이고, 컬럼 값은 tab으로 나눕니다. 값 안의 tab · 줄바꿈 · 역슬래시는 엔진이 `\t` · `\n` · `\\` 두 글자로 바꿔 저장합니다.

### Phase 2 — B+ tree 인덱스

- `execute_insert`가 레코드에 id를 1부터 자동으로 붙이고, `mem_bptree_insert`로 "id → 그 줄이 파일에서 시작하는 위치"를 등록합니다.
- `WHERE id = 숫자`이면 `execute_select`가 `mem_bptree_find`로 위치를 찾아 `pread`로 그 한 줄만 읽습니다. 결과 끝에 `(1 row, index lookup)`이 붙습니다.
- 다른 컬럼 조건이면 `execute_select`가 파일의 모든 줄을 읽으며 비교합니다. 결과 끝에 `(N rows, full scan of M rows)`가 붙습니다.
- 인덱스는 메모리에만 있습니다(node 하나에 key 최대 64개). 프로그램을 다시 켜면 `rebuild_index`가 테이블 파일을 한 번 읽어 다시 만듭니다. 디스크 page 기반인 `bplus_tree.c`와는 별개입니다.

2026-11-11에 Docker(gcc 12)에서 `make bench`로 잰 값입니다. 기계에 따라 달라집니다.

| 항목 | 결과 |
|---|---|
| 100만 건 INSERT (SQL 문장을 하나씩 파싱 · 실행) | 2.65초 (초당 약 37만 건) |
| `SELECT * FROM users WHERE id = ?` (인덱스, 10000회 평균) | 0.0032 ms |
| `SELECT * FROM users WHERE name = ?` (선형 탐색, 10회 평균) | 80.2 ms |
| 선형 탐색 / 인덱스 | 약 25000배 |

### Phase 3 — API 서버와 thread pool

- 통신 형식: Mini Redis와 같은 RESP. `PING` → `+PONG`, `SQL "<문장>"` → 결과 글자(bulk string), 실패하면 `-ERR 이유`.
- main thread가 `accept`로 받은 접속을 `thread_pool_submit`으로 대기열(256칸)에 넣고, worker thread가 꺼내 그 접속의 요청을 실행합니다.
- 동시성: `sql_engine_execute`가 SELECT에는 읽기 lock(`pthread_rwlock_rdlock`), INSERT에는 쓰기 lock(`pthread_rwlock_wrlock`)을 잡습니다. SELECT끼리는 함께 실행되고, INSERT는 id 부여 · 파일 쓰기 · 인덱스 등록을 혼자 실행합니다.
- 서버는 INSERT에 성공이라고 답하기 전에 `fflush`로 레코드를 파일에 내려보냅니다. `fsync`는 하지 않으므로 전원이 갑자기 꺼지면 운영체제 buffer에 있던 레코드는 사라질 수 있습니다.
- 한가한 접속은 5초 뒤 서버가 끊습니다. 접속 하나가 worker 하나를 계속 붙잡는 일을 막습니다.
- gateway(`mini_React/integration_gateway.js`)의 `POST /api/sql`이 이 서버를 부르고, SELECT 결과를 Mini Redis에 TTL과 함께 넣습니다.

### 한계

- UPDATE · DELETE · JOIN · ORDER BY · 자료형 검사가 없습니다. 모든 값은 문자열로 저장하고 `=` 비교만 합니다.
- `SELECT *`의 결과를 모두 메모리에 모은 뒤 보냅니다. 100만 건 전체를 한 번에 조회하면 그만큼 메모리를 씁니다.
- SQL 처리기의 테이블 파일은 WAL · transaction과 연결되어 있지 않습니다.
