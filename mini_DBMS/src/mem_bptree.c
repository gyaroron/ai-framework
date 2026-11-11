#include "mem_bptree.h"

#include <stdlib.h>
#include <string.h>

/*
 * node 하나.
 * - leaf node: keys[i]의 값이 values[i]다. next는 오른쪽 leaf를 가리킨다.
 * - internal node: children[i]는 keys[i]보다 작은 key들이 있는 쪽,
 *   children[i + 1]은 keys[i] 이상인 key들이 있는 쪽이다.
 * 배열을 한 칸 더 크게 잡아, 넘친 상태를 잠깐 담았다가 나눈다.
 */
typedef struct MemNode {
    int is_leaf;
    int key_count;
    uint64_t keys[MEM_BPTREE_MAX_KEYS + 1];
    union {
        uint64_t values[MEM_BPTREE_MAX_KEYS + 1];
        struct MemNode *children[MEM_BPTREE_MAX_KEYS + 2];
    } u;
    struct MemNode *next;
} MemNode;

struct MemBPTree {
    MemNode *root;
    size_t count;
    unsigned int height;
};

/* 자식에서 split이 일어났을 때 부모에게 올려 보내는 정보. */
typedef struct SplitResult {
    int did_split;
    uint64_t promoted_key;
    MemNode *right;
} SplitResult;

static MemNode *node_create(int is_leaf)
{
    MemNode *node = (MemNode *)calloc(1, sizeof(MemNode));

    if (node != NULL) {
        node->is_leaf = is_leaf;
    }
    return node;
}

