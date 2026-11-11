#ifndef MINI_DBMS_THREAD_POOL_H
#define MINI_DBMS_THREAD_POOL_H

#include <stddef.h>

/*
 * thread pool: 일할 thread(worker)를 미리 여러 개 만들어 두고,
 * 들어온 작업을 대기열(queue)에 넣으면 쉬고 있는 worker가 꺼내 실행한다.
 * 요청마다 thread를 새로 만들고 없애는 비용을 없애고, 동시에 도는 thread 수를 묶어 둔다.
 */
typedef struct ThreadPool ThreadPool;

typedef void (*ThreadPoolTask)(void *argument);

/* worker thread thread_count개, 대기열 칸 queue_capacity개인 pool을 만든다. 실패하면 NULL. */
ThreadPool *thread_pool_create(size_t thread_count, size_t queue_capacity);

/*
 * 작업 하나를 대기열에 넣는다. 대기열이 꽉 차 있으면 빈칸이 생길 때까지 기다린다.
 * 성공 0. pool이 닫히는 중이면 -1.
 */
int thread_pool_submit(ThreadPool *pool, ThreadPoolTask task, void *argument);

/* 새 작업을 더 받지 않고, 대기열에 남은 작업을 모두 끝낸 뒤 worker를 정리한다. */
void thread_pool_destroy(ThreadPool *pool);

#endif
