#include "hashtable.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); \
            return 1; \
        } \
    } while (0)

/* "a"와 "q"는 글자 값이 16 차이 나서 ht_hash가 같은 bucket에 넣는다(충돌 테스트용). */
#define COLLIDING_KEY_ONE "a"
#define COLLIDING_KEY_TWO "q"

/* 저장 · 조회 · 덮어쓰기 · 삭제의 기본 동작. */
static int test_set_get_delete(void)
{
    HashTable *table = ht_create();

    CHECK(table != NULL, "ht_create should return a table");
    CHECK(ht_get(table, "missing") == NULL, "missing key should return NULL");
    CHECK(ht_set(table, "server:ip", "192.168.0.1") == 0, "set should succeed");
    CHECK(strcmp(ht_get(table, "server:ip"), "192.168.0.1") == 0,
          "get should return the stored value");
    CHECK(ht_set(table, "server:ip", "10.0.0.1") == 0, "overwrite should succeed");
    CHECK(strcmp(ht_get(table, "server:ip"), "10.0.0.1") == 0,
          "get should return the overwritten value");
    CHECK(ht_delete(table, "server:ip") == 1, "delete should report 1");
    CHECK(ht_get(table, "server:ip") == NULL, "deleted key should be absent");
    CHECK(ht_delete(table, "server:ip") == 0, "second delete should report 0");
    ht_destroy(table);
    puts("PASS 1: set, get, overwrite, delete");
    return 0;
}

/* 같은 bucket에 들어간 두 key가 서로 섞이지 않는지(chaining). */
static int test_collisions(void)
{
    HashTable *table = ht_create();

    CHECK(table != NULL, "ht_create should return a table");
    CHECK(ht_hash(COLLIDING_KEY_ONE) == ht_hash(COLLIDING_KEY_TWO),
          "test keys should share one bucket");
    CHECK(ht_set(table, COLLIDING_KEY_ONE, "first") == 0, "first set");
    CHECK(ht_set(table, COLLIDING_KEY_TWO, "second") == 0, "second set");
    CHECK(strcmp(ht_get(table, COLLIDING_KEY_ONE), "first") == 0,
          "first colliding key keeps its value");
    CHECK(strcmp(ht_get(table, COLLIDING_KEY_TWO), "second") == 0,
          "second colliding key keeps its value");
    /* 먼저 넣은 항목이 연결 리스트의 뒤쪽에 있다. 그것부터 지워 본다. */
    CHECK(ht_delete(table, COLLIDING_KEY_ONE) == 1, "delete chain tail");
    CHECK(ht_get(table, COLLIDING_KEY_ONE) == NULL, "tail is gone");
    CHECK(strcmp(ht_get(table, COLLIDING_KEY_TWO), "second") == 0,
          "chain head survives deleting the tail");
    ht_destroy(table);
    puts("PASS 2: two keys in one bucket stay separate");
    return 0;
}

/* NULL 인자, 빈 문자열, 테이블이 값을 복사해 갖는지. */
static int test_edge_cases(void)
{
    HashTable *table = ht_create();
    const char *original = "value";
    char buffer[8];

    CHECK(table != NULL, "ht_create should return a table");
    CHECK(ht_set(NULL, "k", "v") == -1, "NULL table is rejected");
    CHECK(ht_set(table, NULL, "v") == -1, "NULL key is rejected");
    CHECK(ht_set(table, "k", NULL) == -1, "NULL value is rejected");
    CHECK(ht_get(NULL, "k") == NULL, "get on NULL table returns NULL");
    CHECK(ht_get(table, NULL) == NULL, "get with NULL key returns NULL");
    CHECK(ht_delete(NULL, "k") == -1, "delete on NULL table is rejected");
    CHECK(ht_delete(table, NULL) == -1, "delete with NULL key is rejected");
    CHECK(ht_hash(NULL) == 0, "hash of NULL is 0");
    CHECK(ht_hash("anything") < CAPACITY, "hash stays inside the bucket range");

    CHECK(ht_set(table, "", "empty key") == 0, "empty key is storable");
    CHECK(strcmp(ht_get(table, ""), "empty key") == 0, "empty key is readable");
    CHECK(ht_set(table, "empty", "") == 0, "empty value is storable");
    CHECK(strcmp(ht_get(table, "empty"), "") == 0, "empty value is readable");

    /* 테이블은 복사본을 가져야 한다. 호출자의 buffer를 바꿔도 저장된 값은 그대로다. */
    strcpy(buffer, original);
    CHECK(ht_set(table, "copy", buffer) == 0, "set from a mutable buffer");
    buffer[0] = 'X';
    CHECK(strcmp(ht_get(table, "copy"), original) == 0,
          "stored value is a private copy");
    ht_destroy(table);
    ht_destroy(NULL);
    puts("PASS 3: NULL arguments, empty strings, private copies");
    return 0;
}

