#define _POSIX_C_SOURCE 200809L

/*
 * 미니 DBMS API 서버.
 *
 *   mini_dbms_sql_server <data_dir> [port=6381] [threads=4]
 *
 * 외부 client는 TCP로 접속해 RESP 형식(Mini Redis와 같은 통신 형식)으로 명령을 보낸다.
 *   PING              → +PONG
 *   SQL "<SQL 문장>"   → 실행 결과 글자(bulk string), 실패하면 -ERR 이유
 *
 * 구조
 *   main thread : accept로 접속을 받아 thread pool 대기열에 넣는다.
 *   worker thread : 대기열에서 접속 하나를 꺼내, 그 접속이 보낸 요청을 SQL 엔진으로 실행한다.
 *   SQL 엔진 : SELECT는 읽기 lock, INSERT는 쓰기 lock으로 여러 worker의 동시 접근을 조정한다.
 */

#include "sql.h"
#include "thread_pool.h"
#include "resp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define DEFAULT_PORT 6381
#define DEFAULT_THREADS 4
#define MAX_THREADS 64
/* accept는 했지만 아직 worker가 맡지 못한 접속을 쌓아 둘 수 있는 개수. */
#define PENDING_CONNECTIONS 256
/* 요청 하나의 최대 크기(byte). 이보다 큰 요청은 거절하고 접속을 끊는다. */
#define REQUEST_CAPACITY (64u * 1024u)
/* 접속한 뒤 이 시간(초) 동안 아무것도 보내지 않으면 끊는다. 한가한 접속이 worker를 붙잡지 못하게 한다. */
#define IDLE_TIMEOUT_SECONDS 5

/* SIGINT · SIGTERM을 받으면 1이 된다. */
static volatile sig_atomic_t stop_requested = 0;

static void handle_stop_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

/* worker에게 넘기는 작업 한 건: 어떤 접속을 어떤 엔진으로 처리할지. */
typedef struct ClientJob {
    int fd;
    SqlEngine *engine;
} ClientJob;

/* length byte를 모두 보낼 때까지 send를 반복한다. 성공 0, 실패 -1. */
static int send_all(int fd, const char *bytes, size_t length)
{
    while (length > 0) {
        ssize_t sent = send(fd, bytes, length, 0);

        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        bytes += sent;
        length -= (size_t)sent;
    }
    return 0;
}

/* 실패 응답: "-ERR 이유\r\n". RESP 오류는 한 줄이어야 하므로 줄바꿈을 공백으로 바꾼다. */
static int send_error(int fd, const char *message)
{
    char line[512];
    size_t length;
    size_t i;

    if (strncmp(message, "ERROR: ", 7) == 0) {
        message += 7;
    }
    snprintf(line, sizeof(line) - 2, "-ERR %s", message);
    length = strlen(line);
    while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
        --length;
    }
    for (i = 0; i < length; ++i) {
        if (line[i] == '\n' || line[i] == '\r') {
            line[i] = ' ';
        }
    }
    memcpy(line + length, "\r\n", 2);
    return send_all(fd, line, length + 2);
}

/* 성공 응답: "$길이\r\n내용\r\n" (bulk string). */
static int send_bulk(int fd, const char *text, size_t length)
{
    char header[32];
    int header_length = snprintf(header, sizeof(header), "$%lu\r\n", (unsigned long)length);

    if (header_length < 0 || send_all(fd, header, (size_t)header_length) != 0 ||
        send_all(fd, text, length) != 0) {
        return -1;
    }
    return send_all(fd, "\r\n", 2);
}

/* 명령 하나를 실행하고 응답을 보낸다. 접속을 계속 쓸 수 있으면 0, 끊어야 하면 -1. */
static int handle_command(int fd, SqlEngine *engine, const RespCommand *command)
{
    if (command->argc == 1 && strcasecmp(command->argv[0], "PING") == 0) {
        return send_all(fd, "+PONG\r\n", 7);
    }
    if (command->argc == 2 && strcasecmp(command->argv[0], "SQL") == 0) {
        SqlBuffer out;
        int result;

        sql_buffer_init(&out);
        if (sql_engine_execute(engine, command->argv[1], &out) != 0) {
            result = send_error(fd, out.data != NULL ? out.data : "execution failed");
        } else {
            result = send_bulk(fd, out.data != NULL ? out.data : "", out.length);
        }
        sql_buffer_free(&out);
        return result;
    }
    return send_error(fd, "unknown command (use PING or SQL \"<statement>\")");
}

