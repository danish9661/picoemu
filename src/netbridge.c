/*
 * Bramble Network Bridge - UART to TCP
 *
 * Bridges UART peripherals to TCP sockets for network communication.
 * Supports listen (server) and connect (client) modes.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "netbridge.h"
#include "uart.h"

net_bridge_t net_bridge;

/* Set a file descriptor to non-blocking mode */
static void set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

/* Disable Nagle's algorithm for low-latency byte-at-a-time transfer */
static void set_nodelay(int fd) {
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
}

/* Create a listening TCP socket on the given port */
static int create_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }

    set_nonblock(fd);
    return fd;
}

/* Connect to a remote host:port */
static int create_connect_socket(const char *host, int port) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);
    set_nonblock(fd);
    set_nodelay(fd);
    return fd;
}

int net_bridge_init(void) {
    for (int i = 0; i < NET_BRIDGE_MAX_UART; i++) {
        net_uart_bridge_t *b = &net_bridge.uart[i];

        if (b->mode == NET_MODE_LISTEN) {
            b->listen_fd = create_listen_socket(b->port);
            if (b->listen_fd < 0) {
                fprintf(stderr, "[Net] Failed to listen on port %d for UART%d: %s\n",
                        b->port, i, strerror(errno));
                return -1;
            }
            b->client_fd = -1;
            fprintf(stderr, "[Net] UART%d listening on port %d\n", i, b->port);
        } else if (b->mode == NET_MODE_CONNECT) {
            b->listen_fd = -1;
            b->client_fd = create_connect_socket(b->host, b->remote_port);
            if (b->client_fd < 0) {
                fprintf(stderr, "[Net] Failed to connect UART%d to %s:%d: %s\n",
                        i, b->host, b->remote_port, strerror(errno));
                return -1;
            }
            fprintf(stderr, "[Net] UART%d connected to %s:%d\n",
                    i, b->host, b->remote_port);
        } else {
            b->listen_fd = -1;
            b->client_fd = -1;
        }
    }
    return 0;
}

void net_bridge_cleanup(void) {
    for (int i = 0; i < NET_BRIDGE_MAX_UART; i++) {
        net_uart_bridge_t *b = &net_bridge.uart[i];
        if (b->client_fd >= 0) {
            close(b->client_fd);
            b->client_fd = -1;
        }
        if (b->listen_fd >= 0) {
            close(b->listen_fd);
            b->listen_fd = -1;
        }
    }
}

static void net_bridge_uart_flush(net_uart_bridge_t *b, int uart_num);

void net_bridge_poll(void) {
    for (int i = 0; i < NET_BRIDGE_MAX_UART; i++) {        net_uart_bridge_t *b = &net_bridge.uart[i];
        if (b->mode == NET_MODE_NONE) continue;

        /* Accept new connections in listen mode */
        if (b->mode == NET_MODE_LISTEN && b->listen_fd >= 0 && b->client_fd < 0) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int cfd = accept(b->listen_fd, (struct sockaddr *)&client_addr, &addr_len);
            if (cfd >= 0) {
                set_nonblock(cfd);
                set_nodelay(cfd);
                b->client_fd = cfd;
                fprintf(stderr, "[Net] UART%d client connected from %s:%d\n",
                        i, inet_ntoa(client_addr.sin_addr),
                        ntohs(client_addr.sin_port));
            }
        }

        /* Read data from connected socket → push into UART RX FIFO */
        if (b->client_fd >= 0) {
            struct pollfd pfd = { .fd = b->client_fd, .events = POLLIN };
            if (poll(&pfd, 1, 0) > 0) {
                /* M14: hangup/error means the peer is gone */
                if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    fprintf(stderr, "[Net] UART%d peer hangup, disconnecting\n", i);
                    close(b->client_fd);
                    b->client_fd = -1;
                    b->tx_len = 0;
                } else if (pfd.revents & POLLIN) {
                uint8_t buf[64];
                ssize_t n = read(b->client_fd, buf, sizeof(buf));
                if (n > 0) {
                    for (ssize_t j = 0; j < n; j++) {
                        uart_rx_push(i, buf[j]);
                    }
                } else if (n == 0) {
                    /* Client disconnected */
                    fprintf(stderr, "[Net] UART%d client disconnected\n", i);
                    close(b->client_fd);
                    b->client_fd = -1;
                    b->tx_len = 0;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    /* M13: real read errors disconnect (don't spin on them) */
                    fprintf(stderr, "[Net] UART%d read error (%s), disconnecting\n",
                            i, strerror(errno));
                    close(b->client_fd);
                    b->client_fd = -1;
                    b->tx_len = 0;
                }
                }
            }
        }

        /* H12: flush batched TX once per poll (not per byte) */
        net_bridge_uart_flush(b, i);
    }
}

/* Flush one UART's batched TX buffer (H12: single write() per flush). */
static void net_bridge_uart_flush(net_uart_bridge_t *b, int uart_num) {
    if (b->client_fd < 0 || b->tx_len <= 0) { b->tx_len = 0; return; }
    ssize_t n = write(b->client_fd, b->tx_buf, (size_t)b->tx_len);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return; /* retry next poll */
    if (n < 0) {
        fprintf(stderr, "[Net] UART%d write error, disconnecting\n", uart_num);
        close(b->client_fd);
        b->client_fd = -1;
        b->tx_len = 0;
        return;
    }
    if ((size_t)n < (size_t)b->tx_len)
        memmove(b->tx_buf, b->tx_buf + n, (size_t)b->tx_len - (size_t)n);
    b->tx_len -= (int)n;
}

void net_bridge_uart_tx(int uart_num, uint8_t byte) {
    if (uart_num < 0 || uart_num >= NET_BRIDGE_MAX_UART) return;
    net_uart_bridge_t *b = &net_bridge.uart[uart_num];

    if (b->client_fd >= 0) {
        if (b->tx_len >= (int)sizeof(b->tx_buf))
            net_bridge_uart_flush(b, uart_num);
        if (b->client_fd >= 0 && b->tx_len < (int)sizeof(b->tx_buf))
            b->tx_buf[b->tx_len++] = byte;
    }
}

int net_bridge_uart_active(int uart_num) {
    if (uart_num < 0 || uart_num >= NET_BRIDGE_MAX_UART) return 0;
    return net_bridge.uart[uart_num].mode != NET_MODE_NONE;
}
