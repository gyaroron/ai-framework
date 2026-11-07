#define _POSIX_C_SOURCE 200809L

#include "transaction.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct TxnOperation {
    WalInsertPayload insert;
    struct TxnOperation *next;
} TxnOperation;

typedef struct ActiveTransaction {
    uint64_t txn_id;
    TxnOperation *operations;
    TxnOperation *last_operation;
    int commit_appended;
    int abort_appended;
    struct ActiveTransaction *next;
} ActiveTransaction;

struct TransactionManager {
    Wal *wal;
    BufferPool *pool;
    BPlusTree *tree;
    uint64_t next_txn_id;
    ActiveTransaction *active;
};

typedef struct RecoveryTransaction {
    uint64_t txn_id;
    uint16_t terminal_type;
    TxnOperation *operations;
    TxnOperation *last_operation;
    struct RecoveryTransaction *next;
} RecoveryTransaction;

typedef struct RecoveryContext {
    RecoveryTransaction *transactions;
    RecoveryTransaction *last_transaction;
    uint64_t maximum_txn_id;
} RecoveryContext;

static void free_operations(TxnOperation *operation)
{
    while (operation != NULL) {
        TxnOperation *next = operation->next;
        free(operation);
        operation = next;
    }
}

static void free_active(ActiveTransaction *transaction)
{
    if (transaction != NULL) {
        free_operations(transaction->operations);
        free(transaction);
    }
}

static ActiveTransaction *find_active(TransactionManager *tm, uint64_t txn_id)
{
    ActiveTransaction *transaction;
    for (transaction = tm->active; transaction != NULL;
         transaction = transaction->next) {
        if (transaction->txn_id == txn_id) {
            return transaction;
        }
    }
    return NULL;
}

TransactionManager *txn_manager_create(Wal *wal, BufferPool *pool,
                                       BPlusTree *tree)
{
    TransactionManager *tm;

    if (wal == NULL || pool == NULL || tree == NULL) {
        errno = EINVAL;
        return NULL;
    }
    tm = (TransactionManager *)calloc(1, sizeof(*tm));
    if (tm == NULL) {
        return NULL;
    }
    tm->wal = wal;
    tm->pool = pool;
    tm->tree = tree;
    tm->next_txn_id = 1;
    return tm;
}

uint64_t txn_begin(TransactionManager *tm)
{
    ActiveTransaction *transaction;
    uint64_t txn_id;

    if (tm == NULL || tm->active != NULL || tm->next_txn_id == 0 ||
        tm->next_txn_id == UINT64_MAX) {
        errno = tm != NULL && tm->active != NULL ? EBUSY : EINVAL;
        return 0;
    }
    transaction = (ActiveTransaction *)calloc(1, sizeof(*transaction));
    if (transaction == NULL) {
        return 0;
    }
    txn_id = tm->next_txn_id++;
    if (wal_append(tm->wal, txn_id, WAL_RECORD_BEGIN, NULL, 0) == 0 ||
        wal_flush(tm->wal) != 0) {
        free(transaction);
        return 0;
    }
    transaction->txn_id = txn_id;
    tm->active = transaction;
    return txn_id;
}

