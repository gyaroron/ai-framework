#define _POSIX_C_SOURCE 200809L

#include "server.h"
#include "resp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define CLIENT_INPUT_CAPACITY (64u * 1024u)
#define CLIENT_OUTPUT_CAPACITY (1024u * 1024u)
/* 능동 만료(만료된 key 청소)와 snapshot 저장을 이 간격(밀리초)마다 한 번씩만 한다. */
#define EXPIRY_SWEEP_INTERVAL_MS 1000LL
/* 받을 수 있는 가장 긴 TTL(밀리초, 약 31년). 만료 시각 계산이 넘치지 않게 막는다. */
#define MAX_TTL_MS (1000LL * 1000LL * 1000LL * 1000LL)

typedef struct Client {
    int fd;
    char input[CLIENT_INPUT_CAPACITY];
    size_t input_length;
    char output[CLIENT_OUTPUT_CAPACITY];
    size_t output_length;
    int close_after_write;
    struct Client *next;
} Client;

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void remove_client(Client **clients, Client *client)
{
    Client **link = clients;

    while (*link != NULL && *link != client) {
        link = &(*link)->next;
    }
    if (*link == client) {
        *link = client->next;
        close(client->fd);
        free(client);
    }
}

static int append_bytes(Client *client, const char *bytes, size_t length)
{
    if (length > CLIENT_OUTPUT_CAPACITY - client->output_length) {
        return -1;
    }
    memcpy(client->output + client->output_length, bytes, length);
    client->output_length += length;
    return 0;
}

static int append_bulk(Client *client, const char *value)
{
    char header[64];
    size_t length = strlen(value);
    int header_length = snprintf(header, sizeof(header), "$%lu\r\n",
                                 (unsigned long)length);

    if (header_length < 0 || (size_t)header_length >= sizeof(header) ||
        append_bytes(client, header, (size_t)header_length) != 0 ||
        append_bytes(client, value, length) != 0 ||
        append_bytes(client, "\r\n", 2) != 0) {
        return -1;
    }
    return 0;
}

static int append_error(Client *client, const char *message)
{
    return append_bytes(client, "-ERR ", 5) != 0 ||
           append_bytes(client, message, strlen(message)) != 0 ||
           append_bytes(client, "\r\n", 2) != 0 ? -1 : 0;
}

/* SIGINT · SIGTERM을 받으면 1이 된다. signal handler는 이 변수만 건드린다. */
static volatile sig_atomic_t stop_requested = 0;
/* 마지막 snapshot 이후 key가 바뀌었으면 1. 바뀐 것이 없으면 파일을 다시 쓰지 않는다. */
static int table_dirty = 0;

static void handle_stop_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

/* 벽시계(1970년부터의 밀리초). 재시작해도 이어지는 시각이라 snapshot의 만료 시각에 쓴다. */
static long long wall_now_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        return 0;
    }
    return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000L;
}

/* 벽시계를 사람이 바꿔도 뒤로 가지 않는 시계(monotonic clock)의 밀리초. TTL 계산에 쓴다. */
static long long now_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 1;
    }
    /* +1: 부팅 직후에도 값이 HT_NO_EXPIRY(0)보다 크도록 한다. */
    return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000L + 1;
}

static int append_integer(Client *client, long long number)
{
    char reply[32];
    int length = snprintf(reply, sizeof(reply), ":%lld\r\n", number);

    if (length < 0 || (size_t)length >= sizeof(reply)) {
        return -1;
    }
    return append_bytes(client, reply, (size_t)length);
}

/* 양의 정수 TTL 문자열을 밀리초로 바꾼다. 성공하면 0, 숫자가 아니거나 범위를 벗어나면 -1. */
static int parse_ttl_ms(const char *text, long long unit_ms, long long *ttl_ms)
{
    char *end = NULL;
    long long amount;

    if (text[0] == '\0') {
        return -1;
    }
    errno = 0;
    amount = strtoll(text, &end, 10);
    if (errno != 0 || *end != '\0' || amount <= 0 || amount > MAX_TTL_MS / unit_ms) {
        return -1;
    }
    *ttl_ms = amount * unit_ms;
    return 0;
}

