#include "resp.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); \
            return 1; \
        } \
    } while (0)

static int test_complete_request(void)
{
    static const char request[] =
        "*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$4\r\nJohn\r\n";
    RespCommand command = {0, NULL};
    size_t consumed = 0;
    RespParseStatus status = resp_parse_command(
        request, sizeof(request) - 1, &consumed, &command);

    CHECK(status == RESP_PARSE_OK, "complete request should parse");
    CHECK(command.argc == 3, "argc should be 3");
    CHECK(consumed == 33, "consumed should be 33");
    CHECK(strcmp(command.argv[0], "SET") == 0, "argv[0] should be SET");
    CHECK(strcmp(command.argv[1], "name") == 0, "argv[1] should be name");
    CHECK(strcmp(command.argv[2], "John") == 0, "argv[2] should be John");
    resp_command_free(&command);
    return 0;
}

static int test_incomplete_request(void)
{
    static const char request[] = "*3\r\n$3\r\nSE";
    RespCommand command = {0, NULL};
    size_t consumed = 99;
    RespParseStatus status = resp_parse_command(
        request, sizeof(request) - 1, &consumed, &command);

    CHECK(status == RESP_PARSE_INCOMPLETE, "partial request should be incomplete");
    CHECK(command.argc == 0 && command.argv == NULL,
          "incomplete result should not allocate output");
    CHECK(consumed == 0, "incomplete result should consume no bytes");
    return 0;
}

static int test_pipelined_requests(void)
{
    static const char requests[] =
        "*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPING\r\n";
    const char *buffer = requests;
    size_t remaining = sizeof(requests) - 1;
    RespCommand command = {0, NULL};
    size_t consumed = 0;
    RespParseStatus status;

    status = resp_parse_command(buffer, remaining, &consumed, &command);
    CHECK(status == RESP_PARSE_OK, "first pipelined request should parse");
    CHECK(command.argc == 1 && strcmp(command.argv[0], "PING") == 0,
          "first request should be PING");
    CHECK(consumed == 14, "first request should consume 14 bytes");
    resp_command_free(&command);

    buffer += consumed;
    remaining -= consumed;
    consumed = 0;
    status = resp_parse_command(buffer, remaining, &consumed, &command);
    CHECK(status == RESP_PARSE_OK, "second pipelined request should parse");
    CHECK(command.argc == 1 && strcmp(command.argv[0], "PING") == 0,
          "second request should be PING");
    CHECK(consumed == 14, "second request should consume 14 bytes");
    resp_command_free(&command);
    return 0;
}

int main(void)
{
    if (test_complete_request() != 0 ||
        test_incomplete_request() != 0 ||
        test_pipelined_requests() != 0) {
        return 1;
    }
    puts("All RESP parser tests passed.");
    return 0;
}
