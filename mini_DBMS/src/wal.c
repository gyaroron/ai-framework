#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L

#include "wal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct Wal {
    int fd;
    off_t end_offset;
    uint64_t next_lsn;
};

static uint32_t crc32_bytes(const unsigned char *data, size_t length)
{
    uint32_t crc = UINT32_C(0xffffffff);
    size_t i;

    for (i = 0; i < length; ++i) {
        unsigned int bit;
        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return ~crc;
}

static int write_all(int fd, const unsigned char *buffer, size_t length)
{
    size_t written = 0;

    while (written < length) {
        ssize_t amount = write(fd, buffer + written, length - written);
        if (amount > 0) {
            written += (size_t)amount;
        } else if (amount == 0) {
            errno = EIO;
            return -1;
        } else if (errno != EINTR) {
            return -1;
        }
    }
    return 0;
}

static int pread_all(int fd, unsigned char *buffer, size_t length,
                     off_t offset, size_t *read_count)
{
    size_t total = 0;

    while (total < length) {
        ssize_t amount = pread(fd, buffer + total, length - total,
                               offset + (off_t)total);
        if (amount > 0) {
            total += (size_t)amount;
        } else if (amount == 0) {
            break;
        } else if (errno != EINTR) {
            return -1;
        }
    }
    *read_count = total;
    return 0;
}

static int decode_header(const unsigned char *wire, WalRecordHeader *header)
{
    memcpy(&header->lsn, wire, sizeof(header->lsn));
    memcpy(&header->txn_id, wire + 8, sizeof(header->txn_id));
    memcpy(&header->type, wire + 16, sizeof(header->type));
    memcpy(&header->length, wire + 18, sizeof(header->length));
    return 0;
}

static int valid_record_type(uint16_t type)
{
    return type == WAL_RECORD_BEGIN || type == WAL_RECORD_INSERT ||
           type == WAL_RECORD_COMMIT || type == WAL_RECORD_ABORT;
}

static int scan_and_repair(Wal *wal)
{
    struct stat info;
    off_t offset = 0;
    uint64_t expected_lsn = 1;

    if (fstat(wal->fd, &info) != 0) {
        return -1;
    }
    while (offset < info.st_size) {
        unsigned char header_wire[WAL_WIRE_HEADER_SIZE];
        WalRecordHeader header;
        size_t got;
        size_t record_size;
        unsigned char *record;
        uint32_t expected_crc;
        uint32_t actual_crc;

        if (pread_all(wal->fd, header_wire, sizeof(header_wire), offset, &got) != 0) {
            return -1;
        }
        if (got < sizeof(header_wire)) {
            break;
        }
        (void)decode_header(header_wire, &header);
        record_size = WAL_WIRE_HEADER_SIZE + (size_t)header.length + WAL_CHECKSUM_SIZE;
        if ((uint64_t)record_size > (uint64_t)(info.st_size - offset)) {
            break;
        }
        if (header.lsn != expected_lsn || header.lsn == 0 ||
            header.txn_id == 0 || !valid_record_type(header.type)) {
            errno = EILSEQ;
            return -1;
        }
        record = (unsigned char *)malloc(record_size);
        if (record == NULL) {
            return -1;
        }
        if (pread_all(wal->fd, record, record_size, offset, &got) != 0 ||
            got != record_size) {
            free(record);
            if (errno == 0) errno = EIO;
            return -1;
        }
        memcpy(&expected_crc, record + record_size - WAL_CHECKSUM_SIZE,
               sizeof(expected_crc));
        actual_crc = crc32_bytes(record, record_size - WAL_CHECKSUM_SIZE);
        free(record);
        if (expected_crc != actual_crc) {
            errno = EILSEQ;
            return -1;
        }
        if (expected_lsn == UINT64_MAX) {
            expected_lsn = 0;
        } else if (expected_lsn != 0) {
            ++expected_lsn;
        }
        offset += (off_t)record_size;
    }

    if (offset != info.st_size && ftruncate(wal->fd, offset) != 0) {
        return -1;
    }
    wal->end_offset = offset;
    wal->next_lsn = expected_lsn;
    if (lseek(wal->fd, wal->end_offset, SEEK_SET) < 0) {
        return -1;
    }
    return 0;
}

Wal *wal_open(const char *log_path)
{
    Wal *wal;
    int fd;

    if (log_path == NULL) {
        errno = EINVAL;
        return NULL;
    }
    fd = open(log_path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        return NULL;
    }
    wal = (Wal *)calloc(1, sizeof(*wal));
    if (wal == NULL) {
        int saved_errno = errno;
        (void)close(fd);
        errno = saved_errno;
        return NULL;
    }
    wal->fd = fd;
    wal->next_lsn = 1;
    if (scan_and_repair(wal) != 0) {
        int saved_errno = errno;
        (void)close(fd);
        free(wal);
        errno = saved_errno;
        return NULL;
    }
    return wal;
}

uint64_t wal_append(Wal *wal, uint64_t txn_id, uint16_t type,
                    const void *payload, uint16_t length)
{
    size_t record_size = WAL_WIRE_HEADER_SIZE + (size_t)length + WAL_CHECKSUM_SIZE;
    unsigned char *record;
    uint64_t lsn;
    uint16_t wire_type = type;
    uint16_t wire_length = length;
    uint32_t crc;

    if (wal == NULL || txn_id == 0 || !valid_record_type(type) ||
        (length != 0 && payload == NULL) || wal->next_lsn == 0) {
        if (wal != NULL && wal->next_lsn == 0) {
            errno = EOVERFLOW;
            return 0;
        }
        errno = EINVAL;
        return 0;
    }
    if ((uint64_t)wal->end_offset >
        (uint64_t)INT64_MAX - (uint64_t)record_size) {
        errno = EOVERFLOW;
        return 0;
    }
    record = (unsigned char *)malloc(record_size);
    if (record == NULL) {
        return 0;
    }
    lsn = wal->next_lsn;
    memcpy(record, &lsn, sizeof(lsn));
    memcpy(record + 8, &txn_id, sizeof(txn_id));
    memcpy(record + 16, &wire_type, sizeof(wire_type));
    memcpy(record + 18, &wire_length, sizeof(wire_length));
    if (length != 0) {
        memcpy(record + WAL_WIRE_HEADER_SIZE, payload, length);
    }
    crc = crc32_bytes(record, record_size - WAL_CHECKSUM_SIZE);
    memcpy(record + record_size - WAL_CHECKSUM_SIZE, &crc, sizeof(crc));

    if (lseek(wal->fd, wal->end_offset, SEEK_SET) < 0 ||
        write_all(wal->fd, record, record_size) != 0) {
        int saved_errno = errno;
        (void)ftruncate(wal->fd, wal->end_offset);
        (void)lseek(wal->fd, wal->end_offset, SEEK_SET);
        free(record);
        errno = saved_errno;
        return 0;
    }
    free(record);
    wal->end_offset += (off_t)record_size;
    wal->next_lsn = lsn == UINT64_MAX ? 0 : lsn + 1;
    return lsn;
}

int wal_flush(Wal *wal)
{
    if (wal == NULL) {
        errno = EINVAL;
        return -1;
    }
    return fsync(wal->fd);
}

int wal_visit(Wal *wal, WalVisitor visitor, void *context)
{
    off_t offset = 0;

    if (wal == NULL || visitor == NULL) {
        errno = EINVAL;
        return -1;
    }
    while (offset < wal->end_offset) {
        unsigned char header_wire[WAL_WIRE_HEADER_SIZE];
        WalRecordHeader header;
        unsigned char *record;
        size_t record_size;
        size_t got;
        int callback_result;

        if (pread_all(wal->fd, header_wire, sizeof(header_wire), offset, &got) != 0 ||
            got != sizeof(header_wire)) {
            if (errno == 0) errno = EIO;
            return -1;
        }
        (void)decode_header(header_wire, &header);
        record_size = WAL_WIRE_HEADER_SIZE + (size_t)header.length + WAL_CHECKSUM_SIZE;
        record = (unsigned char *)malloc(record_size);
        if (record == NULL) {
            return -1;
        }
        if (pread_all(wal->fd, record, record_size, offset, &got) != 0 ||
            got != record_size) {
            free(record);
            if (errno == 0) errno = EIO;
            return -1;
        }
        callback_result = visitor(&header, record + WAL_WIRE_HEADER_SIZE, context);
        free(record);
        if (callback_result != 0) {
            return callback_result;
        }
        offset += (off_t)record_size;
    }
    return 0;
}

void wal_close(Wal *wal)
{
    if (wal == NULL) {
        return;
    }
    (void)fsync(wal->fd);
    (void)close(wal->fd);
    free(wal);
}
