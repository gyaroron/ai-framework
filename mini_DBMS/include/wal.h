#ifndef MINI_DBMS_WAL_H
#define MINI_DBMS_WAL_H

#include <stdint.h>

#define WAL_RECORD_BEGIN 0x01u
#define WAL_RECORD_INSERT 0x02u
#define WAL_RECORD_COMMIT 0x03u
#define WAL_RECORD_ABORT 0x04u
#define WAL_WIRE_HEADER_SIZE 20u
#define WAL_CHECKSUM_SIZE 4u

typedef struct Wal Wal;

typedef struct WalRecordHeader {
    uint64_t lsn;
    uint64_t txn_id;
    uint16_t type;
    uint16_t length;
} WalRecordHeader;

/* Wire records serialize the header, payload bytes, then this CRC32 footer. */
typedef struct WalRecordChecksum {
    uint32_t crc32;
} WalRecordChecksum;

/* Logical INSERT record before-image. page_no is the leaf targeted at log time. */
typedef struct WalInsertPayload {
    uint32_t page_no;
    uint8_t had_previous_value;
    uint8_t reserved[3];
    uint64_t key;
    uint64_t value;
    uint64_t previous_value;
} WalInsertPayload;

typedef int (*WalVisitor)(const WalRecordHeader *header,
                          const void *payload,
                          void *context);

Wal *wal_open(const char *log_path);
uint64_t wal_append(Wal *wal, uint64_t txn_id, uint16_t type,
                    const void *payload, uint16_t length);
int wal_flush(Wal *wal);
int wal_visit(Wal *wal, WalVisitor visitor, void *context);
void wal_close(Wal *wal);

#endif
