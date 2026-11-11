/* fileno · fsync를 쓰려면 POSIX 기능을 켜야 한다. */
#define _POSIX_C_SOURCE 200809L

#include "hashtable.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* snapshot 파일 첫 줄. 형식이 바뀌면 숫자를 올린다. */
#define SNAPSHOT_MAGIC "MINIREDIS-SNAPSHOT-1"
/* snapshot에서 읽을 key · value 하나의 최대 길이(byte). 깨진 파일이 큰 메모리를 요구하지 못하게 막는다. */
#define SNAPSHOT_MAX_FIELD (1024L * 1024L)

static char *copy_string(const char *source)
{
    size_t length = strlen(source) + 1;
    char *copy = (char *)malloc(length);

    if (copy != NULL) {
        memcpy(copy, source, length);
    }
    return copy;
}

unsigned int ht_hash(const char *key)
{
    unsigned int hash = 0;

    if (key == NULL) {
        return 0;
    }

    while (*key != '\0') {
        hash = (hash * 31u + (unsigned char)*key) % CAPACITY;
        ++key;
    }
    return hash;
}

HashTable *ht_create(void)
{
    /* calloc initializes every bucket head to NULL. */
    return (HashTable *)calloc(1, sizeof(HashTable));
}

static void free_node(Node *node)
{
    free(node->key);
    free(node->value);
    free(node);
}

static int is_expired(const Node *node, long long now_ms)
{
    return node->expires_at_ms != HT_NO_EXPIRY && node->expires_at_ms <= now_ms;
}

/* key를 찾는다. 만료 시각이 지난 항목은 연결 리스트에서 떼어 지우고 "없음"으로 답한다. */
static Node *find_live(HashTable *table, const char *key, long long now_ms)
{
    unsigned int index = ht_hash(key);
    Node *node = table->buckets[index];
    Node *previous = NULL;

    while (node != NULL) {
        if (strcmp(node->key, key) == 0) {
            if (!is_expired(node, now_ms)) {
                return node;
            }
            if (previous == NULL) {
                table->buckets[index] = node->next;
            } else {
                previous->next = node->next;
            }
            free_node(node);
            return NULL;
        }
        previous = node;
        node = node->next;
    }
    return NULL;
}

int ht_set(HashTable *table, const char *key, const char *value)
{
    return ht_set_with_expiry(table, key, value, HT_NO_EXPIRY);
}

int ht_set_with_expiry(HashTable *table, const char *key, const char *value,
                       long long expires_at_ms)
{
    unsigned int index;
    Node *node;
    char *value_copy;

    if (table == NULL || key == NULL || value == NULL ||
        expires_at_ms < HT_NO_EXPIRY) {
        return -1;
    }

    index = ht_hash(key);
    node = table->buckets[index];
    while (node != NULL) {
        if (strcmp(node->key, key) == 0) {
            value_copy = copy_string(value);
            if (value_copy == NULL) {
                return -1;
            }
            free(node->value);
            node->value = value_copy;
            node->expires_at_ms = expires_at_ms;
            return 0;
        }
        node = node->next;
    }

    node = (Node *)malloc(sizeof(Node));
    if (node == NULL) {
        return -1;
    }
    node->key = copy_string(key);
    if (node->key == NULL) {
        free(node);
        return -1;
    }
    node->value = copy_string(value);
    if (node->value == NULL) {
        free(node->key);
        free(node);
        return -1;
    }

    node->expires_at_ms = expires_at_ms;

    /* New colliding entries are inserted at the head of the chain. */
    node->next = table->buckets[index];
    table->buckets[index] = node;
    return 0;
}

const char *ht_get(const HashTable *table, const char *key)
{
    unsigned int index;
    const Node *node;

    if (table == NULL || key == NULL) {
        return NULL;
    }

    index = ht_hash(key);
    node = table->buckets[index];
    while (node != NULL) {
        if (strcmp(node->key, key) == 0) {
            return node->value;
        }
        node = node->next;
    }
    return NULL;
}

