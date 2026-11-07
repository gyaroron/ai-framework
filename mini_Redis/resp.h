#ifndef MINI_REDIS_RESP_H
#define MINI_REDIS_RESP_H

#include <stddef.h>

typedef struct RespCommand {
    size_t argc;
    char **argv;
} RespCommand;

typedef enum RespParseStatus {
    RESP_PARSE_ERROR = -1,
    RESP_PARSE_INCOMPLETE = 0,
    RESP_PARSE_OK = 1
} RespParseStatus;

/*
 * Parses one RESP Array of bulk strings from buf[0..len).
 * On OK, sets consumed to the request size and out to the allocated command.
 * On INCOMPLETE or ERROR, consumed is zero and out is empty.
 * The returned command owns all argument strings; release it with
 * resp_command_free().
 */
RespParseStatus resp_parse_command(
    const char *buf,
    size_t len,
    size_t *consumed,
    RespCommand *out
);

void resp_command_free(RespCommand *command);

#endif
