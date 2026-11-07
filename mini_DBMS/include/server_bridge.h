#ifndef MINI_DBMS_SERVER_BRIDGE_H
#define MINI_DBMS_SERVER_BRIDGE_H

#include "bplus_tree.h"
#include "transaction.h"

/* Runs the RESP bridge on port 6380 until SIGINT or SIGTERM is received. */
int server_bridge_run(TransactionManager *transactions, BPlusTree *tree);

#endif
