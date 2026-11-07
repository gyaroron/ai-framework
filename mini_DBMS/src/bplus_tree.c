#define _POSIX_C_SOURCE 200809L

#include "bplus_tree.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef union BPlusNode {
    BPlusNodeHeader header;
    BPlusLeafNode leaf;
    BPlusInternalNode internal;
    unsigned char bytes[PAGE_SIZE];
} BPlusNode;

typedef char check_leaf_page_size[
    sizeof(BPlusLeafNode) == PAGE_SIZE ? 1 : -1];
typedef char check_internal_page_size[
    sizeof(BPlusInternalNode) == PAGE_SIZE ? 1 : -1];

struct BPlusTree {
    BufferPool *pool;
    uint32_t root_page_no;
};

static int read_node(BufferPool *pool, uint32_t page_no, BPlusNode *out)
{
    Page *page = buffer_pool_get_page(pool, page_no);
    int unpin_result;

    if (page == NULL) {
        return -1;
    }
    memcpy(out->bytes, page->bytes, PAGE_SIZE);
    unpin_result = buffer_pool_unpin_page(pool, page_no, 0);
    return unpin_result;
}

static int write_node(BufferPool *pool, uint32_t page_no, const BPlusNode *node)
{
    Page *page = buffer_pool_get_page(pool, page_no);
    int unpin_result;

    if (page == NULL) {
        return -1;
    }
    memcpy(page->bytes, node->bytes, PAGE_SIZE);
    unpin_result = buffer_pool_unpin_page(pool, page_no, 1);
    return unpin_result;
}

static int write_new_node(BufferPool *pool, uint32_t *page_no,
                          const BPlusNode *node)
{
    Page *page = buffer_pool_new_page(pool, page_no);

    if (page == NULL) {
        return -1;
    }
    memcpy(page->bytes, node->bytes, PAGE_SIZE);
    return buffer_pool_unpin_page(pool, *page_no, 1);
}

static int initialize_leaf(BufferPool *pool, uint32_t page_no)
{
    BPlusNode node;
    memset(&node, 0, sizeof(node));
    node.leaf.header.node_type = NODE_TYPE_LEAF;
    node.leaf.header.parent_page_no = BPLUS_NO_PAGE;
    node.leaf.next_leaf_page_no = BPLUS_NO_PAGE;
    return write_node(pool, page_no, &node);
}

static int find_leaf_page(BPlusTree *tree, uint64_t key, uint32_t *leaf_page_no)
{
    uint32_t page_no = tree->root_page_no;

    for (;;) {
        BPlusNode node;
        if (read_node(tree->pool, page_no, &node) != 0) {
            return -1;
        }
        if (node.header.node_type == NODE_TYPE_LEAF) {
            *leaf_page_no = page_no;
            return 0;
        }
        if (node.header.node_type != NODE_TYPE_INTERNAL ||
            node.header.num_keys > BPLUS_INTERNAL_MAX_KEYS) {
            errno = EINVAL;
            return -1;
        }

        {
            uint16_t child_index = 0;
            while (child_index < node.internal.header.num_keys &&
                   key >= node.internal.keys[child_index]) {
                ++child_index;
            }
            page_no = node.internal.children[child_index];
            if (page_no == BPLUS_NO_PAGE) {
                errno = EINVAL;
                return -1;
            }
        }
    }
}

int bplus_tree_find_leaf_page(BPlusTree *tree, uint64_t key,
                              uint32_t *out_page_no)
{
    if (tree == NULL || out_page_no == NULL) {
        errno = EINVAL;
        return -1;
    }
    return find_leaf_page(tree, key, out_page_no);
}