/* bucket 16개에 key 1000개: 한 bucket에 여러 개가 길게 이어져도 정확한지. */
static int test_many_keys(void)
{
    HashTable *table = ht_create();
    char key[32];
    char value[32];
    int i;

    CHECK(table != NULL, "ht_create should return a table");
    for (i = 0; i < 1000; ++i) {
        sprintf(key, "key:%d", i);
        sprintf(value, "value:%d", i);
        CHECK(ht_set(table, key, value) == 0, "bulk set");
    }
    for (i = 0; i < 1000; ++i) {
        sprintf(key, "key:%d", i);
        sprintf(value, "value:%d", i);
        CHECK(strcmp(ht_get(table, key), value) == 0, "bulk get");
    }
    for (i = 0; i < 1000; i += 2) {
        sprintf(key, "key:%d", i);
        CHECK(ht_delete(table, key) == 1, "bulk delete of even keys");
    }
    for (i = 0; i < 1000; ++i) {
        sprintf(key, "key:%d", i);
        CHECK((ht_get(table, key) == NULL) == (i % 2 == 0),
              "only odd keys remain");
    }
    ht_destroy(table);
    puts("PASS 4: 1000 keys across 16 buckets");
    return 0;
}

/* 지연 만료: 보관 기간이 지난 값을 요청하면 지우고 "없음"으로 답하는지. */
static int test_lazy_expiration(void)
{
    HashTable *table = ht_create();

    CHECK(table != NULL, "ht_create should return a table");
    CHECK(ht_set_with_expiry(table, "session", "abc", 1500) == 0,
          "set with a deadline");
    CHECK(ht_set_with_expiry(table, "bad", "x", -1) == -1,
          "negative deadline is rejected");
    CHECK(strcmp(ht_get_at(table, "session", 1000), "abc") == 0,
          "value is readable before the deadline");
    CHECK(ht_ttl_ms(table, "session", 1000) == 500, "500 ms remain");
    CHECK(strcmp(ht_get_at(table, "session", 1499), "abc") == 0,
          "value is readable 1 ms before the deadline");
    CHECK(ht_get_at(table, "session", 1500) == NULL,
          "value is gone exactly at the deadline");
    /* ht_get은 만료 시각을 보지 않는다. 그런데도 NULL이면 항목이 실제로 지워진 것이다. */
    CHECK(ht_get(table, "session") == NULL,
          "expired entry was removed from the table");
    CHECK(ht_ttl_ms(table, "session", 1500) == HT_TTL_MISSING,
          "expired key reports missing");
    ht_destroy(table);
    puts("PASS 5: expired value is removed when it is requested");
    return 0;
}

/* 만료 시각 정하기 · 지우기 · 덮어쓰기. */
static int test_expire_persist_overwrite(void)
{
    HashTable *table = ht_create();

    CHECK(table != NULL, "ht_create should return a table");
    CHECK(ht_expire_at(table, "missing", 2000, 1000) == 0,
          "expire on a missing key reports 0");
    CHECK(ht_set(table, "k", "v") == 0, "plain set");
    CHECK(ht_ttl_ms(table, "k", 1000) == HT_TTL_PERSISTENT,
          "plain set has no deadline");
    CHECK(ht_expire_at(table, "k", 0, 1000) == -1, "deadline 0 is rejected");
    CHECK(ht_expire_at(table, "k", 2000, 1000) == 1, "expire sets a deadline");
    CHECK(ht_ttl_ms(table, "k", 1000) == 1000, "1000 ms remain");
    CHECK(ht_persist(table, "k", 1000) == 1, "persist removes the deadline");
    CHECK(ht_persist(table, "k", 1000) == 0, "second persist reports 0");
    CHECK(strcmp(ht_get_at(table, "k", 999999), "v") == 0,
          "persisted key outlives the old deadline");

    CHECK(ht_expire_at(table, "k", 2000, 1000) == 1, "expire again");
    CHECK(ht_set(table, "k", "fresh") == 0, "plain set over an expiring key");
    CHECK(ht_ttl_ms(table, "k", 1000) == HT_TTL_PERSISTENT,
          "plain set clears the deadline");

    CHECK(ht_expire_at(table, "k", 2000, 1000) == 1, "expire a third time");
    CHECK(ht_expire_at(table, "k", 9000, 2000) == 0,
          "expire on an already expired key reports 0");
    CHECK(ht_get(table, "k") == NULL, "that key was removed");
    ht_destroy(table);
    puts("PASS 6: expire, persist, and overwrite change the deadline");
    return 0;
}

