# Mini Redis — 인메모리 캐시 서버

C99로 작성한 키-값 저장소(key-value store)와 TCP 서버입니다. 해시 테이블(hash table)에 문자열을 보관하고, RESP 요청을 받아 `PING`, `SET`, `GET`, `DEL`을 처리합니다. 서버는 `select`를 이용한 단일 스레드(single-threaded) 비차단(nonblocking) 루프에서 여러 연결을 다룹니다.

- `SET`은 키와 값을 복사하여 저장하고, 같은 키가 있으면 값을 바꿉니다.
- `GET`은 저장된 값을 반환하며 없는 키에는 RESP null bulk string을 응답합니다.
- `DEL`은 삭제 성공 여부를 1 또는 0으로 응답합니다.
- `PING`은 `PONG`을 보내거나 전달받은 인자를 돌려줍니다.

## 실행 방법

아래 명령은 `mini_redis` 디렉터리에서 실행합니다. TCP 서버는 POSIX 소켓 API를 사용하므로 Linux 환경과 C 컴파일러, `make`가 필요합니다. 명령은 Makefile에서 확인했으며 이번 작성 중 실행 결과는 확인하지 않았습니다.

```bash
make              # mini_redis 빌드
./mini_redis      # 해시 테이블 생성·삽입·조회·갱신·삭제 확인
make test         # RESP 파서의 완성·부분 수신·연속 요청 테스트
./mini_redis --server  # TCP 6379 포트에서 서버 실행
```

서버를 실행한 뒤 다른 터미널에서 `redis-cli -p 6379 PING`, `redis-cli -p 6379 SET name John`, `redis-cli -p 6379 GET name`을 차례로 보내 `PONG`, `OK`, `John`을 확인할 수 있습니다. 이 명령은 별도로 설치된 `redis-cli`가 필요하며, 여기서는 실행하지 않은 확인 절차입니다.

## 폴더 구조

```text
mini_redis/
├── hashtable.c / hashtable.h  # 16개 버킷의 해시 테이블과 문자열 소유권
├── resp.c / resp.h            # bulk string 배열 형태의 명령 파서
├── server.c / server.h        # select 기반 TCP 서버, 응답 작성, 명령 처리
├── main.c                     # 해시 테이블 확인과 서버 실행 진입점
├── test_resp.c                # RESP 파서 테스트 세 가지
├── Makefile                   # 빌드, 테스트, valgrind, 정리 명령
└── README.md                  # 해시 테이블 API와 포인터 수명 설명
```

## 데이터 흐름

```text
TCP 수신 → 클라이언트 입력 버퍼 → RESP 명령 파싱
                              ├─ 미완성: 다음 수신까지 보관
                              ├─ 오류: 오류 응답을 보낸 뒤 연결 종료
                              └─ 완성: PING / SET / GET / DEL 처리
                                       ↓
                              해시 테이블 읽기 또는 변경
                                       ↓
                              응답 버퍼 → TCP 전송
```

입력 버퍼에 요청이 여러 개 있으면 완성된 요청을 하나씩 처리하고 남은 바이트를 앞으로 옮깁니다. 각 요청은 같은 테이블을 사용합니다.

## 핵심 개념

### 1. 해시와 충돌 처리

`ht_hash`는 키의 각 바이트를 읽으며 `hash = (hash * 31 + byte) % 16`을 계산합니다. 같은 버킷(bucket)에 서로 다른 키가 들어오면 단일 연결 리스트(singly linked list)로 이어서 보관하고, 조회할 때 키를 비교하며 찾습니다. 따라서 키가 한 버킷에 몰리면 해당 리스트를 길게 순회할 수 있습니다. 16개 버킷은 고정되어 있어 데이터가 늘어도 자동 확장되지 않습니다.

### 2. 문자열 소유권과 포인터 수명

테이블은 `SET`에 전달된 키와 값을 새 메모리에 복사합니다. 기존 값을 바꿀 때는 새 값 복사에 성공한 뒤 이전 값을 해제합니다. `ht_get`이 반환하는 포인터는 테이블이 소유한 값을 가리키므로 해당 키의 값 갱신·삭제 또는 테이블 해제 뒤에는 사용할 수 없습니다.

### 3. RESP 요청과 응답

파서(parser)는 RESP의 모든 자료형을 일반적으로 해석하는 구현이 아닙니다. bulk string만 원소로 갖는 비어 있지 않은 배열 형태의 명령을 읽고, 완료·미완성·오류를 구분합니다. 요청 인자 수는 최대 1024개이며 개별 bulk 길이 제한은 1 MiB입니다. 다만 서버의 입력 버퍼는 64 KiB이므로 실제 서버에서 수용할 수 있는 요청 크기는 더 작습니다.