BPlusTree *bplus_tree_create(BufferPool *pool, uint32_t root_page_no)
{
    BPlusTree *tree;
    Page *page;
    BPlusNode node;

    if (pool == NULL) {
        errno = EINVAL;
        return NULL;
    }

    page = buffer_pool_get_page(pool, root_page_no);
    if (page == NULL) {
        uint32_t allocated_page_no;
        page = buffer_pool_new_page(pool, &allocated_page_no);
        if (page == NULL) {
            return NULL;
        }
        if (allocated_page_no != root_page_no) {
            (void)buffer_pool_unpin_page(pool, allocated_page_no, 1);
            errno = EINVAL;
            return NULL;
        }
        memset(page->bytes, 0, PAGE_SIZE);
        memset(&node, 0, sizeof(node));
        node.leaf.header.node_type = NODE_TYPE_LEAF;
        node.leaf.header.parent_page_no = BPLUS_NO_PAGE;
        node.leaf.next_leaf_page_no = BPLUS_NO_PAGE;
        memcpy(page->bytes, node.bytes, PAGE_SIZE);
        if (buffer_pool_unpin_page(pool, root_page_no, 1) != 0) {
            return NULL;
        }
    } else {
        memcpy(node.bytes, page->bytes, PAGE_SIZE);
        if (buffer_pool_unpin_page(pool, root_page_no, 0) != 0) {
            return NULL;
        }
        if (node.header.node_type == 0) {
            if (initialize_leaf(pool, root_page_no) != 0) {
                return NULL;
            }
        } else if (node.header.node_type != NODE_TYPE_LEAF &&
                   node.header.node_type != NODE_TYPE_INTERNAL) {
            errno = EINVAL;
            return NULL;
        }
    }

    tree = (BPlusTree *)malloc(sizeof(*tree));
    if (tree == NULL) {
        return NULL;
    }
    tree->pool = pool;
    tree->root_page_no = root_page_no;
    return tree;
}

