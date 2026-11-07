#ifndef MINI_DBMS_PAGE_H
#define MINI_DBMS_PAGE_H

#define PAGE_SIZE 4096u

typedef struct Page {
    unsigned char bytes[PAGE_SIZE];
} Page;

#endif
