#include "resp.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RESP_MAX_ARGS 1024u
#define RESP_MAX_BULK_LENGTH (1024u * 1024u)

typedef enum LineStatus {
    LINE_ERROR = -1,
    LINE_INCOMPLETE = 0,
    LINE_OK = 1
} LineStatus;

static LineStatus read_line(const char *buf, size_t len, size_t *position,
                            size_t *line_start, size_t *line_length)
{
    size_t i;

    for (i = *position; i < len; ++i) {
        if (buf[i] == '\n') {
            if (i == *position || buf[i - 1] != '\r') {
                return LINE_ERROR;
            }
            *line_start = *position;
            *line_length = i - *position - 1;
            *position = i + 1;
            return LINE_OK;
        }
        if (buf[i] == '\r' && i + 1 < len && buf[i + 1] != '\n') {
            return LINE_ERROR;
        }
    }
    return LINE_INCOMPLETE;
}

static int parse_decimal(const char *buf, size_t start, size_t length,
                         size_t maximum, size_t *result)
{
    size_t i;
    size_t value = 0;

    if (length == 0) {
        return 0;
    }
    for (i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)buf[start + i];
        size_t digit;

        if (c < '0' || c > '9') {
            return 0;
        }
        digit = (size_t)(c - '0');
        if (value > (maximum - digit) / 10) {
            return 0;
        }
        value = value * 10 + digit;
    }
    *result = value;
    return 1;
}

void resp_command_free(RespCommand *command)
{
    size_t i;

    if (command == NULL) {
        return;
    }
    if (command->argv != NULL) {
        for (i = 0; i < command->argc; ++i) {
            free(command->argv[i]);
        }
        free(command->argv);
    }
    command->argc = 0;
    command->argv = NULL;
}

RespParseStatus resp_parse_command(const char *buf, size_t len,
                                   size_t *consumed, RespCommand *out)
{
    size_t position = 0;
    size_t line_start;
    size_t line_length;
    size_t argc;
    size_t i;
    RespCommand parsed;
    LineStatus line_status;

    if (consumed != NULL) {
        *consumed = 0;
    }
    if (out == NULL) {
        return RESP_PARSE_ERROR;
    }
    out->argc = 0;
    out->argv = NULL;
    if (buf == NULL || consumed == NULL) {
        return RESP_PARSE_ERROR;
    }

    if (len == 0) {
        return RESP_PARSE_INCOMPLETE;
    }
    if (buf[position++] != '*') {
        return RESP_PARSE_ERROR;
    }
    line_status = read_line(buf, len, &position, &line_start, &line_length);
    if (line_status == LINE_INCOMPLETE) {
        return RESP_PARSE_INCOMPLETE;
    }
    if (line_status == LINE_ERROR ||
        !parse_decimal(buf, line_start, line_length, RESP_MAX_ARGS, &argc) ||
        argc == 0) {
        return RESP_PARSE_ERROR;
    }

    parsed.argc = argc;
    parsed.argv = (char **)calloc(argc, sizeof(char *));
    if (parsed.argv == NULL) {
        return RESP_PARSE_ERROR;
    }

    for (i = 0; i < argc; ++i) {
        size_t bulk_length;

        if (position >= len) {
            resp_command_free(&parsed);
            return RESP_PARSE_INCOMPLETE;
        }
        if (buf[position++] != '$') {
            resp_command_free(&parsed);
            return RESP_PARSE_ERROR;
        }

        line_status = read_line(buf, len, &position, &line_start, &line_length);
        if (line_status == LINE_INCOMPLETE) {
            resp_command_free(&parsed);
            return RESP_PARSE_INCOMPLETE;
        }
        if (line_status == LINE_ERROR ||
            !parse_decimal(buf, line_start, line_length,
                           RESP_MAX_BULK_LENGTH, &bulk_length)) {
            resp_command_free(&parsed);
            return RESP_PARSE_ERROR;
        }

        if (bulk_length > len - position) {
            resp_command_free(&parsed);
            return RESP_PARSE_INCOMPLETE;
        }
        if (len - position - bulk_length < 2) {
            resp_command_free(&parsed);
            return RESP_PARSE_INCOMPLETE;
        }
        if (buf[position + bulk_length] != '\r' ||
            buf[position + bulk_length + 1] != '\n') {
            resp_command_free(&parsed);
            return RESP_PARSE_ERROR;
        }

        parsed.argv[i] = (char *)malloc(bulk_length + 1);
        if (parsed.argv[i] == NULL) {
            resp_command_free(&parsed);
            return RESP_PARSE_ERROR;
        }
        memcpy(parsed.argv[i], buf + position, bulk_length);
        parsed.argv[i][bulk_length] = '\0';
        position += bulk_length + 2;
    }

    *out = parsed;
    *consumed = position;
    return RESP_PARSE_OK;
}
