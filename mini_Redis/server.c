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
#include <unistd.h>

#define CLIENT_INPUT_CAPACITY (64u * 1024u)
#define CLIENT_OUTPUT_CAPACITY (1024u * 1024u)

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

static int dispatch_command(HashTable *table, Client *client,
                            const RespCommand *command)
{
    const char *value;
    int result;

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
        if (command->argc != 3) {
            return append_error(client, "wrong number of arguments for 'set'");
        }
        if (ht_set(table, command->argv[1], command->argv[2]) != 0) {
            return append_error(client, "out of memory");
        }
        return append_bytes(client, "+OK\r\n", 5);
    }

    if (strcasecmp(command->argv[0], "GET") == 0) {
        if (command->argc != 2) {
            return append_error(client, "wrong number of arguments for 'get'");
        }
        value = ht_get(table, command->argv[1]);
        if (value == NULL) {
            return append_bytes(client, "$-1\r\n", 5);
        }
        return append_bulk(client, value);
    }

    if (strcasecmp(command->argv[0], "DEL") == 0) {
        if (command->argc != 2) {
            return append_error(client, "wrong number of arguments for 'del'");
        }
        result = ht_delete(table, command->argv[1]);
        if (result < 0) {
            return append_error(client, "invalid delete request");
        }
        return append_bytes(client, result == 1 ? ":1\r\n" : ":0\r\n", 4);
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
    int listener;
    int reuse = 1;
    struct sockaddr_in address;
    Client *clients = NULL;

    if (table == NULL) {
        errno = EINVAL;
        return -1;
    }
    /* A peer closing during send must not terminate the whole server. */
    (void)signal(SIGPIPE, SIG_IGN);
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

    for (;;) {
        fd_set read_fds;
        fd_set write_fds;
        int max_fd = listener;
        int ready;
        Client *client;

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

        ready = select(max_fd + 1, &read_fds, &write_fds, NULL, NULL);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
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
    return -1;
}
