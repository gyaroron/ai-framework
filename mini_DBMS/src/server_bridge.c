#define _POSIX_C_SOURCE 200809L

#include "server_bridge.h"

#include "resp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define BRIDGE_PORT 6380
#define CLIENT_BUFFER_SIZE 8192u
#define BRIDGE_MAX_CLIENTS 128u
#define DB_KEY_SERVER_IP UINT64_C(1)

typedef struct Client {
    int fd;
    char input[CLIENT_BUFFER_SIZE];
    size_t input_length;
    char output[128];
    size_t output_length;
    size_t output_offset;
} Client;

static volatile sig_atomic_t bridge_stopping;

static void handle_stop_signal(int signal_number)
{
    (void)signal_number;
    bridge_stopping = 1;
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }
    return 0;
}

static int is_server_ip_key(const char *key)
{
    return strcmp(key, "server:ip") == 0;
}

static int parse_ipv4_value(const char *text, uint64_t *encoded)
{
    struct in_addr address;
    uint32_t host_value;

    if (inet_pton(AF_INET, text, &address) != 1) {
        return -1;
    }
    host_value = ntohl(address.s_addr);
    *encoded = (uint64_t)host_value + UINT64_C(1);
    return 0;
}

static int commit_ip_value(TransactionManager *transactions, uint64_t value)
{
    uint64_t txn_id = txn_begin(transactions);
    if (txn_id == 0) {
        return -1;
    }
    if (txn_insert(transactions, txn_id, DB_KEY_SERVER_IP, value) != 0) {
        (void)txn_abort(transactions, txn_id);
        return -1;
    }
    if (txn_commit(transactions, txn_id) != 0) {
        (void)txn_abort(transactions, txn_id);
        return -1;
    }
    return 0;
}

static int format_response(const RespCommand *command,
                           TransactionManager *transactions,
                           BPlusTree *tree, char *output, size_t capacity,
                           size_t *output_length)
{
    uint64_t value;
    int written;

    if (command->argc == 1 && strcmp(command->argv[0], "PING") == 0) {
        written = snprintf(output, capacity, "+PONG\r\n");
    } else if (command->argc == 2 && strcmp(command->argv[0], "GET") == 0) {
        struct in_addr address;
        if (!is_server_ip_key(command->argv[1])) {
            written = snprintf(output, capacity,
                               "-ERR unsupported DBMS key\r\n");
        } else if (bplus_tree_find(tree, DB_KEY_SERVER_IP, &value) != 0 ||
                   value == 0) {
            written = snprintf(output, capacity, "$-1\r\n");
        } else {
            address.s_addr = htonl((uint32_t)(value - UINT64_C(1)));
            {
                char ip_text[INET_ADDRSTRLEN];
                if (inet_ntop(AF_INET, &address, ip_text,
                              sizeof(ip_text)) == NULL) {
                    return -1;
                }
                written = snprintf(output, capacity, "$%lu\r\n%s\r\n",
                                   (unsigned long)strlen(ip_text), ip_text);
            }
        }
    } else if (command->argc == 3 &&
               strcmp(command->argv[0], "SET") == 0) {
        if (!is_server_ip_key(command->argv[1])) {
            written = snprintf(output, capacity,
                               "-ERR unsupported DBMS key\r\n");
        } else if (parse_ipv4_value(command->argv[2], &value) != 0) {
            written = snprintf(output, capacity,
                               "-ERR value must be an IPv4 address\r\n");
        } else if (commit_ip_value(transactions, value) != 0) {
            written = snprintf(output, capacity, "-ERR DBMS write failed\r\n");
        } else {
            written = snprintf(output, capacity, "+OK\r\n");
        }
    } else if (command->argc == 2 &&
               strcmp(command->argv[0], "DEL") == 0) {
        if (!is_server_ip_key(command->argv[1])) {
            written = snprintf(output, capacity,
                               "-ERR unsupported DBMS key\r\n");
        } else {
            int existed = bplus_tree_find(tree, DB_KEY_SERVER_IP, &value) == 0 &&
                          value != 0;
            if (commit_ip_value(transactions, 0) != 0) {
                written = snprintf(output, capacity,
                                   "-ERR DBMS write failed\r\n");
            } else {
                written = snprintf(output, capacity, ":%d\r\n", existed ? 1 : 0);
            }
        }
    } else {
        written = snprintf(output, capacity, "-ERR unsupported command\r\n");
    }

    if (written < 0 || (size_t)written >= capacity) {
        return -1;
    }
    *output_length = (size_t)written;
    return 0;
}

static void close_client(Client *client)
{
    if (client->fd >= 0) {
        (void)close(client->fd);
        client->fd = -1;
    }
    client->input_length = 0;
    client->output_length = 0;
    client->output_offset = 0;
}

