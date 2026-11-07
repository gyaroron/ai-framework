#define _POSIX_C_SOURCE 200809L

#include "pager.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_FILE "test_db.bin"

static int fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

int main(void)
{
    Pager *pager = NULL;
    Page written;
    Page read_back;
    struct stat file_info;
    size_t i;
    int result = 1;

    (void)unlink(TEST_FILE);
    pager = pager_open(TEST_FILE);
    if (pager == NULL) {
        return fail("could not create test file");
    }
    if (pager_get_num_pages(pager) != 0) {
        fail("new file should have zero pages");
        goto cleanup;
    }

    for (i = 0; i < PAGE_SIZE; ++i) {
        written.bytes[i] = (unsigned char)(i % 251u);
    }
    if (pager_write_page(pager, 0, &written) != 0) {
        fail("could not write page zero");
        goto cleanup;
    }
    if (pager_get_num_pages(pager) != 1) {
        fail("page count should become one after page zero write");
        goto cleanup;
    }
    if (pager_close(pager) != 0) {
        pager = NULL;
        fail("could not close pager after first write");
        goto cleanup;
    }
    pager = NULL;

    pager = pager_open(TEST_FILE);
    if (pager == NULL || pager_read_page(pager, 0, &read_back) != 0) {
        fail("could not reopen and read page zero");
        goto cleanup;
    }
    if (memcmp(written.bytes, read_back.bytes, PAGE_SIZE) != 0) {
        fail("reopened page contents do not match all 4096 bytes");
        goto cleanup;
    }

    memset(&written, 0xa5, sizeof(written));
    if (pager_write_page(pager, 2, &written) != 0) {
        fail("could not write non-contiguous page two");
        goto cleanup;
    }
    if (pager_get_num_pages(pager) != 3) {
        fail("page count should become three after writing page two");
        goto cleanup;
    }
    if (stat(TEST_FILE, &file_info) != 0 ||
        file_info.st_size != (off_t)(3u * PAGE_SIZE)) {
        fail("file size should be exactly 12288 bytes");
        goto cleanup;
    }

    if (pager_read_page(pager, 3, &read_back) == 0) {
        fail("reading page at num_pages should fail");
        goto cleanup;
    }
    if (pager_read_page(pager, 1, &read_back) != 0) {
        fail("sparse page one inside the file range should be readable");
        goto cleanup;
    }
    for (i = 0; i < PAGE_SIZE; ++i) {
        if (read_back.bytes[i] != 0) {
            fail("new sparse page should read back as zero-filled");
            goto cleanup;
        }
    }

    result = 0;

cleanup:
    if (pager != NULL && pager_close(pager) != 0 && result == 0) {
        result = fail("could not close pager");
    }
    (void)unlink(TEST_FILE);
    if (result == 0) {
        puts("All pager tests passed.");
    }
    return result;
}
