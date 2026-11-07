#define _POSIX_C_SOURCE 200809L

#include "server_bridge.h"

#include <stdio.h>

int main(int argc, char **argv)
{
    const char *data_path = argc > 1 ? argv[1] : "mini_dbms.db";
    const char *wal_path = argc > 2 ? argv[2] : "mini_dbms.wal";
    Pager *pager = NULL;
    BufferPool *pool = NULL;
    BPlusTree *tree = NULL;
    Wal *wal = NULL;
    TransactionManager *transactions = NULL;
    int result = 1;

    if (argc > 3) {
        (void)fprintf(stderr, "usage: %s [data-file [wal-file]]\n", argv[0]);
        return 2;
    }
    pager = pager_open(data_path);
    if (pager == NULL) {
        perror("pager_open");
        goto cleanup;
    }
    pool = buffer_pool_create(pager, 0);
    if (pool == NULL) {
        perror("buffer_pool_create");
        goto cleanup;
    }
    tree = bplus_tree_create(pool, 0);
    if (tree == NULL) {
        perror("bplus_tree_create");
        goto cleanup;
    }
    wal = wal_open(wal_path);
    if (wal == NULL) {
        perror("wal_open");
        goto cleanup;
    }
    transactions = txn_manager_create(wal, pool, tree);
    if (transactions == NULL) {
        perror("txn_manager_create");
        goto cleanup;
    }
    if (txn_recover(transactions) != 0) {
        perror("txn_recover");
        goto cleanup;
    }
    result = server_bridge_run(transactions, tree) == 0 ? 0 : 1;
    if (result != 0) {
        perror("server_bridge_run");
    }

cleanup:
    txn_manager_destroy(transactions);
    bplus_tree_destroy(tree);
    if (pool != NULL && buffer_pool_flush_all(pool) != 0) {
        perror("buffer_pool_flush_all");
        result = 1;
    }
    buffer_pool_destroy(pool);
    wal_close(wal);
    if (pager != NULL && pager_close(pager) != 0) {
        perror("pager_close");
        result = 1;
    }
    return result;
}