int txn_insert(TransactionManager *tm, uint64_t txn_id,
               uint64_t key, uint64_t value)
{
    ActiveTransaction *transaction;
    TxnOperation *operation;
    WalInsertPayload payload;
    uint64_t old_value;
    uint32_t leaf_page_no;

    if (tm == NULL || (transaction = find_active(tm, txn_id)) == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (transaction->commit_appended || transaction->abort_appended) {
        errno = EINVAL;
        return -1;
    }
    if (bplus_tree_find_leaf_page(tm->tree, key, &leaf_page_no) != 0) {
        return -1;
    }
    memset(&payload, 0, sizeof(payload));
    payload.page_no = leaf_page_no;
    payload.key = key;
    payload.value = value;
    if (bplus_tree_find(tm->tree, key, &old_value) == 0) {
        payload.had_previous_value = 1;
        payload.previous_value = old_value;
    } else if (errno != ENOENT) {
        return -1;
    }

    operation = (TxnOperation *)calloc(1, sizeof(*operation));
    if (operation == NULL) {
        return -1;
    }
    operation->insert = payload;
    if (wal_append(tm->wal, txn_id, WAL_RECORD_INSERT,
                   &payload, (uint16_t)sizeof(payload)) == 0 ||
        wal_flush(tm->wal) != 0) {
        free(operation);
        return -1;
    }

    if (transaction->last_operation == NULL) {
        transaction->operations = operation;
    } else {
        transaction->last_operation->next = operation;
    }
    transaction->last_operation = operation;
    return bplus_tree_insert(tm->tree, key, value);
}

int txn_commit(TransactionManager *tm, uint64_t txn_id)
{
    ActiveTransaction *transaction;

    if (tm == NULL || (transaction = find_active(tm, txn_id)) == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (transaction->abort_appended) {
        errno = EINVAL;
        return -1;
    }
    if (!transaction->commit_appended) {
        if (wal_append(tm->wal, txn_id, WAL_RECORD_COMMIT, NULL, 0) == 0) {
            return -1;
        }
        transaction->commit_appended = 1;
    }
    if (wal_flush(tm->wal) != 0) {
        return -1;
    }
    tm->active = transaction->next;
    free_active(transaction);
    return 0;
}

static int undo_operation(TransactionManager *tm, const WalInsertPayload *insert)
{
    if (insert->had_previous_value) {
        return bplus_tree_insert(tm->tree, insert->key, insert->previous_value);
    }
    if (bplus_tree_delete(tm->tree, insert->key) == 0 || errno == ENOENT) {
        return 0;
    }
    return -1;
}

static int undo_operations(TransactionManager *tm, TxnOperation *operations)
{
    TxnOperation *reversed = NULL;

    while (operations != NULL) {
        TxnOperation *next = operations->next;
        operations->next = reversed;
        reversed = operations;
        operations = next;
    }
    while (reversed != NULL) {
        TxnOperation *next = reversed->next;
        if (undo_operation(tm, &reversed->insert) != 0) {
            free_operations(next);
            free(reversed);
            return -1;
        }
        free(reversed);
        reversed = next;
    }
    return 0;
}

int txn_abort(TransactionManager *tm, uint64_t txn_id)
{
    ActiveTransaction *transaction;

    if (tm == NULL || (transaction = find_active(tm, txn_id)) == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (transaction->commit_appended) {
        errno = EINVAL;
        return -1;
    }
    if (!transaction->abort_appended) {
        if (wal_append(tm->wal, txn_id, WAL_RECORD_ABORT, NULL, 0) == 0) {
            return -1;
        }
        transaction->abort_appended = 1;
    }
    if (wal_flush(tm->wal) != 0) {
        return -1;
    }
    if (undo_operations(tm, transaction->operations) != 0 ||
        buffer_pool_flush_all(tm->pool) != 0) {
        transaction->operations = NULL;
        return -1;
    }
    transaction->operations = NULL;
    tm->active = transaction->next;
    free(transaction);
    return 0;
}

static RecoveryTransaction *find_recovery_transaction(RecoveryContext *context,
                                                       uint64_t txn_id)
{
    RecoveryTransaction *transaction;
    for (transaction = context->transactions; transaction != NULL;
         transaction = transaction->next) {
        if (transaction->txn_id == txn_id) {
            return transaction;
        }
    }
    return NULL;
}

static int collect_record(const WalRecordHeader *header,
                          const void *payload, void *opaque)
{
    RecoveryContext *context = (RecoveryContext *)opaque;
    RecoveryTransaction *transaction =
        find_recovery_transaction(context, header->txn_id);

    if (header->txn_id > context->maximum_txn_id) {
        context->maximum_txn_id = header->txn_id;
    }
    if (header->type == WAL_RECORD_BEGIN) {
        if (header->length != 0 || transaction != NULL) {
            errno = EILSEQ;
            return -1;
        }
        transaction = (RecoveryTransaction *)calloc(1, sizeof(*transaction));
        if (transaction == NULL) {
            return -1;
        }
        transaction->txn_id = header->txn_id;
        if (context->last_transaction == NULL) {
            context->transactions = transaction;
        } else {
            context->last_transaction->next = transaction;
        }
        context->last_transaction = transaction;
        return 0;
    }
    if (transaction == NULL || transaction->terminal_type != 0) {
        errno = EILSEQ;
        return -1;
    }
    if (header->type == WAL_RECORD_INSERT) {
        TxnOperation *operation;
        if (header->length != sizeof(WalInsertPayload)) {
            errno = EILSEQ;
            return -1;
        }
        operation = (TxnOperation *)calloc(1, sizeof(*operation));
        if (operation == NULL) {
            return -1;
        }
        memcpy(&operation->insert, payload, sizeof(operation->insert));
        if (operation->insert.had_previous_value > 1u) {
            free(operation);
            errno = EILSEQ;
            return -1;
        }
        if (context->last_transaction == NULL ||
            context->last_transaction->txn_id != header->txn_id) {
            /* The manager permits one active transaction, so transaction records
             * must be contiguous in the log. */
            free(operation);
            errno = EILSEQ;
            return -1;
        }
        if (transaction->operations == NULL) {
            transaction->operations = operation;
        } else {
            transaction->last_operation->next = operation;
        }
        transaction->last_operation = operation;
        return 0;
    }
    if (header->length != 0 ||
        (header->type != WAL_RECORD_COMMIT && header->type != WAL_RECORD_ABORT)) {
        errno = EILSEQ;
        return -1;
    }
    transaction->terminal_type = header->type;
    return 0;
}

static int apply_recovery_transaction(TransactionManager *tm,
                                      RecoveryTransaction *transaction)
{
    TxnOperation *operation;

    if (transaction->terminal_type == WAL_RECORD_COMMIT) {
        for (operation = transaction->operations; operation != NULL;
             operation = operation->next) {
            if (bplus_tree_insert(tm->tree, operation->insert.key,
                                  operation->insert.value) != 0) {
                return -1;
            }
        }
        return 0;
    }
    {
        int undo_result = undo_operations(tm, transaction->operations);
        transaction->operations = NULL;
        transaction->last_operation = NULL;
        return undo_result;
    }
}

static void free_recovery(RecoveryTransaction *transaction)
{
    while (transaction != NULL) {
        RecoveryTransaction *next = transaction->next;
        free_operations(transaction->operations);
        free(transaction);
        transaction = next;
    }
}

int txn_recover(TransactionManager *tm)
{
    RecoveryContext context;
    RecoveryTransaction *transaction;

    if (tm == NULL || tm->active != NULL) {
        errno = tm == NULL ? EINVAL : EBUSY;
        return -1;
    }
    memset(&context, 0, sizeof(context));
    if (wal_visit(tm->wal, collect_record, &context) != 0) {
        free_recovery(context.transactions);
        return -1;
    }
    for (transaction = context.transactions; transaction != NULL;
         transaction = transaction->next) {
        if (apply_recovery_transaction(tm, transaction) != 0) {
            free_recovery(context.transactions);
            return -1;
        }
        if (transaction->terminal_type == 0) {
            transaction->terminal_type = WAL_RECORD_ABORT;
            if (wal_append(tm->wal, transaction->txn_id,
                           WAL_RECORD_ABORT, NULL, 0) == 0 ||
                wal_flush(tm->wal) != 0) {
                free_recovery(context.transactions);
                return -1;
            }
        }
    }
    if (buffer_pool_flush_all(tm->pool) != 0) {
        free_recovery(context.transactions);
        return -1;
    }
    if (context.maximum_txn_id == UINT64_MAX) {
        tm->next_txn_id = 0;
    } else if (tm->next_txn_id <= context.maximum_txn_id) {
        tm->next_txn_id = context.maximum_txn_id + 1;
    }
    free_recovery(context.transactions);
    return 0;
}

void txn_manager_destroy(TransactionManager *tm)
{
    ActiveTransaction *transaction;

    if (tm == NULL) {
        return;
    }
    transaction = tm->active;
    while (transaction != NULL) {
        ActiveTransaction *next = transaction->next;
        free_active(transaction);
        transaction = next;
    }
    free(tm);
}