| 명령 | 인자 | 응답 예 |
|---|---|---|
| `PING` | 없음 또는 문자열 하나 | `+PONG\r\n` 또는 bulk string |
| `SET` | 키와 값 | `+OK\r\n` |
| `GET` | 키 하나 | bulk string 또는 `$-1\r\n` |
| `DEL` | 키 하나 | `:1\r\n` 또는 `:0\r\n` |

### 4. 단일 스레드 이벤트 루프

서버는 `select`로 읽거나 쓸 준비가 된 소켓을 찾고, 요청을 한 번에 하나씩 처리합니다. 처리 순서가 한 실행 흐름에 놓이는 것과 해시 테이블 자체가 스레드 안전(thread-safe)한 것은 다릅니다. README는 테이블이 스레드 안전하지 않다고 밝히므로 다른 멀티스레드 코드에서 공유할 때는 별도 동기화가 필요합니다.

## 입력 검사와 엣지 케이스

| 상황 | 코드에서 확인한 처리 |
|---|---|
| 같은 키에 다시 `SET` | 새 값 복사 성공 후 기존 값을 교체 |
| 없는 키의 `GET` / `DEL` | null bulk string / 정수 0 |
| 요청 일부만 도착 | 파싱을 보류하고 입력 버퍼에 남김 |
| 잘못된 RESP 형식 | `-ERR Protocol error`를 쓴 뒤 연결 종료 |
| 허용하지 않는 명령 또는 인자 수 | 오류 응답 |
| 입력 버퍼가 가득 참 | `request too large` 응답을 쓴 뒤 연결 종료 |
| 여러 요청이 한 번에 도착 | 완성된 요청을 순서대로 처리 |

## 알려진 한계와 확인 범위

- 데이터는 프로세스 메모리에 있습니다. `./mini_redis --server <파일>`로 실행하면 서버가 key가 바뀐 뒤 1초 안에 그 파일에 snapshot을 쓰고, 다시 시작할 때 읽습니다. 마지막 snapshot 이후 1초 안의 변경은 서버가 갑자기 죽으면 사라질 수 있습니다. 만료 시간(TTL)은 아래 "보관 기간(TTL)" 절을 봅니다.
- RESP 파서는 bulk string 배열 명령에 한정됩니다. `strlen`과 `strcmp`를 쓰므로 NUL 바이트를 포함한 임의의 바이너리 값은 보존하지 못합니다.
- `DEL`은 키 하나만 받습니다. 전체 Redis 명령이나 프로토콜 호환을 주장할 수 없습니다.
- `make test`는 `test_resp.c`(RESP 파서 3개 사례)와 `test_hashtable.c`(해시 테이블 8개 사례)를 실행합니다. 네트워크 동작은 서버를 띄운 뒤 `node test_server_ttl.js`로 검증합니다. 인자 없는 `./mini_redis`는 다섯 단계 해시 테이블 확인을 출력합니다.
- `make valgrind` 대상은 인자 없이 프로그램을 실행합니다. 이번 작업에서 누수 검사나 서버 통합 테스트는 실행하지 않았으므로 결과는 미확인입니다.

---

# Mini-Redis Hash Table

A C99 hash table implementation using 16 fixed buckets, the specified
polynomial rolling hash, and singly linked list chaining. Keys and values are
copied into table-owned memory.

## Build

```sh
cc -std=c99 -Wall -Wextra -Wpedantic hashtable.c main.c -o mini_redis
```

## API

- `ht_create` / `ht_destroy`: allocate and release a table.
- `ht_set`: add a key or replace its value; returns `0` on success and `-1` on
  invalid arguments or allocation failure.
- `ht_get`: return a borrowed pointer to the stored value, or `NULL` if absent.
  The pointer remains valid until that key is updated/deleted or the table is
  destroyed.
- `ht_delete`: return `1` if removed, `0` if absent, and `-1` for invalid
  arguments.
- `ht_hash`: return the bucket index in `[0, CAPACITY)`.

The table is not thread-safe. A returned value pointer must not be used after
an update or deletion of its key, or after `ht_destroy`.

## 보관 기간(TTL) · 다운 대비 · 성능 비교 (2026-11-11 추가)

### 보관 기간이 지난 값의 처리

