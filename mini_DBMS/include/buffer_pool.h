#ifndef MINI_DBMS_BUFFER_POOL_H
#define MINI_DBMS_BUFFER_POOL_H

#include <stddef.h>
#include <stdint.h>

#include "pager.h"

#define POOL_SIZE 8u

typedef struct BufferPool BufferPool;

/* pool_size == 0 selects POOL_SIZE. */
BufferPool *buffer_pool_create(Pager *pager, size_t pool_size);

/* Returns a pinned page, or NULL on I/O error, missing page, or no free frame. */
Page *buffer_pool_get_page(BufferPool *pool, uint32_t page_no);

/* Allocates the next page number, zero-fills it, and returns it pinned. */
Page *buffer_pool_new_page(BufferPool *pool, uint32_t *out_page_no);

/* Releases one pin and optionally marks the resident page dirty. */
int buffer_pool_unpin_page(BufferPool *pool, uint32_t page_no, int is_dirty);

int buffer_pool_flush_page(BufferPool *pool, uint32_t page_no);
int buffer_pool_flush_all(BufferPool *pool);
void buffer_pool_destroy(BufferPool *pool);

#endif