static int dispatch_command(HashTable *table, Client *client,
                            const RespCommand *command)
{
    const char *value;
    int result;
    long long now = now_ms();
    long long ttl_ms;

    if (strcasecmp(command->argv[0], "PING") == 0) {
        if (command->argc == 1) {
            return append_bytes(client, "+PONG\r\n", 7);
        }
        if (command->argc == 2) {
            return append_bulk(client, command->argv[1]);
        }
        return append_error(client, "wrong number of arguments for 'ping'");
    }

    if (strcasecmp(command->argv[0], "SET") == 0) {
        long long expires_at_ms = HT_NO_EXPIRY;

        if (command->argc != 3 && command->argc != 5) {
            return append_error(client, "wrong number of arguments for 'set'");
        }
        if (command->argc == 5) {
            long long unit_ms;

            if (strcasecmp(command->argv[3], "EX") == 0) {
                unit_ms = 1000LL;
            } else if (strcasecmp(command->argv[3], "PX") == 0) {
                unit_ms = 1LL;
            } else {
                return append_error(client, "syntax error");
            }
            if (parse_ttl_ms(command->argv[4], unit_ms, &ttl_ms) != 0) {
                return append_error(client, "invalid expire time in 'set'");
            }
            expires_at_ms = now + ttl_ms;
        }
        if (ht_set_with_expiry(table, command->argv[1], command->argv[2],
                               expires_at_ms) != 0) {
            return append_error(client, "out of memory");
        }
        table_dirty = 1;
        return append_bytes(client, "+OK\r\n", 5);
    }

    if (strcasecmp(command->argv[0], "GET") == 0) {
        if (command->argc != 2) {
            return append_error(client, "wrong number of arguments for 'get'");
        }
        /* 만료된 key는 여기서 지워지고, 없는 key와 똑같이 응답한다. */
        value = ht_get_at(table, command->argv[1], now);
        if (value == NULL) {
            return append_bytes(client, "$-1\r\n", 5);
        }
        return append_bulk(client, value);
    }

    if (strcasecmp(command->argv[0], "DEL") == 0) {
        if (command->argc != 2) {
            return append_error(client, "wrong number of arguments for 'del'");
        }
        /* 이미 만료된 key를 먼저 지워서, 그런 key에 대한 DEL이 0을 답하게 한다. */
        (void)ht_get_at(table, command->argv[1], now);
        result = ht_delete(table, command->argv[1]);
        if (result < 0) {
            return append_error(client, "invalid delete request");
        }
        if (result == 1) {
            table_dirty = 1;
        }
        return append_bytes(client, result == 1 ? ":1\r\n" : ":0\r\n", 4);
    }

    if (strcasecmp(command->argv[0], "EXPIRE") == 0 ||
        strcasecmp(command->argv[0], "PEXPIRE") == 0) {
        int is_seconds = strcasecmp(command->argv[0], "EXPIRE") == 0;

        if (command->argc != 3) {
            return append_error(client, "wrong number of arguments for 'expire'");
        }
        if (parse_ttl_ms(command->argv[2], is_seconds ? 1000LL : 1LL, &ttl_ms) != 0) {
            return append_error(client, "invalid expire time in 'expire'");
        }
        result = ht_expire_at(table, command->argv[1], now + ttl_ms, now);
        if (result < 0) {
            return append_error(client, "invalid expire request");
        }
        if (result == 1) {
            table_dirty = 1;
        }
        return append_integer(client, result);
    }

    if (strcasecmp(command->argv[0], "TTL") == 0 ||
        strcasecmp(command->argv[0], "PTTL") == 0) {
        if (command->argc != 2) {
            return append_error(client, "wrong number of arguments for 'ttl'");
        }
        ttl_ms = ht_ttl_ms(table, command->argv[1], now);
        if (ttl_ms >= 0 && strcasecmp(command->argv[0], "TTL") == 0) {
            /* 올림한다. 아직 살아 있는 key가 0초로 보이지 않게 하기 위해서다. */
            ttl_ms = (ttl_ms + 999LL) / 1000LL;
        }
        return append_integer(client, ttl_ms);
    }

    if (strcasecmp(command->argv[0], "PERSIST") == 0) {
        if (command->argc != 2) {
            return append_error(client, "wrong number of arguments for 'persist'");
        }
        result = ht_persist(table, command->argv[1], now);
        if (result < 0) {
            return append_error(client, "invalid persist request");
        }
        if (result == 1) {
            table_dirty = 1;
        }
        return append_integer(client, result);
    }

    return append_error(client, "unknown command");
}

/* Processes complete buffered requests; leaves partial requests untouched. */
static int process_input(HashTable *table, Client *client)
{
    while (client->input_length > 0) {
        RespCommand command = {0, NULL};
        size_t consumed = 0;
        RespParseStatus status = resp_parse_command(
            client->input, client->input_length, &consumed, &command);

        if (status == RESP_PARSE_INCOMPLETE) {
            return 0;
        }
        if (status == RESP_PARSE_ERROR) {
            if (append_error(client, "Protocol error") != 0) {
                return -1;
            }
            client->close_after_write = 1;
            client->input_length = 0;
            return 0;
        }

        if (dispatch_command(table, client, &command) != 0) {
            resp_command_free(&command);
            return -1;
        }
        resp_command_free(&command);
        memmove(client->input, client->input + consumed,
                client->input_length - consumed);
        client->input_length -= consumed;
    }
    return 0;
}

static int accept_clients(int listener, Client **clients)
{
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        Client *client;

        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (fd >= FD_SETSIZE || set_nonblocking(fd) != 0) {
            close(fd);
            continue;
        }
        client = (Client *)calloc(1, sizeof(*client));
        if (client == NULL) {
            close(fd);
            continue;
        }
        client->fd = fd;
        client->next = *clients;
        *clients = client;
    }
}

int server_run(HashTable *table)
{
    return server_run_with_snapshot(table, NULL);
}