| 명령 | 하는 일 | 응답 |
|---|---|---|
| `SET key value EX 초` · `SET key value PX 밀리초` | 서버가 값을 저장하고 만료 시각을 함께 적는다 | `+OK` |
| `EXPIRE key 초` · `PEXPIRE key 밀리초` | 서버가 살아 있는 key의 만료 시각을 정한다 | 정했으면 `:1`, key가 없으면 `:0` |
| `TTL key` · `PTTL key` | 서버가 남은 시간을 초 · 밀리초로 답한다 | key가 없으면 `:-2`, 만료 시각이 없으면 `:-1` |
| `PERSIST key` | 서버가 만료 시각을 지운다 | 지웠으면 `:1`, 아니면 `:0` |

- 만료 시각은 `hashtable.h`의 `Node.expires_at_ms`에 저장합니다. 값이 `HT_NO_EXPIRY`(0)이면 만료되지 않습니다.
- 지연 만료(lazy expiration): client가 `GET` · `TTL` · `EXPIRE` · `DEL`로 key를 요청하면 `hashtable.c`의 `find_live`가 만료 시각을 확인합니다. 시각이 지났으면 `find_live`가 그 항목을 연결 리스트에서 떼어 메모리를 해제하고, 서버는 없는 key와 똑같이 `$-1`로 답합니다.
- 능동 만료(active expiration): `server.c`의 event loop가 `select`에 1초 timeout을 주어 요청이 없어도 1초마다 깨어나고, `ht_purge_expired`로 만료된 key를 모두 지웁니다. 아무도 다시 요청하지 않는 key가 메모리에 계속 남는 일을 막습니다.
- 시계: TTL 계산은 `CLOCK_MONOTONIC`(사람이 시각을 바꿔도 뒤로 가지 않는 시계)을 씁니다. 벽시계를 쓰면 시각을 뒤로 돌렸을 때 만료가 미뤄지기 때문입니다.
- 일반 `SET key value`는 실제 Redis와 같이 그 key의 만료 시각을 지웁니다.

### 서버가 다운될 때를 대비한 snapshot (선택 과제)

- `./mini_redis --server /경로/redis.snapshot`으로 실행하면 서버가 ① 시작할 때 파일을 읽어 테이블을 채우고 ② key가 바뀐 뒤 1초 안에 파일을 다시 쓰고 ③ `SIGINT` · `SIGTERM`을 받으면 마지막으로 한 번 더 쓴 뒤 종료 코드 0으로 끝납니다.
- `ht_save`는 임시 파일(`.tmp`)에 모두 쓰고 `fsync`한 뒤 `rename`으로 바꿉니다. 쓰는 도중 서버가 죽어도 이전 파일이 그대로 남습니다.
- 만료 시각은 벽시계 시각으로 바꿔 저장합니다. `ht_load`는 서버가 꺼져 있는 동안 만료 시각이 지난 key를 되살리지 않습니다.
- 한계: 마지막 snapshot 이후 최대 1초의 변경은 전원이 갑자기 꺼지면 사라집니다. 명령마다 기록하는 방식(AOF, Append Only File)은 구현하지 않았습니다.

### 테스트와 성능 비교

```bash
make test                         # test_resp(파서) + test_hashtable(저장 · 조회 · 삭제 · TTL · snapshot 8개 사례)
./mini_redis --server &           # 서버를 띄운 뒤
node test_server_ttl.js           # 실제 TCP 연결로 TTL · 잘못된 인자 5개 사례
node bench_cache.js               # API 응답을 cache에 넣을 때와 안 넣을 때 비교
```

`bench_cache.js`는 작은 HTTP API 두 개를 만듭니다. `/origin`은 요청마다 원본을 읽고, `/cached`는 Mini Redis에 먼저 물은 뒤 없을 때만 원본을 읽어 API 응답을 `SET ... EX`로 저장합니다. 아래는 2026-11-11에 Docker(gcc 12) 안의 서버와 Windows의 Node.js v24로 잰 값입니다. 기계와 부하에 따라 달라집니다.

| 원본(origin) | cache 없이 평균 | cache 사용 평균 | 비율 |
|---|---|---|---|
| Mini DBMS bridge의 `GET server:ip` (요청 2000개) | 0.844 ms | 0.794 ms | 1.06배 |
| Mini DBMS SQL 서버, 100만 건 테이블의 `WHERE name = ?` 선형 탐색 (요청 100개) | 131.047 ms | 2.232 ms | 58.71배 |

원본이 이미 빠르면(key 하나를 B+ tree에서 찾기) cache의 이득이 거의 없고, 원본이 느릴수록(100만 건 선형 탐색) 이득이 커집니다.
