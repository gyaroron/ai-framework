#define _POSIX_C_SOURCE 200809L

#include "bplus_tree.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST_FILE "test_bplus_tree.bin"
#define INSERT_COUNT 600u

static void read_leaf(BufferPool *pool, uint32_t page_no, BPlusLeafNode *out)
{
    Page *page = buffer_pool_get_page(pool, page_no);
    assert(page != NULL);
    memcpy(out, page->bytes, PAGE_SIZE);
    assert(buffer_pool_unpin_page(pool, page_no, 0) == 0);
    assert(out->header.node_type == NODE_TYPE_LEAF);
    assert(out->header.num_keys <= BPLUS_LEAF_MAX_KEYS);
}

static uint32_t find_leftmost_leaf(BufferPool *pool, uint32_t root_page_no)
{
    uint32_t page_no = root_page_no;

    for (;;) {
        uint32_t current_page_no = page_no;
        Page *page = buffer_pool_get_page(pool, page_no);
        BPlusNodeHeader header;
        assert(page != NULL);
        memcpy(&header, page->bytes, sizeof(header));
        if (header.node_type == NODE_TYPE_LEAF) {
            assert(buffer_pool_unpin_page(pool, page_no, 0) == 0);
            return page_no;
        }
        assert(header.node_type == NODE_TYPE_INTERNAL);
        memcpy(&page_no, page->bytes + sizeof(BPlusNodeHeader) +
               BPLUS_INTERNAL_MAX_KEYS * sizeof(uint64_t), sizeof(page_no));
        assert(buffer_pool_unpin_page(pool, current_page_no, 0) == 0);
    }
}

int main(void)
{
    Pager *pager;
    BufferPool *pool;
    BPlusTree *tree;
    uint64_t value;
    uint32_t i;
    uint32_t leaf_page_no;
    uint64_t previous_key = 0;
    uint32_t scanned = 0;
    uint32_t visited_pages = 0;

    (void)unlink(TEST_FILE);
    pager = pager_open(TEST_FILE);
    assert(pager != NULL);
    pool = buffer_pool_create(pager, 8);
    assert(pool != NULL);
    tree = bplus_tree_create(pool, 0);
    assert(tree != NULL);

    /* PASS 1: small ordered inserts and successful lookup in one leaf. */
    for (i = 10; i < 30; ++i) {
        assert(bplus_tree_insert(tree, i, (uint64_t)i * 100u) == 0);
    }
    for (i = 10; i < 30; ++i) {
        assert(bplus_tree_find(tree, i, &value) == 0);
        assert(value == (uint64_t)i * 100u);
    }
    puts("PASS 1: insert and find within a leaf page");

    /* PASS 2: absent keys report failure. */
    assert(bplus_tree_find(tree, 9999, &value) == -1);
    puts("PASS 2: missing key lookup returned failure");

    /* PASS 3: force multiple leaf splits, then verify every key/value. */
    for (i = 0; i < INSERT_COUNT; ++i) {
        assert(bplus_tree_insert(tree, i, (uint64_t)i + 5000u) == 0);
    }
    for (i = 0; i < INSERT_COUNT; ++i) {
        assert(bplus_tree_find(tree, i, &value) == 0);
        assert(value == (uint64_t)i + 5000u);
    }
    puts("PASS 3: split leaves and find every inserted key");

    /* PASS 4: walk the linked leaf chain and verify global key order. */
    leaf_page_no = find_leftmost_leaf(pool, 0);
    while (leaf_page_no != BPLUS_NO_PAGE) {
        BPlusLeafNode leaf;
        uint16_t entry_index;

        assert(++visited_pages < 100u);
        read_leaf(pool, leaf_page_no, &leaf);
        for (entry_index = 0; entry_index < leaf.header.num_keys; ++entry_index) {
            uint64_t key = leaf.entries[entry_index].key;
            if (scanned != 0) {
                assert(key > previous_key);
            }
            previous_key = key;
            ++scanned;
        }
        leaf_page_no = leaf.next_leaf_page_no;
    }
    assert(scanned == INSERT_COUNT);
    puts("PASS 4: next_leaf_page_no range scan preserved sorted keys");

    bplus_tree_destroy(tree);
    assert(buffer_pool_flush_all(pool) == 0);
    buffer_pool_destroy(pool);
    assert(pager_close(pager) == 0);
    assert(unlink(TEST_FILE) == 0);
    puts("All B+ tree tests passed.");
    return 0;
}