/* key가 바뀌었을 때만 snapshot 파일을 다시 쓴다. 실패하면 다음 간격에 다시 시도한다. */
static void save_snapshot_if_dirty(HashTable *table, const char *snapshot_path)
{
    if (snapshot_path == NULL || !table_dirty) {
        return;
    }
    if (ht_save(table, snapshot_path, now_ms(), wall_now_ms()) >= 0) {
        table_dirty = 0;
    } else {
        perror("snapshot save");
    }
}

int server_run_with_snapshot(HashTable *table, const char *snapshot_path)
{
    struct sigaction stop_action;
    int listener;
    int reuse = 1;
    struct sockaddr_in address;
    Client *clients = NULL;
    long long last_sweep_ms = now_ms();

    if (table == NULL) {
        errno = EINVAL;
        return -1;
    }
    /* A peer closing during send must not terminate the whole server. */
    (void)signal(SIGPIPE, SIG_IGN);
    /* SA_RESTART를 주지 않는다. 그래야 신호가 오면 select가 EINTR로 깨어나 종료 여부를 확인한다. */
    memset(&stop_action, 0, sizeof(stop_action));
    stop_action.sa_handler = handle_stop_signal;
    sigemptyset(&stop_action.sa_mask);
    (void)sigaction(SIGINT, &stop_action, NULL);
    (void)sigaction(SIGTERM, &stop_action, NULL);
    stop_requested = 0;
    table_dirty = 0;

    if (snapshot_path != NULL) {
        long loaded = ht_load(table, snapshot_path, now_ms(), wall_now_ms());
        if (loaded < 0) {
            fprintf(stderr, "snapshot %s is damaged; starting empty\n", snapshot_path);
        } else {
            printf("Loaded %ld keys from %s.\n", loaded, snapshot_path);
            fflush(stdout);
        }
    }
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        return -1;
    }
    if (set_nonblocking(listener) != 0 ||
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        close(listener);
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((unsigned short)SERVER_DEFAULT_PORT);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, SOMAXCONN) != 0) {
        close(listener);
        return -1;
    }

    while (!stop_requested) {
        fd_set read_fds;
        fd_set write_fds;
        int max_fd = listener;
        int ready;
        Client *client;
        struct timeval timeout;
        long long loop_now;

        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        FD_SET(listener, &read_fds);
        for (client = clients; client != NULL; client = client->next) {
            if (client->input_length < CLIENT_INPUT_CAPACITY &&
                !client->close_after_write) {
                FD_SET(client->fd, &read_fds);
            }
            if (client->output_length > 0) {
                FD_SET(client->fd, &write_fds);
            }
            if (client->fd > max_fd) {
                max_fd = client->fd;
            }
        }

        /* 요청이 없어도 간격마다 한 번은 깨어나, 만료된 key를 지우고 snapshot을 쓴다. */
        timeout.tv_sec = (time_t)(EXPIRY_SWEEP_INTERVAL_MS / 1000LL);
        timeout.tv_usec = 0;
        ready = select(max_fd + 1, &read_fds, &write_fds, NULL, &timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        loop_now = now_ms();
        if (loop_now - last_sweep_ms >= EXPIRY_SWEEP_INTERVAL_MS) {
            if (ht_purge_expired(table, loop_now) > 0) {
                table_dirty = 1;
            }
            save_snapshot_if_dirty(table, snapshot_path);
            last_sweep_ms = loop_now;
        }
        if (ready == 0) {
            continue;
        }

        if (FD_ISSET(listener, &read_fds) && accept_clients(listener, &clients) != 0) {
            break;
        }

        client = clients;
        while (client != NULL) {
            Client *next_client = client->next;
            int remove = 0;

            if (FD_ISSET(client->fd, &read_fds)) {
                ssize_t received = recv(client->fd,
                    client->input + client->input_length,
                    CLIENT_INPUT_CAPACITY - client->input_length, 0);
                if (received > 0) {
                    client->input_length += (size_t)received;
                    if (process_input(table, client) != 0) {
                        remove = 1;
                    } else if (client->input_length == CLIENT_INPUT_CAPACITY) {
                        if (append_error(client, "request too large") != 0) {
                            remove = 1;
                        } else {
                            client->input_length = 0;
                            client->close_after_write = 1;
                        }
                    }
                } else if (received == 0 ||
                           (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    remove = 1;
                }
            }

            if (!remove && client->output_length > 0 &&
                FD_ISSET(client->fd, &write_fds)) {
                ssize_t sent = send(client->fd, client->output,
                                    client->output_length, 0);
                if (sent > 0) {
                    client->output_length -= (size_t)sent;
                    memmove(client->output, client->output + sent,
                            client->output_length);
                } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                           errno != EINTR) {
                    remove = 1;
                }
            }

            if (!remove && client->close_after_write && client->output_length == 0) {
                remove = 1;
            }
            if (remove) {
                remove_client(&clients, client);
            }
            client = next_client;
        }
    }

    while (clients != NULL) {
        remove_client(&clients, clients);
    }
    close(listener);
    /* 종료 신호로 끝난 경우: 마지막 변경까지 파일에 남기고 정상 종료(0)로 알린다. */
    if (stop_requested) {
        save_snapshot_if_dirty(table, snapshot_path);
        return 0;
    }
    return -1;
}
