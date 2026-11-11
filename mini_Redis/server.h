#ifndef MINI_REDIS_SERVER_H
#define MINI_REDIS_SERVER_H

#include "hashtable.h"

#define SERVER_DEFAULT_PORT 6379u

/* Runs the single-threaded, nonblocking POSIX TCP server on port 6379. */
int server_run(HashTable *table);

/*
 * server_run과 같지만 snapshot_path가 NULL이 아니면
 * ① 시작할 때 그 파일을 읽어 테이블을 채우고
 * ② key가 바뀐 뒤 1초 안에 파일을 다시 쓰고
 * ③ SIGINT · SIGTERM을 받으면 마지막으로 한 번 더 쓴 뒤 0을 돌려준다.
 */
int server_run_with_snapshot(HashTable *table, const char *snapshot_path);

#endif
