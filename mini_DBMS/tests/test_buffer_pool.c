#define _POSIX_C_SOURCE 200809L

#include "buffer_pool.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST_FILE "test_buffer_pool.bin"

static void initialize_page_file(Pager *pager, uint32_t count)
{
    uint32_t page_no;

    for (page_no = 0; page_no < count; ++page_no) {
        Page page;
        memset(&page, (int)(0x10u + page_no), sizeof(page));
        assert(pager_write_page(pager, page_no, &page) == 0);
    }
}

int main(void)
{
    Pager *pager;
    BufferPool *pool;
    Page *page0;
    Page *page1;
    Page *page2;
    Page disk_page;

    (void)unlink(TEST_FILE);
    pager = pager_open(TEST_FILE);
    assert(pager != NULL);
    initialize_page_file(pager, 4);

    /* 1. A hit pins and returns the same in-pool Page address. */
    pool = buffer_pool_create(pager, 2);
    assert(pool != NULL);
    page0 = buffer_pool_get_page(pool, 0);
    assert(page0 != NULL);
    assert(buffer_pool_unpin_page(pool, 0, 0) == 0);
    assert(buffer_pool_get_page(pool, 0) == page0);
    assert(buffer_pool_unpin_page(pool, 0, 0) == 0);
    buffer_pool_destroy(pool);
    puts("PASS 1: cache hit returned the same page address");

    /* 2. Touch page zero after page one; page one must be evicted for page two. */
    pool = buffer_pool_create(pager, 2);
    assert(pool != NULL);
    page0 = buffer_pool_get_page(pool, 0);
    assert(page0 != NULL && buffer_pool_unpin_page(pool, 0, 0) == 0);
    page1 = buffer_pool_get_page(pool, 1);
    assert(page1 != NULL && buffer_pool_unpin_page(pool, 1, 0) == 0);
    assert(buffer_pool_get_page(pool, 0) == page0);
    assert(buffer_pool_unpin_page(pool, 0, 0) == 0);
    page2 = buffer_pool_get_page(pool, 2);
    assert(page2 == page1);
    assert(buffer_pool_unpin_page(pool, 2, 0) == 0);
    buffer_pool_destroy(pool);
    puts("PASS 2: LRU evicted the oldest unpinned page");

    /* 3. Evicting a dirty page writes the modified bytes through the Pager. */
    pool = buffer_pool_create(pager, 1);
    assert(pool != NULL);
    page0 = buffer_pool_get_page(pool, 0);
    assert(page0 != NULL);
    memset(page0->bytes, 0x7b, PAGE_SIZE);
    assert(buffer_pool_unpin_page(pool, 0, 1) == 0);
    assert(buffer_pool_get_page(pool, 1) != NULL);
    assert(pager_read_page(pager, 0, &disk_page) == 0);
    for (size_t i = 0; i < PAGE_SIZE; ++i) {
        assert(disk_page.bytes[i] == 0x7b);
    }
    buffer_pool_destroy(pool);
    puts("PASS 3: dirty page was flushed before eviction");

    /* 4. Pinned frames cannot be selected; all-pinned pools report EBUSY. */
    pool = buffer_pool_create(pager, 2);
    assert(pool != NULL);
    page0 = buffer_pool_get_page(pool, 0);
    page1 = buffer_pool_get_page(pool, 1);
    assert(page0 != NULL && page1 != NULL);
    errno = 0;
    assert(buffer_pool_get_page(pool, 2) == NULL);
    assert(errno == EBUSY);

    assert(buffer_pool_unpin_page(pool, 1, 0) == 0);
    page2 = buffer_pool_get_page(pool, 2);
    assert(page2 == page1);
    errno = 0;
    assert(buffer_pool_get_page(pool, 3) == NULL);
    assert(errno == EBUSY);
    assert(buffer_pool_unpin_page(pool, 0, 0) == 0);
    assert(buffer_pool_unpin_page(pool, 2, 0) == 0);
    buffer_pool_destroy(pool);
    puts("PASS 4: pinned frames were protected from replacement");

    /* 5. A partial read followed by EOF must not poison the old page. */
    for (int dirty = 0; dirty <= 1; ++dirty) {
        Page expected;
        initialize_page_file(pager, 4);
        pool = buffer_pool_create(pager, 1);
        assert(pool != NULL);
        page0 = buffer_pool_get_page(pool, 0);
        assert(page0 != NULL);
        if (dirty) {
            memset(page0->bytes, 0x7b, PAGE_SIZE);
        }
        expected = *page0;
        assert(buffer_pool_unpin_page(pool, 0, dirty) == 0);

        /* Pager still knows four pages; page one now ends halfway through. */
        assert(truncate(TEST_FILE, PAGE_SIZE + PAGE_SIZE / 2) == 0);
        errno = 0;
        assert(buffer_pool_get_page(pool, 1) == NULL);
        assert(errno == EIO);
        assert(buffer_pool_flush_all(pool) == 0);
        assert(pager_read_page(pager, 0, &disk_page) == 0);
        assert(memcmp(&disk_page, &expected, sizeof(expected)) == 0);

        page0 = buffer_pool_get_page(pool, 0);
        assert(page0 != NULL);
        assert(memcmp(page0, &expected, sizeof(expected)) == 0);
        assert(buffer_pool_unpin_page(pool, 0, 0) == 0);
        buffer_pool_destroy(pool);
        assert(pager_read_page(pager, 0, &disk_page) == 0);
        assert(memcmp(&disk_page, &expected, sizeof(expected)) == 0);
    }
    puts("PASS 5: failed partial reads preserved clean and dirty old pages");

    assert(pager_close(pager) == 0);
    assert(unlink(TEST_FILE) == 0);
    puts("All buffer pool tests passed.");
    return 0;
}