const char *ht_get_at(HashTable *table, const char *key, long long now_ms)
{
    Node *node;

    if (table == NULL || key == NULL) {
        return NULL;
    }
    node = find_live(table, key, now_ms);
    return node == NULL ? NULL : node->value;
}

int ht_expire_at(HashTable *table, const char *key, long long expires_at_ms,
                 long long now_ms)
{
    Node *node;

    if (table == NULL || key == NULL || expires_at_ms <= HT_NO_EXPIRY) {
        return -1;
    }
    node = find_live(table, key, now_ms);
    if (node == NULL) {
        return 0;
    }
    node->expires_at_ms = expires_at_ms;
    return 1;
}

int ht_persist(HashTable *table, const char *key, long long now_ms)
{
    Node *node;

    if (table == NULL || key == NULL) {
        return -1;
    }
    node = find_live(table, key, now_ms);
    if (node == NULL || node->expires_at_ms == HT_NO_EXPIRY) {
        return 0;
    }
    node->expires_at_ms = HT_NO_EXPIRY;
    return 1;
}

long long ht_ttl_ms(HashTable *table, const char *key, long long now_ms)
{
    Node *node;

    if (table == NULL || key == NULL) {
        return HT_TTL_MISSING;
    }
    node = find_live(table, key, now_ms);
    if (node == NULL) {
        return HT_TTL_MISSING;
    }
    if (node->expires_at_ms == HT_NO_EXPIRY) {
        return HT_TTL_PERSISTENT;
    }
    return node->expires_at_ms - now_ms;
}

/* snapshot 한 항목을 읽어 n byte 문자열로 만든다. 실패하면 NULL. */
static char *read_field(FILE *file, long length)
{
    char *text = (char *)malloc((size_t)length + 1);

    if (text == NULL) {
        return NULL;
    }
    if (length > 0 && fread(text, 1, (size_t)length, file) != (size_t)length) {
        free(text);
        return NULL;
    }
    text[length] = '\0';
    return text;
}

long ht_save(const HashTable *table, const char *path, long long now_ms,
             long long wall_now_ms)
{
    char *temp_path;
    size_t path_length;
    FILE *file;
    long saved = 0;
    unsigned int i;
    int failed = 0;

    if (table == NULL || path == NULL) {
        return -1;
    }
    path_length = strlen(path);
    temp_path = (char *)malloc(path_length + 5);
    if (temp_path == NULL) {
        return -1;
    }
    memcpy(temp_path, path, path_length);
    memcpy(temp_path + path_length, ".tmp", 5);

    file = fopen(temp_path, "wb");
    if (file == NULL) {
        free(temp_path);
        return -1;
    }
    if (fprintf(file, "%s\n", SNAPSHOT_MAGIC) < 0) {
        failed = 1;
    }
    for (i = 0; i < CAPACITY && !failed; ++i) {
        const Node *node;
        for (node = table->buckets[i]; node != NULL; node = node->next) {
            long long wall_deadline = 0;

            if (is_expired(node, now_ms)) {
                continue;
            }
            /* 테이블 시계의 남은 시간을 벽시계 시각으로 바꿔 적는다. 재시작하면 테이블 시계가 달라지기 때문이다. */
            if (node->expires_at_ms != HT_NO_EXPIRY) {
                wall_deadline = wall_now_ms + (node->expires_at_ms - now_ms);
            }
            if (fprintf(file, "%lu %lu %lld\n", (unsigned long)strlen(node->key),
                        (unsigned long)strlen(node->value), wall_deadline) < 0 ||
                fputs(node->key, file) == EOF || fputs(node->value, file) == EOF ||
                fputc('\n', file) == EOF) {
                failed = 1;
                break;
            }
            ++saved;
        }
    }
    /* 운영체제 buffer까지 디스크로 내려보낸 뒤에야 이전 파일을 바꾼다. */
    if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
        failed = 1;
    }
    if (fclose(file) != 0) {
        failed = 1;
    }
    if (!failed && rename(temp_path, path) != 0) {
        failed = 1;
    }
    if (failed) {
        (void)remove(temp_path);
    }
    free(temp_path);
    return failed ? -1 : saved;
}

