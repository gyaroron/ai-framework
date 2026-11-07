#ifndef MINI_DBMS_PAGER_H
#define MINI_DBMS_PAGER_H

#include <stdint.h>

#include "page.h"

typedef struct Pager Pager;

/* Opens or creates a page file. Returns NULL and sets errno on failure. */
Pager *pager_open(const char *path);

/* Reads/writes one complete page. Return 0 on success and -1 on failure. */
int pager_read_page(Pager *pager, uint32_t page_no, Page *out);
int pager_write_page(Pager *pager, uint32_t page_no, const Page *page);

/* Closes the file and frees the Pager. Returns 0 on success, -1 on failure. */
int pager_close(Pager *pager);

uint32_t pager_get_num_pages(const Pager *pager);

#endif
