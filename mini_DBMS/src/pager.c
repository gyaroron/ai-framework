#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L

#include "pager.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

struct Pager {
    int fd;
    uint32_t num_pages;
};

static int page_offset(uint32_t page_no, off_t *out)
{
    uint64_t offset = (uint64_t)page_no * (uint64_t)PAGE_SIZE;

    if (offset > (uint64_t)INT64_MAX - PAGE_SIZE) {
        errno = EOVERFLOW;
        return -1;
    }
    *out = (off_t)offset;
    return 0;
}

static int read_full(int fd, unsigned char *buffer, size_t length)
{
    size_t total = 0;

    while (total < length) {
        ssize_t amount = read(fd, buffer + total, length - total);
        if (amount > 0) {
            total += (size_t)amount;
        } else if (amount == 0) {
            errno = EIO;
            return -1;
        } else if (errno != EINTR) {
            return -1;
        }
    }
    return 0;
}

static int write_full(int fd, const unsigned char *buffer, size_t length)
{
    size_t total = 0;

    while (total < length) {
        ssize_t amount = write(fd, buffer + total, length - total);
        if (amount > 0) {
            total += (size_t)amount;
        } else if (amount == 0) {
            errno = EIO;
            return -1;
        } else if (errno != EINTR) {
            return -1;
        }
    }
    return 0;
}

Pager *pager_open(const char *path)
{
    Pager *pager;
    off_t file_size;
    uint64_t page_count;
    int fd;

    if (path == NULL) {
        errno = EINVAL;
        return NULL;
    }
    fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        return NULL;
    }
    file_size = lseek(fd, 0, SEEK_END);
    if (file_size < 0) {
        int saved_errno = errno;
        (void)close(fd);
        errno = saved_errno;
        return NULL;
    }
    if ((uint64_t)file_size % PAGE_SIZE != 0) {
        (void)close(fd);
        errno = EINVAL;
        return NULL;
    }
    page_count = (uint64_t)file_size / PAGE_SIZE;
    if (page_count > UINT32_MAX) {
        (void)close(fd);
        errno = EOVERFLOW;
        return NULL;
    }

    pager = (Pager *)malloc(sizeof(*pager));
    if (pager == NULL) {
        int saved_errno = errno;
        (void)close(fd);
        errno = saved_errno;
        return NULL;
    }
    pager->fd = fd;
    pager->num_pages = (uint32_t)page_count;
    return pager;
}

int pager_read_page(Pager *pager, uint32_t page_no, Page *out)
{
    off_t offset;

    if (pager == NULL || out == NULL || page_no >= pager->num_pages) {
        errno = EINVAL;
        return -1;
    }
    if (page_offset(page_no, &offset) != 0 || lseek(pager->fd, offset, SEEK_SET) < 0) {
        return -1;
    }
    return read_full(pager->fd, out->bytes, PAGE_SIZE);
}

int pager_write_page(Pager *pager, uint32_t page_no, const Page *page)
{
    off_t offset;
    uint32_t new_page_count;

    if (pager == NULL || page == NULL || page_no == UINT32_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (page_offset(page_no, &offset) != 0 || lseek(pager->fd, offset, SEEK_SET) < 0) {
        return -1;
    }
    if (write_full(pager->fd, page->bytes, PAGE_SIZE) != 0) {
        return -1;
    }
    new_page_count = page_no + 1;
    if (new_page_count > pager->num_pages) {
        pager->num_pages = new_page_count;
    }
    return 0;
}

int pager_close(Pager *pager)
{
    int result;

    if (pager == NULL) {
        errno = EINVAL;
        return -1;
    }
    result = close(pager->fd);
    free(pager);
    return result;
}

uint32_t pager_get_num_pages(const Pager *pager)
{
    return pager == NULL ? 0 : pager->num_pages;
}
