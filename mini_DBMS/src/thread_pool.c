#define _POSIX_C_SOURCE 200809L

#include "thread_pool.h"

#include <pthread.h>
#include <stdlib.h>

typedef struct QueuedTask {
    ThreadPoolTask task;
    void *argument;
} QueuedTask;

struct ThreadPool {
    pthread_t *threads;
    size_t thread_count;
    /* 원형 대기열(circular queue): head에서 꺼내고 (head + count) 자리에 넣는다. */
    QueuedTask *queue;
    size_t queue_capacity;
    size_t head;
    size_t count;
    int is_stopping;
    /* 대기열과 is_stopping을 지키는 lock. */
    pthread_mutex_t mutex;
    /* 대기열에 작업이 들어왔음을 worker에게 알린다. */
    pthread_cond_t has_task;
    /* 대기열에 빈칸이 생겼음을 submit 쪽에 알린다. */
    pthread_cond_t has_space;
};

/* worker thread가 도는 함수: 작업을 꺼내 실행하는 일을 pool이 닫힐 때까지 반복한다. */
static void *worker_main(void *argument)
{
    ThreadPool *pool = (ThreadPool *)argument;

    for (;;) {
        QueuedTask task;

        pthread_mutex_lock(&pool->mutex);
        while (pool->count == 0 && !pool->is_stopping) {
            pthread_cond_wait(&pool->has_task, &pool->mutex);
        }
        if (pool->count == 0) {
            /* 닫히는 중이고 남은 작업도 없다. */
            pthread_mutex_unlock(&pool->mutex);
            return NULL;
        }
        task = pool->queue[pool->head];
        pool->head = (pool->head + 1) % pool->queue_capacity;
        --pool->count;
        pthread_cond_signal(&pool->has_space);
        pthread_mutex_unlock(&pool->mutex);

        /* lock을 푼 뒤에 실행한다. 그래야 다른 worker가 동시에 다른 작업을 꺼낼 수 있다. */
        task.task(task.argument);
    }
}

ThreadPool *thread_pool_create(size_t thread_count, size_t queue_capacity)
{
    ThreadPool *pool;
    size_t started = 0;

    if (thread_count == 0 || queue_capacity == 0) {
        return NULL;
    }
    pool = (ThreadPool *)calloc(1, sizeof(ThreadPool));
    if (pool == NULL) {
        return NULL;
    }
    pool->threads = (pthread_t *)calloc(thread_count, sizeof(pthread_t));
    pool->queue = (QueuedTask *)calloc(queue_capacity, sizeof(QueuedTask));
    if (pool->threads == NULL || pool->queue == NULL) {
        free(pool->threads);
        free(pool->queue);
        free(pool);
        return NULL;
    }
    pool->queue_capacity = queue_capacity;
    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->has_task, NULL);
    pthread_cond_init(&pool->has_space, NULL);

    for (started = 0; started < thread_count; ++started) {
        if (pthread_create(&pool->threads[started], NULL, worker_main, pool) != 0) {
            break;
        }
    }
    pool->thread_count = started;
    if (started != thread_count) {
        /* 일부만 만들어졌으면 만든 것까지 정리하고 실패로 알린다. */
        thread_pool_destroy(pool);
        return NULL;
    }
    return pool;
}

int thread_pool_submit(ThreadPool *pool, ThreadPoolTask task, void *argument)
{
    size_t tail;

    if (pool == NULL || task == NULL) {
        return -1;
    }
    pthread_mutex_lock(&pool->mutex);
    while (pool->count == pool->queue_capacity && !pool->is_stopping) {
        pthread_cond_wait(&pool->has_space, &pool->mutex);
    }
    if (pool->is_stopping) {
        pthread_mutex_unlock(&pool->mutex);
        return -1;
    }
    tail = (pool->head + pool->count) % pool->queue_capacity;
    pool->queue[tail].task = task;
    pool->queue[tail].argument = argument;
    ++pool->count;
    pthread_cond_signal(&pool->has_task);
    pthread_mutex_unlock(&pool->mutex);
    return 0;
}

void thread_pool_destroy(ThreadPool *pool)
{
    size_t i;

    if (pool == NULL) {
        return;
    }
    pthread_mutex_lock(&pool->mutex);
    pool->is_stopping = 1;
    /* 잠들어 있는 모든 worker와 submit 호출을 깨워 종료 여부를 확인하게 한다. */
    pthread_cond_broadcast(&pool->has_task);
    pthread_cond_broadcast(&pool->has_space);
    pthread_mutex_unlock(&pool->mutex);

    for (i = 0; i < pool->thread_count; ++i) {
        pthread_join(pool->threads[i], NULL);
    }
    pthread_cond_destroy(&pool->has_task);
    pthread_cond_destroy(&pool->has_space);
    pthread_mutex_destroy(&pool->mutex);
    free(pool->threads);
    free(pool->queue);
    free(pool);
}
