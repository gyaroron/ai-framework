#define _POSIX_C_SOURCE 200809L

#include "transaction.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#define DATA_FILE "test_transaction.db"
#define WAL_FILE "test_transaction.wal"

typedef struct LsnCheck {
    uint64_t previous;
    uint64_t count;
} LsnCheck;

static int check_lsn_order(const WalRecordHeader *header,
                           const void *payload, void *context)
{
    LsnCheck *check = (LsnCheck *)context;
    (void)payload;
    assert(header->lsn == check->previous + 1u);
    check->previous = header->lsn;
    ++check->count;
    return 0;
}

int main(void)
{
    Pager *pager;
    BufferPool *pool;
    BPlusTree *tree;
    Wal *wal;
    TransactionManager *tm;
    uint64_t txn_id;
    uint64_t value;
    LsnCheck lsn_check = {0, 0};

    (void)unlink(DATA_FILE);
    (void)unlink(WAL_FILE);
    pager = pager_open(DATA_FILE);
    assert(pager != NULL);
    pool = buffer_pool_create(pager, 8);
    assert(pool != NULL);
    tree = bplus_tree_create(pool, 0);
    assert(tree != NULL);
    wal = wal_open(WAL_FILE);
    assert(wal != NULL);
    tm = txn_manager_create(wal, pool, tree);
    assert(tm != NULL);

    /* PASS 1: commit is durable in WAL before it returns. */
    txn_id = txn_begin(tm);
    assert(txn_id != 0);
    assert(txn_insert(tm, txn_id, 11, 1100) == 0);
    assert(txn_commit(tm, txn_id) == 0);
    assert(bplus_tree_find(tree, 11, &value) == 0 && value == 1100);
    assert(wal_visit(wal, check_lsn_order, &lsn_check) == 0);
    assert(lsn_check.count == 3);
    puts("PASS 1: committed insert visible and WAL records synchronized");

    /* PASS 2: abort restores the pre-transaction tree state. */
    txn_id = txn_begin(tm);
    assert(txn_id != 0);
    assert(txn_insert(tm, txn_id, 11, 1111) == 0);
    assert(txn_insert(tm, txn_id, 22, 2200) == 0);
    assert(bplus_tree_find(tree, 11, &value) == 0 && value == 1111);
    assert(bplus_tree_find(tree, 22, &value) == 0 && value == 2200);
    assert(txn_abort(tm, txn_id) == 0);
    assert(bplus_tree_find(tree, 11, &value) == 0 && value == 1100);
    errno = 0;
    assert(bplus_tree_find(tree, 22, &value) == -1 && errno == ENOENT);
    puts("PASS 2: aborted insert was undone");

    /* PASS 3: discard the manager during an active transaction, then recover.
     * Remove the committed key to model a data page missing its committed change. */
    txn_id = txn_begin(tm);
    assert(txn_id != 0);
    assert(txn_insert(tm, txn_id, 33, 3300) == 0);
    txn_manager_destroy(tm);
    tm = txn_manager_create(wal, pool, tree);
    assert(tm != NULL);
    assert(bplus_tree_delete(tree, 11) == 0);
    assert(txn_recover(tm) == 0);
    assert(bplus_tree_find(tree, 11, &value) == 0 && value == 1100);
    errno = 0;
    assert(bplus_tree_find(tree, 22, &value) == -1 && errno == ENOENT);
    errno = 0;
    assert(bplus_tree_find(tree, 33, &value) == -1 && errno == ENOENT);
    puts("PASS 3: recovery redid committed data and undid unfinished work");

    /* PASS 4: reopening validates record checksums and continues LSN order. */
    txn_manager_destroy(tm);
    wal_close(wal);
    wal = wal_open(WAL_FILE);
    assert(wal != NULL);
    lsn_check.previous = 0;
    lsn_check.count = 0;
    assert(wal_visit(wal, check_lsn_order, &lsn_check) == 0);
    assert(lsn_check.count == 10);
    tm = txn_manager_create(wal, pool, tree);
    assert(tm != NULL);
    assert(txn_recover(tm) == 0);
    txn_id = txn_begin(tm);
    assert(txn_id != 0);
    assert(txn_commit(tm, txn_id) == 0);
    txn_manager_destroy(tm);
    lsn_check.previous = 0;
    lsn_check.count = 0;
    assert(wal_visit(wal, check_lsn_order, &lsn_check) == 0);
    assert(lsn_check.count == 12);
    puts("PASS 4: WAL reopened with sequential, checksum-validated LSNs");

    wal_close(wal);
    bplus_tree_destroy(tree);
    buffer_pool_destroy(pool);
    assert(pager_close(pager) == 0);
    assert(unlink(DATA_FILE) == 0);
    assert(unlink(WAL_FILE) == 0);
    puts("All transaction tests passed.");
    return 0;
}
