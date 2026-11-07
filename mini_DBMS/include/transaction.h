#ifndef MINI_DBMS_TRANSACTION_H
#define MINI_DBMS_TRANSACTION_H

#include <stdint.h>

#include "bplus_tree.h"
#include "wal.h"

typedef struct TransactionManager TransactionManager;

TransactionManager *txn_manager_create(Wal *wal, BufferPool *pool,
                                       BPlusTree *tree);
uint64_t txn_begin(TransactionManager *tm);
int txn_insert(TransactionManager *tm, uint64_t txn_id,
               uint64_t key, uint64_t value);
int txn_commit(TransactionManager *tm, uint64_t txn_id);
int txn_abort(TransactionManager *tm, uint64_t txn_id);
int txn_recover(TransactionManager *tm);
void txn_manager_destroy(TransactionManager *tm);

#endif