/* worker thread가 실행하는 함수: 접속 하나가 끊길 때까지 요청을 읽고 답한다. */
static void serve_client(void *argument)
{
    ClientJob *job = (ClientJob *)argument;
    char *input = (char *)malloc(REQUEST_CAPACITY);
    size_t input_length = 0;
    struct timeval timeout;
    int is_open = input != NULL;

    timeout.tv_sec = IDLE_TIMEOUT_SECONDS;
    timeout.tv_usec = 0;
    (void)setsockopt(job->fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    while (is_open && !stop_requested) {
        ssize_t received = recv(job->fd, input + input_length,
                                REQUEST_CAPACITY - input_length, 0);

        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            break;   /* 상대가 끊었거나, 한가한 시간이 넘었거나, 오류가 났다. */
        }
        input_length += (size_t)received;

        /* buffer에 완성된 요청이 여러 개 있을 수 있으므로 남지 않을 때까지 처리한다. */
        while (is_open) {
            RespCommand command = {0, NULL};
            size_t consumed = 0;
            RespParseStatus status = resp_parse_command(input, input_length,
                                                        &consumed, &command);

            if (status == RESP_PARSE_INCOMPLETE) {
                if (input_length == REQUEST_CAPACITY) {
                    (void)send_error(job->fd, "request exceeds input limit");
                    is_open = 0;
                }
                break;
            }
            if (status == RESP_PARSE_ERROR) {
                (void)send_error(job->fd, "malformed RESP request");
                is_open = 0;
                break;
            }
            if (handle_command(job->fd, job->engine, &command) != 0) {
                is_open = 0;
            }
            resp_command_free(&command);
            memmove(input, input + consumed, input_length - consumed);
            input_length -= consumed;
        }
    }

    free(input);
    (void)close(job->fd);
    free(job);
}

/* 숫자 인자를 읽는다. minimum ~ maximum 범위 밖이거나 숫자가 아니면 -1. */
static long parse_number(const char *text, long minimum, long maximum)
{
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < minimum || value > maximum) {
        return -1;
    }
    return value;
}

int main(int argc, char **argv)
{
    struct sigaction stop_action;
    struct sockaddr_in address;
    SqlEngine *engine;
    ThreadPool *pool;
    long port = DEFAULT_PORT;
    long thread_count = DEFAULT_THREADS;
    int listener;
    int reuse = 1;

    if (argc < 2 || argc > 4 ||
        (argc >= 3 && (port = parse_number(argv[2], 1, 65535)) < 0) ||
        (argc == 4 && (thread_count = parse_number(argv[3], 1, MAX_THREADS)) < 0)) {
        fprintf(stderr, "Usage: %s <data_dir> [port=%d] [threads=%d]\n", argv[0],
                DEFAULT_PORT, DEFAULT_THREADS);
        return 2;
    }

    /* 상대가 먼저 끊은 접속에 send해도 서버 전체가 죽지 않게 한다. */
    (void)signal(SIGPIPE, SIG_IGN);
    /* SA_RESTART 없이 등록해서, 신호가 오면 accept가 EINTR로 깨어나게 한다. */
    memset(&stop_action, 0, sizeof(stop_action));
    stop_action.sa_handler = handle_stop_signal;
    sigemptyset(&stop_action.sa_mask);
    (void)sigaction(SIGINT, &stop_action, NULL);
    (void)sigaction(SIGTERM, &stop_action, NULL);

    engine = sql_engine_open(argv[1]);
    if (engine == NULL) {
        fprintf(stderr, "could not open data directory '%s'\n", argv[1]);
        return 1;
    }
    /* 서버는 INSERT에 "성공"이라고 답하기 전에 레코드를 파일로 내려보낸다. */
    sql_engine_set_flush_each_insert(engine, 1);

    pool = thread_pool_create((size_t)thread_count, PENDING_CONNECTIONS);
    if (pool == NULL) {
        fputs("could not create the thread pool\n", stderr);
        sql_engine_close(engine);
        return 1;
    }

    listener = socket(AF_INET, SOCK_STREAM, 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((unsigned short)port);
    if (listener < 0 ||
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0 ||
        bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, SOMAXCONN) != 0) {
        perror("listen");
        if (listener >= 0) {
            (void)close(listener);
        }
        thread_pool_destroy(pool);
        sql_engine_close(engine);
        return 1;
    }
    printf("Mini DBMS SQL server listening on port %ld with %ld worker threads.\n",
           port, thread_count);
    fflush(stdout);

    while (!stop_requested) {
        ClientJob *job;
        int client_fd = accept(listener, NULL, NULL);

        if (client_fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            perror("accept");
            break;
        }
        job = (ClientJob *)malloc(sizeof(ClientJob));
        if (job == NULL) {
            (void)close(client_fd);
            continue;
        }
        job->fd = client_fd;
        job->engine = engine;
        /* 접속을 worker에게 넘긴다. 대기열이 꽉 차 있으면 빈칸이 날 때까지 여기서 기다린다. */
        if (thread_pool_submit(pool, serve_client, job) != 0) {
            (void)close(client_fd);
            free(job);
        }
    }

    (void)close(listener);
    /* 진행 중인 요청을 끝낸 뒤 worker를 정리하고, 남은 레코드를 파일에 쓴다. */
    thread_pool_destroy(pool);
    sql_engine_close(engine);
    puts("Mini DBMS SQL server stopped.");
    return 0;
}