long ht_load(HashTable *table, const char *path, long long now_ms,
             long long wall_now_ms)
{
    char magic[64];
    FILE *file;
    long loaded = 0;

    if (table == NULL || path == NULL) {
        return -1;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        /* 처음 실행이라 파일이 없는 것은 오류가 아니다. */
        return 0;
    }
    if (fgets(magic, sizeof(magic), file) == NULL ||
        strcmp(magic, SNAPSHOT_MAGIC "\n") != 0) {
        fclose(file);
        return -1;
    }
    for (;;) {
        unsigned long key_length;
        unsigned long value_length;
        long long wall_deadline;
        long long expires_at_ms = HT_NO_EXPIRY;
        char *key;
        char *value;
        int fields = fscanf(file, "%lu %lu %lld", &key_length, &value_length,
                            &wall_deadline);

        if (fields == EOF) {
            break;
        }
        if (fields != 3 || key_length > (unsigned long)SNAPSHOT_MAX_FIELD ||
            value_length > (unsigned long)SNAPSHOT_MAX_FIELD ||
            wall_deadline < 0 || fgetc(file) != '\n') {
            fclose(file);
            return -1;
        }
        key = read_field(file, (long)key_length);
        value = key == NULL ? NULL : read_field(file, (long)value_length);
        if (value == NULL || fgetc(file) != '\n') {
            free(key);
            free(value);
            fclose(file);
            return -1;
        }
        if (wall_deadline != 0) {
            long long remaining = wall_deadline - wall_now_ms;
            /* 서버가 꺼져 있는 동안 만료 시각이 지난 key는 되살리지 않는다. */
            if (remaining <= 0) {
                free(key);
                free(value);
                continue;
            }
            expires_at_ms = now_ms + remaining;
        }
        if (ht_set_with_expiry(table, key, value, expires_at_ms) != 0) {
            free(key);
            free(value);
            fclose(file);
            return -1;
        }
        free(key);
        free(value);
        ++loaded;
    }
    fclose(file);
    return loaded;
}

size_t ht_purge_expired(HashTable *table, long long now_ms)
{
    size_t removed = 0;
    unsigned int i;

    if (table == NULL) {
        return 0;
    }
    for (i = 0; i < CAPACITY; ++i) {
        Node **link = &table->buckets[i];
        while (*link != NULL) {
            Node *node = *link;
            if (is_expired(node, now_ms)) {
                *link = node->next;
                free_node(node);
                ++removed;
            } else {
                link = &node->next;
            }
        }
    }
    return removed;
}

int ht_delete(HashTable *table, const char *key)
{
    unsigned int index;
    Node *node;
    Node *previous = NULL;

    if (table == NULL || key == NULL) {
        return -1;
    }

    index = ht_hash(key);
    node = table->buckets[index];
    while (node != NULL) {
        if (strcmp(node->key, key) == 0) {
            if (previous == NULL) {
                table->buckets[index] = node->next;
            } else {
                previous->next = node->next;
            }
            free(node->key);
            free(node->value);
            free(node);
            return 1;
        }
        previous = node;
        node = node->next;
    }
    return 0;
}

void ht_destroy(HashTable *table)
{
    unsigned int i;

    if (table == NULL) {
        return;
    }

    for (i = 0; i < CAPACITY; ++i) {
        Node *node = table->buckets[i];
        while (node != NULL) {
            Node *next = node->next;
            free(node->key);
            free(node->value);
            free(node);
            node = next;
        }
    }
    free(table);
}
