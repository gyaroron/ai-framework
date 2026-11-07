#ifndef MINI_REDIS_SERVER_H
#define MINI_REDIS_SERVER_H

#include "hashtable.h"

#define SERVER_DEFAULT_PORT 6379u

/* Runs the single-threaded, nonblocking POSIX TCP server on port 6379. */
int server_run(HashTable *table);

#endif