static int parse_buffered_command(Client *client,
                                  TransactionManager *transactions,
                                  BPlusTree *tree)
{
    RespCommand command = {0, NULL};
    size_t consumed = 0;
    RespParseStatus status = resp_parse_command(client->input,
                                                client->input_length,
                                                &consumed, &command);

    if (status == RESP_PARSE_INCOMPLETE) {
        if (client->input_length == sizeof(client->input)) {
            static const char too_large[] = "-ERR request exceeds input limit\r\n";
            memcpy(client->output, too_large, sizeof(too_large) - 1u);
            client->output_length = sizeof(too_large) - 1u;
            client->output_offset = 0;
            client->input_length = 0;
            return 1;
        }
        return 0;
    }
    if (status == RESP_PARSE_ERROR) {
        static const char malformed[] = "-ERR malformed RESP request\r\n";
        memcpy(client->output, malformed, sizeof(malformed) - 1u);
        client->output_length = sizeof(malformed) - 1u;
        client->output_offset = 0;
        client->input_length = 0;
        return 1;
    }
    if (format_response(&command, transactions, tree, client->output,
                        sizeof(client->output), &client->output_length) != 0) {
        resp_command_free(&command);
        return -1;
    }
    resp_command_free(&command);
    memmove(client->input, client->input + consumed,
            client->input_length - consumed);
    client->input_length -= consumed;
    client->output_offset = 0;
    return 1;
}

int server_bridge_run(TransactionManager *transactions, BPlusTree *tree)
{
    struct sigaction action;
    struct sockaddr_in address;
    Client clients[BRIDGE_MAX_CLIENTS];
    int listener;
    size_t i;
    int result = -1;

    if (transactions == NULL || tree == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(clients, 0, sizeof(clients));
    for (i = 0; i < BRIDGE_MAX_CLIENTS; ++i) {
        clients[i].fd = -1;
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_stop_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0) {
        return -1;
    }
    (void)signal(SIGPIPE, SIG_IGN);

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        return -1;
    }
    {
        int reuse = 1;
        if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse,
                       sizeof(reuse)) != 0 || set_nonblocking(listener) != 0) {
            goto cleanup_listener;
        }
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(BRIDGE_PORT);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, 32) != 0) {
        goto cleanup_listener;
    }
    bridge_stopping = 0;
    (void)fprintf(stdout, "Mini DBMS RESP bridge listening on port %d\n",
                  BRIDGE_PORT);
    (void)fflush(stdout);
    result = 0;

    while (!bridge_stopping) {
        fd_set read_set;
        fd_set write_set;
        int max_fd = listener;
        int ready;

        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        FD_SET(listener, &read_set);
        for (i = 0; i < BRIDGE_MAX_CLIENTS; ++i) {
            if (clients[i].fd >= 0) {
                if (clients[i].input_length < sizeof(clients[i].input)) {
                    FD_SET(clients[i].fd, &read_set);
                }
                if (clients[i].output_offset < clients[i].output_length) {
                    FD_SET(clients[i].fd, &write_set);
                }
                if (clients[i].fd > max_fd) {
                    max_fd = clients[i].fd;
                }
            }
        }
        ready = select(max_fd + 1, &read_set, &write_set, NULL, NULL);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            result = -1;
            break;
        }
        if (FD_ISSET(listener, &read_set)) {
            for (;;) {
                int fd = accept(listener, NULL, NULL);
                if (fd < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    result = -1;
                    bridge_stopping = 1;
                    break;
                }
                if (fd >= FD_SETSIZE) {
                    (void)close(fd);
                    continue;
                }
                if (set_nonblocking(fd) != 0) {
                    (void)close(fd);
                    continue;
                }
                for (i = 0; i < BRIDGE_MAX_CLIENTS; ++i) {
                    if (clients[i].fd < 0) {
                        clients[i].fd = fd;
                        break;
                    }
                }
                if (i == BRIDGE_MAX_CLIENTS) {
                    (void)close(fd);
                }
            }
        }
        for (i = 0; i < BRIDGE_MAX_CLIENTS; ++i) {
            Client *client = &clients[i];
            if (client->fd < 0) {
                continue;
            }
            if (FD_ISSET(client->fd, &read_set)) {
                ssize_t amount;
                if (client->input_length == sizeof(client->input)) {
                    close_client(client);
                    continue;
                }
                amount = recv(client->fd, client->input + client->input_length,
                              sizeof(client->input) - client->input_length, 0);
                if (amount <= 0) {
                    if (amount < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                                       errno == EINTR)) {
                        /* Keep the connection and retry on the next readiness event. */
                    } else {
                        close_client(client);
                    }
                    continue;
                }
                client->input_length += (size_t)amount;
                if (client->output_length == 0 &&
                    parse_buffered_command(client, transactions, tree) < 0) {
                    close_client(client);
                }
            }
            if (client->fd >= 0 && FD_ISSET(client->fd, &write_set)) {
                ssize_t amount = send(client->fd,
                                      client->output + client->output_offset,
                                      client->output_length - client->output_offset,
                                      0);
                if (amount < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                                   errno == EINTR)) {
                    continue;
                }
                if (amount <= 0) {
                    close_client(client);
                    continue;
                }
                client->output_offset += (size_t)amount;
                if (client->output_offset == client->output_length) {
                    client->output_length = 0;
                    client->output_offset = 0;
                    if (client->input_length != 0 &&
                        parse_buffered_command(client, transactions, tree) < 0) {
                        close_client(client);
                    }
                }
            }
        }
    }

    for (i = 0; i < BRIDGE_MAX_CLIENTS; ++i) {
        close_client(&clients[i]);
    }
cleanup_listener:
    (void)close(listener);
    return result;
}
