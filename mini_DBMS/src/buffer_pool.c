#define _POSIX_C_SOURCE 200809L

#include "buffer_pool.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct BufferFrame {
    uint32_t page_no;
    size_t pin_count;
    int is_dirty;
    int is_valid;
    uint64_t lru_timestamp;
    Page page;
} BufferFrame;

struct BufferPool {
    Pager *pager;
    size_t pool_size;
    uint64_t clock;
    uint32_t next_page_no;
    BufferFrame *frames;
};

static BufferFrame *find_frame(BufferPool *pool, uint32_t page_no)
{
    size_t i;

    for (i = 0; i < pool->pool_size; ++i) {
        if (pool->frames[i].is_valid && pool->frames[i].page_no == page_no) {
            return &pool->frames[i];
        }
    }
    return NULL;
}

static BufferFrame *choose_frame(BufferPool *pool)
{
    BufferFrame *oldest = NULL;
    size_t i;

    for (i = 0; i < pool->pool_size; ++i) {
        BufferFrame *frame = &pool->frames[i];
        if (!frame->is_valid) {
            return frame;
        }
        if (frame->pin_count == 0 &&
            (oldest == NULL || frame->lru_timestamp < oldest->lru_timestamp)) {
            oldest = frame;
        }
    }
    return oldest;
}

static void touch(BufferPool *pool, BufferFrame *frame)
{
    if (pool->clock == UINT64_MAX) {
        size_t i;
        uint64_t minimum = UINT64_MAX;
        for (i = 0; i < pool->pool_size; ++i) {
            if (pool->frames[i].is_valid &&
                pool->frames[i].lru_timestamp < minimum) {
                minimum = pool->frames[i].lru_timestamp;
            }
        }
        if (minimum != UINT64_MAX) {
            for (i = 0; i < pool->pool_size; ++i) {
                if (pool->frames[i].is_valid) {
                    pool->frames[i].lru_timestamp -= minimum;
                }
            }
            pool->clock -= minimum;
        } else {
            pool->clock = 0;
        }
    }
    frame->lru_timestamp = ++pool->clock;
}

BufferPool *buffer_pool_create(Pager *pager, size_t pool_size)
{
    BufferPool *pool;

    if (pager == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (pool_size == 0) {
        pool_size = POOL_SIZE;
    }
    if (pool_size > SIZE_MAX / sizeof(BufferFrame)) {
        errno = EOVERFLOW;
        return NULL;
    }

    pool = (BufferPool *)calloc(1, sizeof(*pool));
    if (pool == NULL) {
        return NULL;
    }
    pool->frames = (BufferFrame *)calloc(pool_size, sizeof(BufferFrame));
    if (pool->frames == NULL) {
        free(pool);
        return NULL;
    }
    pool->pager = pager;
    pool->pool_size = pool_size;
    pool->next_page_no = pager_get_num_pages(pager);
    return pool;
}

Page *buffer_pool_get_page(BufferPool *pool, uint32_t page_no)
{
    BufferFrame *frame;

    if (pool == NULL) {
        errno = EINVAL;
        return NULL;
    }
    frame = find_frame(pool, page_no);
    if (frame != NULL) {
        if (frame->pin_count == SIZE_MAX) {
            errno = EOVERFLOW;
            return NULL;
        }
        ++frame->pin_count;
        touch(pool, frame);
        return &frame->page;
    }

    if (page_no >= pager_get_num_pages(pool->pager)) {
        errno = EINVAL;
        return NULL;
    }
    frame = choose_frame(pool);
    if (frame == NULL) {
        errno = EBUSY;
        return NULL;
    }
    if (frame->is_valid && frame->is_dirty &&
        pager_write_page(pool->pager, frame->page_no, &frame->page) != 0) {
        return NULL;
    }
    /* The old page is now clean; a failed read may overwrite only part of it. */
    frame->is_dirty = 0;
    if (pager_read_page(pool->pager, page_no, &frame->page) != 0) {
        frame->is_valid = 0;
        return NULL;
    }

    frame->page_no = page_no;
    frame->pin_count = 1;
    frame->is_dirty = 0;
    frame->is_valid = 1;
    touch(pool, frame);
    return &frame->page;
}

Page *buffer_pool_new_page(BufferPool *pool, uint32_t *out_page_no)
{
    BufferFrame *frame;

    if (pool == NULL || out_page_no == NULL || pool->next_page_no == UINT32_MAX) {
        errno = pool == NULL || out_page_no == NULL ? EINVAL : EOVERFLOW;
        return NULL;
    }
    frame = choose_frame(pool);
    if (frame == NULL) {
        errno = EBUSY;
        return NULL;
    }
    if (frame->is_valid && frame->is_dirty &&
        pager_write_page(pool->pager, frame->page_no, &frame->page) != 0) {
        return NULL;
    }

    *out_page_no = pool->next_page_no++;
    memset(&frame->page, 0, sizeof(frame->page));
    frame->page_no = *out_page_no;
    frame->pin_count = 1;
    frame->is_dirty = 1;
    frame->is_valid = 1;
    touch(pool, frame);
    return &frame->page;
}

int buffer_pool_unpin_page(BufferPool *pool, uint32_t page_no, int is_dirty)
{
    BufferFrame *frame;

    if (pool == NULL) {
        errno = EINVAL;
        return -1;
    }
    frame = find_frame(pool, page_no);
    if (frame == NULL || frame->pin_count == 0) {
        errno = EINVAL;
        return -1;
    }
    --frame->pin_count;
    if (is_dirty) {
        frame->is_dirty = 1;
    }
    touch(pool, frame);
    return 0;
}

int buffer_pool_flush_page(BufferPool *pool, uint32_t page_no)
{
    BufferFrame *frame;

    if (pool == NULL) {
        errno = EINVAL;
        return -1;
    }
    frame = find_frame(pool, page_no);
    if (frame == NULL) {
        errno = ENOENT;
        return -1;
    }
    if (!frame->is_dirty) {
        return 0;
    }
    if (pager_write_page(pool->pager, frame->page_no, &frame->page) != 0) {
        return -1;
    }
    frame->is_dirty = 0;
    return 0;
}

int buffer_pool_flush_all(BufferPool *pool)
{
    size_t i;

    if (pool == NULL) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < pool->pool_size; ++i) {
        BufferFrame *frame = &pool->frames[i];
        if (frame->is_valid && frame->is_dirty &&
            buffer_pool_flush_page(pool, frame->page_no) != 0) {
            return -1;
        }
    }
    return 0;
}

void buffer_pool_destroy(BufferPool *pool)
{
    if (pool == NULL) {
        return;
    }
    (void)buffer_pool_flush_all(pool);
    free(pool->frames);
    free(pool);
}
