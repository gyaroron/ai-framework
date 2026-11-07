# Mini-Redis Hash Table

A C99 hash table implementation using 16 fixed buckets, the specified
polynomial rolling hash, and singly linked list chaining. Keys and values are
copied into table-owned memory.

## Build

```sh
cc -std=c99 -Wall -Wextra -Wpedantic hashtable.c main.c -o mini_redis
```

## API

- `ht_create` / `ht_destroy`: allocate and release a table.
- `ht_set`: add a key or replace its value; returns `0` on success and `-1` on
  invalid arguments or allocation failure.
- `ht_get`: return a borrowed pointer to the stored value, or `NULL` if absent.
  The pointer remains valid until that key is updated/deleted or the table is
  destroyed.
- `ht_delete`: return `1` if removed, `0` if absent, and `-1` for invalid
  arguments.
- `ht_hash`: return the bucket index in `[0, CAPACITY)`.

The table is not thread-safe. A returned value pointer must not be used after
an update or deletion of its key, or after `ht_destroy`.