int bplus_tree_find(BPlusTree *tree, uint64_t key, uint64_t *out_value)
{
    uint32_t leaf_page_no;
    BPlusNode leaf;
    uint16_t low;
    uint16_t high;

    if (tree == NULL || out_value == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (find_leaf_page(tree, key, &leaf_page_no) != 0 ||
        read_node(tree->pool, leaf_page_no, &leaf) != 0) {
        return -1;
    }
    if (leaf.leaf.header.node_type != NODE_TYPE_LEAF ||
        leaf.leaf.header.num_keys > BPLUS_LEAF_MAX_KEYS) {
        errno = EINVAL;
        return -1;
    }

    low = 0;
    high = leaf.leaf.header.num_keys;
    while (low < high) {
        uint16_t middle = (uint16_t)(low + (high - low) / 2);
        if (leaf.leaf.entries[middle].key < key) {
            low = (uint16_t)(middle + 1);
        } else {
            high = middle;
        }
    }
    if (low == leaf.leaf.header.num_keys || leaf.leaf.entries[low].key != key) {
        errno = ENOENT;
        return -1;
    }
    *out_value = leaf.leaf.entries[low].value;
    return 0;
}

static int update_child_parent(BufferPool *pool, uint32_t child_page_no,
                               uint32_t parent_page_no)
{
    BPlusNode child;

    if (read_node(pool, child_page_no, &child) != 0) {
        return -1;
    }
    child.header.parent_page_no = parent_page_no;
    return write_node(pool, child_page_no, &child);
}

static int install_root(BPlusTree *tree, uint32_t left_page_no,
                        uint32_t right_page_no, uint64_t separator)
{
    BPlusNode root;

    memset(&root, 0, sizeof(root));
    root.internal.header.node_type = NODE_TYPE_INTERNAL;
    root.internal.header.parent_page_no = BPLUS_NO_PAGE;
    root.internal.header.num_keys = 1;
    root.internal.keys[0] = separator;
    root.internal.children[0] = left_page_no;
    root.internal.children[1] = right_page_no;
    if (write_node(tree->pool, tree->root_page_no, &root) != 0 ||
        update_child_parent(tree->pool, left_page_no, tree->root_page_no) != 0 ||
        update_child_parent(tree->pool, right_page_no, tree->root_page_no) != 0) {
        return -1;
    }
    return 0;
}

static int insert_into_parent(BPlusTree *tree, uint32_t left_page_no,
                              uint32_t right_page_no, uint64_t separator,
                              uint32_t parent_page_no);

static int split_internal_and_promote(BPlusTree *tree, uint32_t page_no,
                                       const BPlusNodeHeader *header,
                                       const uint64_t *source_keys,
                                       const uint32_t *source_children,
                                       uint32_t old_parent_page_no)
{
    uint64_t temp_keys[BPLUS_INTERNAL_MAX_KEYS + 1u];
    uint32_t temp_children[BPLUS_INTERNAL_MAX_KEYS + 2u];
    BPlusNode left_node;
    BPlusNode right_node;
    uint16_t middle = (uint16_t)((BPLUS_INTERNAL_MAX_KEYS + 1u) / 2u);
    uint16_t i;
    uint32_t right_page_no;
    uint64_t promoted_key;

    memcpy(temp_keys, source_keys, sizeof(temp_keys));
    memcpy(temp_children, source_children, sizeof(temp_children));
    promoted_key = temp_keys[middle];

    memset(&left_node, 0, sizeof(left_node));
    memset(&right_node, 0, sizeof(right_node));
    left_node.internal.header = *header;
    left_node.internal.header.num_keys = middle;
    right_node.internal.header.node_type = NODE_TYPE_INTERNAL;
    right_node.internal.header.parent_page_no = old_parent_page_no;
    right_node.internal.header.num_keys =
        (uint16_t)(BPLUS_INTERNAL_MAX_KEYS + 1u - middle - 1u);

    memcpy(left_node.internal.keys, temp_keys,
           (size_t)middle * sizeof(uint64_t));
    memcpy(left_node.internal.children, temp_children,
           (size_t)(middle + 1u) * sizeof(uint32_t));
    memcpy(right_node.internal.keys, temp_keys + middle + 1u,
           (size_t)right_node.internal.header.num_keys * sizeof(uint64_t));
    memcpy(right_node.internal.children, temp_children + middle + 1u,
           (size_t)(right_node.internal.header.num_keys + 1u) * sizeof(uint32_t));

    if (old_parent_page_no == BPLUS_NO_PAGE) {
        uint32_t left_page_no;
        left_node.internal.header.parent_page_no = tree->root_page_no;
        right_node.internal.header.parent_page_no = tree->root_page_no;
        if (write_new_node(tree->pool, &left_page_no, &left_node) != 0 ||
            write_new_node(tree->pool, &right_page_no, &right_node) != 0) {
            return -1;
        }
        for (i = 0; i <= left_node.internal.header.num_keys; ++i) {
            if (update_child_parent(tree->pool, left_node.internal.children[i],
                                    left_page_no) != 0) {
                return -1;
            }
        }
        for (i = 0; i <= right_node.internal.header.num_keys; ++i) {
            if (update_child_parent(tree->pool, right_node.internal.children[i],
                                    right_page_no) != 0) {
                return -1;
            }
        }
        return install_root(tree, left_page_no, right_page_no, promoted_key);
    }

    if (write_new_node(tree->pool, &right_page_no, &right_node) != 0) {
        return -1;
    }
    if (write_node(tree->pool, page_no, &left_node) != 0) {
        return -1;
    }
    for (i = 0; i <= right_node.internal.header.num_keys; ++i) {
        if (update_child_parent(tree->pool, right_node.internal.children[i],
                                right_page_no) != 0) {
            return -1;
        }
    }
    return insert_into_parent(tree, page_no, right_page_no, promoted_key,
                              old_parent_page_no);
}

static int create_new_root(BPlusTree *tree, uint32_t left_page_no,
                           uint32_t right_page_no, uint64_t separator)
{
    BPlusNode root;
    uint32_t new_root_page_no;

    memset(&root, 0, sizeof(root));
    root.internal.header.node_type = NODE_TYPE_INTERNAL;
    root.internal.header.parent_page_no = BPLUS_NO_PAGE;
    root.internal.header.num_keys = 1;
    root.internal.keys[0] = separator;
    root.internal.children[0] = left_page_no;
    root.internal.children[1] = right_page_no;

    if (write_new_node(tree->pool, &new_root_page_no, &root) != 0 ||
        update_child_parent(tree->pool, left_page_no, new_root_page_no) != 0 ||
        update_child_parent(tree->pool, right_page_no, new_root_page_no) != 0) {
        return -1;
    }
    tree->root_page_no = new_root_page_no;
    return 0;
}

static int insert_into_parent(BPlusTree *tree, uint32_t left_page_no,
                              uint32_t right_page_no, uint64_t separator,
                              uint32_t parent_page_no)
{
    BPlusNode parent;
    uint16_t child_index;
    uint16_t i;

    if (parent_page_no == BPLUS_NO_PAGE) {
        return create_new_root(tree, left_page_no, right_page_no, separator);
    }
    if (read_node(tree->pool, parent_page_no, &parent) != 0 ||
        parent.header.node_type != NODE_TYPE_INTERNAL ||
        parent.internal.header.num_keys > BPLUS_INTERNAL_MAX_KEYS) {
        errno = EINVAL;
        return -1;
    }

    child_index = 0;
    while (child_index <= parent.internal.header.num_keys &&
           parent.internal.children[child_index] != left_page_no) {
        ++child_index;
    }
    if (child_index > parent.internal.header.num_keys) {
        errno = EINVAL;
        return -1;
    }

    if (parent.internal.header.num_keys < BPLUS_INTERNAL_MAX_KEYS) {
        for (i = parent.internal.header.num_keys; i > child_index; --i) {
            parent.internal.keys[i] = parent.internal.keys[i - 1u];
        }
        for (i = (uint16_t)(parent.internal.header.num_keys + 1u);
             i > (uint16_t)(child_index + 1u); --i) {
            parent.internal.children[i] = parent.internal.children[i - 1u];
        }
        parent.internal.keys[child_index] = separator;
        parent.internal.children[child_index + 1u] = right_page_no;
        ++parent.internal.header.num_keys;
        if (write_node(tree->pool, parent_page_no, &parent) != 0) {
            return -1;
        }
        return update_child_parent(tree->pool, right_page_no, parent_page_no);
    }

    {
        uint64_t temp_keys[BPLUS_INTERNAL_MAX_KEYS + 1u];
        uint32_t temp_children[BPLUS_INTERNAL_MAX_KEYS + 2u];
        BPlusNodeHeader overflow_header = parent.internal.header;

        for (i = 0; i < child_index; ++i) {
            temp_keys[i] = parent.internal.keys[i];
        }
        temp_keys[child_index] = separator;
        for (i = child_index; i < parent.internal.header.num_keys; ++i) {
            temp_keys[i + 1u] = parent.internal.keys[i];
        }
        for (i = 0; i <= child_index; ++i) {
            temp_children[i] = parent.internal.children[i];
        }
        temp_children[child_index + 1u] = right_page_no;
        for (i = (uint16_t)(child_index + 1u);
             i <= parent.internal.header.num_keys; ++i) {
            temp_children[i + 1u] = parent.internal.children[i];
        }
        overflow_header.num_keys = (uint16_t)(BPLUS_INTERNAL_MAX_KEYS + 1u);
        return split_internal_and_promote(tree, parent_page_no, &overflow_header,
                                          temp_keys, temp_children,
                                          parent.internal.header.parent_page_no);
    }
}

int bplus_tree_insert(BPlusTree *tree, uint64_t key, uint64_t value)
{
    uint32_t leaf_page_no;
    BPlusNode leaf;
    uint16_t position;

    if (tree == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (find_leaf_page(tree, key, &leaf_page_no) != 0 ||
        read_node(tree->pool, leaf_page_no, &leaf) != 0) {
        return -1;
    }
    if (leaf.leaf.header.node_type != NODE_TYPE_LEAF ||
        leaf.leaf.header.num_keys > BPLUS_LEAF_MAX_KEYS) {
        errno = EINVAL;
        return -1;
    }

    position = 0;
    while (position < leaf.leaf.header.num_keys &&
           leaf.leaf.entries[position].key < key) {
        ++position;
    }
    if (position < leaf.leaf.header.num_keys &&
        leaf.leaf.entries[position].key == key) {
        leaf.leaf.entries[position].value = value;
        return write_node(tree->pool, leaf_page_no, &leaf);
    }

    if (leaf.leaf.header.num_keys < BPLUS_LEAF_MAX_KEYS) {
        uint16_t i = leaf.leaf.header.num_keys;
        while (i > position) {
            leaf.leaf.entries[i] = leaf.leaf.entries[i - 1u];
            --i;
        }
        leaf.leaf.entries[position].key = key;
        leaf.leaf.entries[position].value = value;
        ++leaf.leaf.header.num_keys;
        return write_node(tree->pool, leaf_page_no, &leaf);
    }

    {
        BPlusLeafEntry entries[BPLUS_LEAF_MAX_KEYS + 1u];
        BPlusNode right_node;
        uint16_t i;
        uint16_t source = 0;
        uint16_t middle = (uint16_t)((BPLUS_LEAF_MAX_KEYS + 1u) / 2u);
        uint32_t right_page_no;
        uint32_t old_parent_page_no = leaf.leaf.header.parent_page_no;
        uint32_t old_next_leaf_page_no = leaf.leaf.next_leaf_page_no;

        for (i = 0; i < position; ++i) {
            entries[i] = leaf.leaf.entries[i];
        }
        entries[position].key = key;
        entries[position].value = value;
        for (i = position; i < BPLUS_LEAF_MAX_KEYS; ++i) {
            entries[i + 1u] = leaf.leaf.entries[source++];
        }

        memset(&right_node, 0, sizeof(right_node));
        right_node.leaf.header.node_type = NODE_TYPE_LEAF;
        right_node.leaf.header.parent_page_no = old_parent_page_no;
        right_node.leaf.header.num_keys =
            (uint16_t)(BPLUS_LEAF_MAX_KEYS + 1u - middle);
        right_node.leaf.next_leaf_page_no = old_next_leaf_page_no;
        memcpy(right_node.leaf.entries, entries + middle,
               (size_t)right_node.leaf.header.num_keys * sizeof(BPlusLeafEntry));

        if (old_parent_page_no == BPLUS_NO_PAGE) {
            BPlusNode left_node;
            uint32_t left_page_no;
            memset(&left_node, 0, sizeof(left_node));
            if (write_new_node(tree->pool, &right_page_no, &right_node) != 0) {
                return -1;
            }
            memset(leaf.leaf.entries + middle, 0,
                   (BPLUS_LEAF_MAX_KEYS - middle) * sizeof(BPlusLeafEntry));
            memcpy(leaf.leaf.entries, entries,
                   (size_t)middle * sizeof(BPlusLeafEntry));
            leaf.leaf.header.num_keys = middle;
            leaf.leaf.next_leaf_page_no = right_page_no;
            leaf.leaf.header.parent_page_no = tree->root_page_no;
            left_node.leaf = leaf.leaf;
            if (write_new_node(tree->pool, &left_page_no, &left_node) != 0) {
                return -1;
            }
            return install_root(tree, left_page_no, right_page_no,
                                right_node.leaf.entries[0].key);
        }

        if (write_new_node(tree->pool, &right_page_no, &right_node) != 0) {
            return -1;
        }
        memset(leaf.leaf.entries + middle, 0,
               (BPLUS_LEAF_MAX_KEYS - middle) * sizeof(BPlusLeafEntry));
        memcpy(leaf.leaf.entries, entries,
               (size_t)middle * sizeof(BPlusLeafEntry));
        leaf.leaf.header.num_keys = middle;
        leaf.leaf.next_leaf_page_no = right_page_no;
        if (write_node(tree->pool, leaf_page_no, &leaf) != 0) {
            return -1;
        }
        return insert_into_parent(tree, leaf_page_no, right_page_no,
                                  right_node.leaf.entries[0].key,
                                  old_parent_page_no);
    }
}

int bplus_tree_delete(BPlusTree *tree, uint64_t key)
{
    uint32_t leaf_page_no;
    BPlusNode leaf;
    uint16_t position;

    if (tree == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (find_leaf_page(tree, key, &leaf_page_no) != 0 ||
        read_node(tree->pool, leaf_page_no, &leaf) != 0) {
        return -1;
    }
    if (leaf.leaf.header.node_type != NODE_TYPE_LEAF ||
        leaf.leaf.header.num_keys > BPLUS_LEAF_MAX_KEYS) {
        errno = EINVAL;
        return -1;
    }

    position = 0;
    while (position < leaf.leaf.header.num_keys &&
           leaf.leaf.entries[position].key < key) {
        ++position;
    }
    if (position == leaf.leaf.header.num_keys ||
        leaf.leaf.entries[position].key != key) {
        errno = ENOENT;
        return -1;
    }
    while (position + 1u < leaf.leaf.header.num_keys) {
        leaf.leaf.entries[position] = leaf.leaf.entries[position + 1u];
        ++position;
    }
    --leaf.leaf.header.num_keys;
    memset(&leaf.leaf.entries[leaf.leaf.header.num_keys], 0,
           sizeof(BPlusLeafEntry));
    return write_node(tree->pool, leaf_page_no, &leaf);
}

void bplus_tree_destroy(BPlusTree *tree)
{
    free(tree);
}
