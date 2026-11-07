#ifndef MINI_DBMS_BPLUS_TREE_H
#define MINI_DBMS_BPLUS_TREE_H

#include <stdint.h>

#include "buffer_pool.h"

#define NODE_TYPE_INTERNAL 0x01u
#define NODE_TYPE_LEAF 0x02u
#define BPLUS_NO_PAGE UINT32_MAX
#define BPLUS_COMMON_HEADER_SIZE 8u
#define BPLUS_LEAF_HEADER_SIZE 16u
#define BPLUS_LEAF_MAX_KEYS ((PAGE_SIZE - BPLUS_LEAF_HEADER_SIZE) / 16u)
#define BPLUS_INTERNAL_MAX_KEYS ((PAGE_SIZE - 16u) / 12u)

typedef struct BPlusNodeHeader {
    uint16_t node_type;
    uint16_t num_keys;
    uint32_t parent_page_no;
} BPlusNodeHeader;

typedef struct BPlusLeafEntry {
    uint64_t key;
    uint64_t value;
} BPlusLeafEntry;

typedef struct BPlusLeafNode {
    BPlusNodeHeader header;
    uint32_t next_leaf_page_no;
    uint32_t reserved;
    BPlusLeafEntry entries[BPLUS_LEAF_MAX_KEYS];
} BPlusLeafNode;

typedef struct BPlusInternalNode {
    BPlusNodeHeader header;
    uint64_t keys[BPLUS_INTERNAL_MAX_KEYS];
    uint32_t children[BPLUS_INTERNAL_MAX_KEYS + 1u];
    uint32_t reserved;
} BPlusInternalNode;

typedef struct BPlusTree BPlusTree;

BPlusTree *bplus_tree_create(BufferPool *pool, uint32_t root_page_no);
int bplus_tree_find(BPlusTree *tree, uint64_t key, uint64_t *out_value);
int bplus_tree_insert(BPlusTree *tree, uint64_t key, uint64_t value);
int bplus_tree_delete(BPlusTree *tree, uint64_t key);
int bplus_tree_find_leaf_page(BPlusTree *tree, uint64_t key,
                              uint32_t *out_page_no);
void bplus_tree_destroy(BPlusTree *tree);

#endif
