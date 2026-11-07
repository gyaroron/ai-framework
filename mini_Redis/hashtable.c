#include "hashtable.h"

#include <stdlib.h>
#include <string.h>

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

int ht_set(HashTable *table, const char *key, const char *value)
{
    unsigned int index;
    Node *node;
    char *value_copy;

    if (table == NULL || key == NULL || value == NULL) {
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