/* 능동 만료: 요청이 없어도 만료된 key를 한꺼번에 지우는지. */
static int test_active_expiration(void)
{
    HashTable *table = ht_create();

    CHECK(table != NULL, "ht_create should return a table");
    CHECK(ht_purge_expired(NULL, 1000) == 0, "purge on NULL table removes 0");
    CHECK(ht_set_with_expiry(table, COLLIDING_KEY_ONE, "old", 1000) == 0, "set a");
    CHECK(ht_set(table, COLLIDING_KEY_TWO, "forever") == 0, "set q");
    CHECK(ht_set_with_expiry(table, "later", "x", 5000) == 0, "set later");
    CHECK(ht_set_with_expiry(table, "soon", "y", 1200) == 0, "set soon");
    CHECK(ht_purge_expired(table, 999) == 0, "nothing expires before 1000");
    CHECK(ht_purge_expired(table, 1200) == 2, "two keys expire by 1200");
    CHECK(ht_get(table, COLLIDING_KEY_ONE) == NULL, "a was purged");
    CHECK(ht_get(table, "soon") == NULL, "soon was purged");
    CHECK(strcmp(ht_get(table, COLLIDING_KEY_TWO), "forever") == 0,
          "key without a deadline survives in the same bucket");
    CHECK(strcmp(ht_get(table, "later"), "x") == 0, "later is still stored");
    CHECK(ht_purge_expired(table, 5000) == 1, "later expires at 5000");
    ht_destroy(table);
    puts("PASS 7: purge removes every expired key without a request");
    return 0;
}

#define SNAPSHOT_PATH "test_hashtable.snapshot"

/* snapshot 파일에 저장했다가 새 테이블로 다시 읽는다. */
static int test_snapshot_round_trip(void)
{
    HashTable *table = ht_create();
    HashTable *restored = ht_create();
    HashTable *late = ht_create();
    FILE *file;

    CHECK(table != NULL && restored != NULL && late != NULL, "ht_create");
    (void)remove(SNAPSHOT_PATH);
    CHECK(ht_load(restored, SNAPSHOT_PATH, 0, 0) == 0, "missing file loads 0 keys");

    CHECK(ht_set(table, "plain", "kept forever") == 0, "set plain");
    CHECK(ht_set(table, "multi line", "a b\nc") == 0, "set value with space and newline");
    CHECK(ht_set(table, "empty", "") == 0, "set empty value");
    CHECK(ht_set_with_expiry(table, "session", "abc", 1500) == 0, "set session");
    CHECK(ht_set_with_expiry(table, "gone", "x", 900) == 0, "set already expired key");
    /* 테이블 시계 1000, 벽시계 50000일 때 저장한다. session은 500ms 남았다. */
    CHECK(ht_save(table, SNAPSHOT_PATH, 1000, 50000) == 4, "four live keys are saved");

    /* 재시작: 테이블 시계는 7에서 다시 시작하고 벽시계는 200ms 흘렀다. */
    CHECK(ht_load(restored, SNAPSHOT_PATH, 7, 50200) == 4, "four keys are loaded");
    CHECK(strcmp(ht_get(restored, "plain"), "kept forever") == 0, "plain value");
    CHECK(strcmp(ht_get(restored, "multi line"), "a b\nc") == 0, "newline value");
    CHECK(strcmp(ht_get(restored, "empty"), "") == 0, "empty value");
    CHECK(ht_get(restored, "gone") == NULL, "expired key was not saved");
    CHECK(ht_ttl_ms(restored, "session", 7) == 300, "300 ms remain after restart");
    CHECK(ht_ttl_ms(restored, "plain", 7) == HT_TTL_PERSISTENT, "plain has no deadline");

    /* 벽시계가 600ms 흐른 뒤 재시작하면 session은 꺼져 있는 동안 만료된 것이다. */
    CHECK(ht_load(late, SNAPSHOT_PATH, 7, 50600) == 3, "key expired while down is dropped");
    CHECK(ht_get(late, "session") == NULL, "session is absent");

    file = fopen(SNAPSHOT_PATH, "wb");
    CHECK(file != NULL, "open snapshot for damage");
    fputs("not a snapshot\n", file);
    fclose(file);
    CHECK(ht_load(late, SNAPSHOT_PATH, 7, 50600) == -1, "damaged file is rejected");
    CHECK(ht_save(NULL, SNAPSHOT_PATH, 0, 0) == -1, "NULL table is rejected");

    (void)remove(SNAPSHOT_PATH);
    ht_destroy(table);
    ht_destroy(restored);
    ht_destroy(late);
    puts("PASS 8: snapshot keeps values and remaining TTL across a restart");
    return 0;
}

int main(void)
{
    if (test_set_get_delete() != 0 || test_collisions() != 0 ||
        test_edge_cases() != 0 || test_many_keys() != 0 ||
        test_lazy_expiration() != 0 || test_expire_persist_overwrite() != 0 ||
        test_active_expiration() != 0 ||
        test_snapshot_round_trip() != 0) {
        return 1;
    }
    puts("All hash table tests passed.");
    return 0;
}