/* node 안에서 key가 들어갈 자리(key 이상인 첫 칸)를 이진 탐색으로 찾는다. */
static int lower_bound(const MemNode *node, uint64_t key)
{
    int low = 0;
    int high = node->key_count;

    while (low < high) {
        int middle = low + (high - low) / 2;
        if (node->keys[middle] < key) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

/* internal node에서 key가 속한 자식의 번호를 고른다. keys[i] == key면 오른쪽 자식이다. */
static int child_index(const MemNode *node, uint64_t key)
{
    int index = lower_bound(node, key);

    if (index < node->key_count && node->keys[index] == key) {
        ++index;
    }
    return index;
}

MemBPTree *mem_bptree_create(void)
{
    MemBPTree *tree = (MemBPTree *)calloc(1, sizeof(MemBPTree));

    if (tree == NULL) {
        return NULL;
    }
    tree->root = node_create(1);
    if (tree->root == NULL) {
        free(tree);
        return NULL;
    }
    tree->height = 1;
    return tree;
}

static void node_destroy(MemNode *node)
{
    int i;

    if (node == NULL) {
        return;
    }
    if (!node->is_leaf) {
        for (i = 0; i <= node->key_count; ++i) {
            node_destroy(node->u.children[i]);
        }
    }
    free(node);
}

void mem_bptree_destroy(MemBPTree *tree)
{
    if (tree == NULL) {
        return;
    }
    node_destroy(tree->root);
    free(tree);
}

/*
 * 넘친 leaf를 반으로 나눈다. 오른쪽 절반을 새 leaf로 옮기고,
 * 새 leaf의 첫 key를 부모에게 올린다(leaf에도 그 key가 그대로 남는다).
 */
static int split_leaf(MemNode *leaf, SplitResult *result)
{
    MemNode *right = node_create(1);
    int left_count = leaf->key_count / 2;
    int right_count = leaf->key_count - left_count;

    if (right == NULL) {
        return -1;
    }
    memcpy(right->keys, leaf->keys + left_count, (size_t)right_count * sizeof(uint64_t));
    memcpy(right->u.values, leaf->u.values + left_count,
           (size_t)right_count * sizeof(uint64_t));
    right->key_count = right_count;
    right->next = leaf->next;
    leaf->key_count = left_count;
    leaf->next = right;

    result->did_split = 1;
    result->promoted_key = right->keys[0];
    result->right = right;
    return 0;
}

/*
 * 넘친 internal node를 반으로 나눈다. 가운데 key는 부모로 올라가고
 * 두 node 어디에도 남지 않는다(leaf split과 다른 점).
 */
static int split_internal(MemNode *node, SplitResult *result)
{
    MemNode *right = node_create(0);
    int middle = node->key_count / 2;
    int right_count = node->key_count - middle - 1;

    if (right == NULL) {
        return -1;
    }
    memcpy(right->keys, node->keys + middle + 1, (size_t)right_count * sizeof(uint64_t));
    memcpy(right->u.children, node->u.children + middle + 1,
           (size_t)(right_count + 1) * sizeof(MemNode *));
    right->key_count = right_count;

    result->did_split = 1;
    result->promoted_key = node->keys[middle];
    result->right = right;
    node->key_count = middle;
    return 0;
}

/*
 * node 아래에 key를 넣는다. 재귀로 leaf까지 내려가 넣고,
 * 올라오면서 넘친 node를 나눈다. *is_new에는 새 key였는지를 적는다.
 */
static int insert_into(MemNode *node, uint64_t key, uint64_t value,
                       SplitResult *result, int *is_new)
{
    int index;

    result->did_split = 0;
    if (node->is_leaf) {
        index = lower_bound(node, key);
        if (index < node->key_count && node->keys[index] == key) {
            node->u.values[index] = value;
            *is_new = 0;
            return 0;
        }
        memmove(node->keys + index + 1, node->keys + index,
                (size_t)(node->key_count - index) * sizeof(uint64_t));
        memmove(node->u.values + index + 1, node->u.values + index,
                (size_t)(node->key_count - index) * sizeof(uint64_t));
        node->keys[index] = key;
        node->u.values[index] = value;
        ++node->key_count;
        *is_new = 1;
        if (node->key_count > MEM_BPTREE_MAX_KEYS) {
            return split_leaf(node, result);
        }
        return 0;
    }

    index = child_index(node, key);
    {
        SplitResult child_result;

        if (insert_into(node->u.children[index], key, value, &child_result, is_new) != 0) {
            return -1;
        }
        if (!child_result.did_split) {
            return 0;
        }
        /* 자식이 둘로 나뉘었다. 올라온 key와 새 오른쪽 자식을 이 node에 끼워 넣는다. */
        memmove(node->keys + index + 1, node->keys + index,
                (size_t)(node->key_count - index) * sizeof(uint64_t));
        memmove(node->u.children + index + 2, node->u.children + index + 1,
                (size_t)(node->key_count - index) * sizeof(MemNode *));
        node->keys[index] = child_result.promoted_key;
        node->u.children[index + 1] = child_result.right;
        ++node->key_count;
        if (node->key_count > MEM_BPTREE_MAX_KEYS) {
            return split_internal(node, result);
        }
    }
    return 0;
}

int mem_bptree_insert(MemBPTree *tree, uint64_t key, uint64_t value)
{
    SplitResult result;
    int is_new = 0;

    if (tree == NULL) {
        return -1;
    }
    if (insert_into(tree->root, key, value, &result, &is_new) != 0) {
        return -1;
    }
    if (result.did_split) {
        /* root가 나뉘면 새 root를 만들어 tree가 한 층 높아진다. */
        MemNode *new_root = node_create(0);

        if (new_root == NULL) {
            return -1;
        }
        new_root->keys[0] = result.promoted_key;
        new_root->u.children[0] = tree->root;
        new_root->u.children[1] = result.right;
        new_root->key_count = 1;
        tree->root = new_root;
        ++tree->height;
    }
    if (is_new) {
        ++tree->count;
    }
    return 0;
}

int mem_bptree_find(const MemBPTree *tree, uint64_t key, uint64_t *out_value)
{
    const MemNode *node;
    int index;

    if (tree == NULL) {
        return 0;
    }
    node = tree->root;
    while (!node->is_leaf) {
        node = node->u.children[child_index(node, key)];
    }
    index = lower_bound(node, key);
    if (index < node->key_count && node->keys[index] == key) {
        if (out_value != NULL) {
            *out_value = node->u.values[index];
        }
        return 1;
    }
    return 0;
}

size_t mem_bptree_count(const MemBPTree *tree)
{
    return tree == NULL ? 0 : tree->count;
}

unsigned int mem_bptree_height(const MemBPTree *tree)
{
    return tree == NULL ? 0 : tree->height;
}

int mem_bptree_check(const MemBPTree *tree)
{
    const MemNode *node;
    size_t seen = 0;
    uint64_t previous = 0;
    int has_previous = 0;
    int i;

    if (tree == NULL) {
        return 0;
    }
    node = tree->root;
    while (!node->is_leaf) {
        node = node->u.children[0];
    }
    for (; node != NULL; node = node->next) {
        for (i = 0; i < node->key_count; ++i) {
            if (has_previous && node->keys[i] <= previous) {
                return 0;
            }
            previous = node->keys[i];
            has_previous = 1;
            ++seen;
        }
    }
    return seen == tree->count;
}
