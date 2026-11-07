#ifndef MINI_REDIS_HASHTABLE_H
#define MINI_REDIS_HASHTABLE_H

#define CAPACITY 16

typedef struct Node {
    char *key;
    char *value;
    struct Node *next;
} Node;

typedef struct HashTable {
    Node *buckets[CAPACITY];
} HashTable;

/* Creates an empty table. Returns NULL if allocation fails. */
HashTable *ht_create(void);

/* Stores a copy of value for key. Returns 0 on success, -1 on failure. */
int ht_set(HashTable *table, const char *key, const char *value);

/* Returns the stored value, or NULL if key is absent or arguments are invalid. */
const char *ht_get(const HashTable *table, const char *key);

/* Removes key. Returns 1 if removed, 0 if absent, or -1 for invalid arguments. */
int ht_delete(HashTable *table, const char *key);

/* Frees the table and all keys and values it owns. */
void ht_destroy(HashTable *table);

/* Computes the bucket index using the specified polynomial rolling hash. */
unsigned int ht_hash(const char *key);

#endif
