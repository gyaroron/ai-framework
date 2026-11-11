#ifndef MINI_REDIS_HASHTABLE_H
#define MINI_REDIS_HASHTABLE_H

#include <stddef.h>

#define CAPACITY 16

/* expires_at_ms 값이 이 값이면 "만료되지 않는 key"라는 뜻이다. */
#define HT_NO_EXPIRY 0LL
/* ht_ttl_ms 결과: key가 없을 때(-2), 만료 시각이 없는 key일 때(-1). */
#define HT_TTL_MISSING (-2LL)
#define HT_TTL_PERSISTENT (-1LL)

typedef struct Node {
    char *key;
    char *value;
    /* 만료 시각(호출자가 쓰는 시계의 밀리초 값). 만료가 없으면 HT_NO_EXPIRY. */
    long long expires_at_ms;
    struct Node *next;
} Node;

typedef struct HashTable {
    Node *buckets[CAPACITY];
} HashTable;

/* Creates an empty table. Returns NULL if allocation fails. */
HashTable *ht_create(void);

/* key에 value 복사본을 저장하고 만료 시각을 지운다. 성공 0, 실패 -1. */
int ht_set(HashTable *table, const char *key, const char *value);

/* ht_set과 같지만 시계가 expires_at_ms에 닿으면 key가 만료된다. */
int ht_set_with_expiry(HashTable *table, const char *key, const char *value,
                       long long expires_at_ms);

/* 만료 시각을 보지 않고 저장된 value를 돌려준다. key가 없으면 NULL. */
const char *ht_get(const HashTable *table, const char *key);

/*
 * 지연 만료(lazy expiration): now_ms 시점에 아직 살아 있는 value만 돌려준다.
 * 만료된 key는 이 호출에서 테이블에서 지우고 NULL을 돌려준다.
 */
const char *ht_get_at(HashTable *table, const char *key, long long now_ms);

/* 살아 있는 key의 만료 시각을 정한다. 정했으면 1, key가 없으면 0, 인자가 틀리면 -1. */
int ht_expire_at(HashTable *table, const char *key, long long expires_at_ms,
                 long long now_ms);

/* 살아 있는 key의 만료 시각을 지운다. 지웠으면 1, 지울 것이 없으면 0, 인자가 틀리면 -1. */
int ht_persist(HashTable *table, const char *key, long long now_ms);

/* 남은 밀리초, 또는 HT_TTL_PERSISTENT, 또는 HT_TTL_MISSING을 돌려준다. */
long long ht_ttl_ms(HashTable *table, const char *key, long long now_ms);

/* 능동 만료(active expiration): 만료 시각이 지난 key를 모두 지우고 지운 개수를 돌려준다. */
size_t ht_purge_expired(HashTable *table, long long now_ms);

/*
 * snapshot 저장: 살아 있는 모든 key를 path 파일에 쓴다.
 * 임시 파일에 다 쓴 뒤 rename으로 바꾸므로, 쓰는 도중 서버가 죽어도 이전 파일이 남는다.
 * now_ms는 테이블이 쓰는 시계, wall_now_ms는 벽시계(1970년부터의 밀리초)의 현재 값이다.
 * 저장한 key 개수를 돌려주고, 실패하면 -1을 돌려준다.
 */
long ht_save(const HashTable *table, const char *path, long long now_ms,
             long long wall_now_ms);

/*
 * snapshot 읽기: path 파일의 key를 테이블에 넣는다. 꺼져 있는 동안 만료된 key는 버린다.
 * 파일이 없으면 0, 읽은 key 개수, 파일이 깨졌거나 메모리가 모자라면 -1을 돌려준다.
 */
long ht_load(HashTable *table, const char *path, long long now_ms,
             long long wall_now_ms);

/* Removes key. Returns 1 if removed, 0 if absent, or -1 for invalid arguments. */
int ht_delete(HashTable *table, const char *key);

/* Frees the table and all keys and values it owns. */
void ht_destroy(HashTable *table);

/* Computes the bucket index using the specified polynomial rolling hash. */
unsigned int ht_hash(const char *key);

#endif
