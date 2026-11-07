#include "hashtable.h"
#include "server.h"

#include <stdio.h>
#include <string.h>

static int run_hash_table_checks(void)
{
    HashTable *table = ht_create();
    const char *value;
    int delete_result;
    int failed = 0;

    puts("=== Mini-Redis hash table five-step verification ===");
    if (table == NULL) {
        fputs("Failed to create hash table.\n", stderr);
        return 1;
    }
    puts("[1/5] Created hash table with 16 buckets.");

    if (ht_set(table, "server:ip", "192.168.0.1") != 0) {
        fputs("[2/5] Insert failed.\n", stderr);
        ht_destroy(table);
        return 1;
    }
    puts("[2/5] Inserted server:ip = 192.168.0.1.");

    value = ht_get(table, "server:ip");
    if (value == NULL || strcmp(value, "192.168.0.1") != 0) {
        fputs("[3/5] Initial lookup did not match expected value.\n", stderr);
        failed = 1;
    } else {
        printf("[3/5] Lookup returned %s.\n", value);
    }

    if (ht_set(table, "server:ip", "10.0.0.1") != 0) {
        fputs("[4/5] Update failed.\n", stderr);
        failed = 1;
    } else {
        value = ht_get(table, "server:ip");
        if (value == NULL || strcmp(value, "10.0.0.1") != 0) {
            fputs("[4/5] Updated value did not match expected value.\n", stderr);
            failed = 1;
        } else {
            printf("[4/5] Updated lookup returned %s.\n", value);
        }
    }

    delete_result = ht_delete(table, "server:ip");
    value = ht_get(table, "server:ip");
    if (delete_result != 1 || value != NULL) {
        fputs("[5/5] Delete or post-delete lookup failed.\n", stderr);
        failed = 1;
    } else {
        puts("[5/5] Delete returned 1 and lookup returned NULL.");
    }

    ht_destroy(table);
    if (failed) {
        return 1;
    }
    puts("=== All five checks passed; table memory released. ===");
    return 0;
}

int main(int argc, char **argv)
{
    HashTable *table;
    int result;

    if (argc == 1) {
        return run_hash_table_checks();
    }
    if (argc != 2 || strcmp(argv[1], "--server") != 0) {
        fprintf(stderr, "Usage: %s [--server]\n", argv[0]);
        return 2;
    }

    table = ht_create();
    if (table == NULL) {
        fputs("Failed to create hash table.\n", stderr);
        return 1;
    }

    printf("Mini-Redis listening on port %u. Press Ctrl+C to stop.\n",
           SERVER_DEFAULT_PORT);
    result = server_run(table);
    if (result != 0) {
        perror("server_run");
    }
    ht_destroy(table);
    return result == 0 ? 0 : 1;
}
