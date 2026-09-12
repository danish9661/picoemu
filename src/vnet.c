/*
 * Bramble Virtual Network Bus
 *
 * Central frame routing layer. Device models register as ports, and all
 * Ethernet frames are routed between ports, the TAP interface, and peer
 * Bramble instances.
 *
 * Peer connections use a simple length-prefixed framing over Unix domain
 * sockets: [4-byte LE length][Ethernet frame]. This enables Ethernet-level
 * bridging between multiple emulator instances.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "vnet.h"
#include "tapif.h"

vnet_state_t vnet;

/* WS gateway uplink mirror (set by bramble_wasm.c, NULL = disabled). */
vnet_mirror_fn vnet_ws_mirror = NULL;

/* ========================================================================
 * Utility
 * ======================================================================== */

static void set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void vnet_generate_mac(uint8_t *mac, int index) {
    /* Locally administered, unicast: bit 1 of first byte set, bit 0 clear */
    mac[0] = 0x02;
    mac[1] = 0xBB;  /* "Bramble" */
    mac[2] = 0x00;
    mac[3] = 0x00;
    mac[4] = 0x00;
    mac[5] = (uint8_t)(index & 0xFF);
}

static int is_broadcast(const uint8_t *mac) {
    return mac[0] == 0xFF && mac[1] == 0xFF && mac[2] == 0xFF &&
           mac[3] == 0xFF && mac[4] == 0xFF && mac[5] == 0xFF;
}

static int mac_match(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, VNET_MAC_LEN) == 0;
}

/* ========================================================================
 * Lifecycle
 * ======================================================================== */

int vnet_init(void) {
    /* Peers added during flag parsing (vnet_add_peer connects eagerly)
     * must survive this memset — otherwise native -net-peer links are
     * silently dead (peer_count wiped, live fds leaked). */
    vnet_peer_t saved_peers[VNET_MAX_PEERS];
    int saved_peer_count = 0;
    if (vnet.peer_count > 0 && vnet.peer_count <= VNET_MAX_PEERS) {
        saved_peer_count = vnet.peer_count;
        memcpy(saved_peers, vnet.peers, sizeof(saved_peers));
    }
    memset(&vnet, 0, sizeof(vnet));
    if (saved_peer_count > 0) {
        vnet.peer_count = saved_peer_count;
        memcpy(vnet.peers, saved_peers, sizeof(saved_peers));
    }
    vnet.tap_fd = -1;
    for (int i = 0; i < VNET_MAX_PEERS; i++) {
        vnet.peers[i].fd = -1;
        vnet.peers[i].listen_fd = -1;
    }
    /* Gateway MAC: 02:BB:00:00:00:01 */
    vnet_generate_mac(vnet.gateway_mac, 1);
    vnet.enabled = 1;
    return 0;
}

void vnet_cleanup(void) {
    /* Close TAP */
    if (vnet.tap_fd >= 0) {
        tapif_close(vnet.tap_fd);
        vnet.tap_fd = -1;
    }

    /* Close peers */
    for (int i = 0; i < vnet.peer_count; i++) {
        vnet_peer_t *p = &vnet.peers[i];
        if (p->fd >= 0) {
            close(p->fd);
            p->fd = -1;
        }
        if (p->listen_fd >= 0) {
            close(p->listen_fd);
            /* Only unlink files we created: another live process may be
             * bound to (or about to use) a same-path socket. */
            if (p->owns_file)
                unlink(p->path);
            p->listen_fd = -1;
            p->owns_file = 0;
        }
    }

    if (vnet.frames_tx + vnet.frames_rx > 0) {
        vnet_report_stats();
    }

    vnet.enabled = 0;
}

/* ========================================================================
 * TAP Bridge
 * ======================================================================== */

int vnet_attach_tap(const char *name) {
    if (!name || !name[0]) name = VNET_TAP_NAME;

    int fd = tapif_open(name);
    if (fd < 0) return -1;

    vnet.tap_fd = fd;
    strncpy(vnet.tap_name, name, sizeof(vnet.tap_name) - 1);
    vnet.tap_name[sizeof(vnet.tap_name) - 1] = '\0';
    fprintf(stderr, "[VNet] TAP '%s' attached (fd=%d)\n", vnet.tap_name, fd);
    return 0;
}

/* ========================================================================
 * Port Registration
 * ======================================================================== */

int vnet_register_port(const char *name, vnet_port_type_t type,
                       const uint8_t *mac, vnet_rx_fn rx_fn, void *ctx) {
    if (vnet.port_count >= VNET_MAX_PORTS) {
        fprintf(stderr, "[VNet] Maximum ports (%d) reached\n", VNET_MAX_PORTS);
        return -1;
    }

    int idx = vnet.port_count++;
    vnet_port_t *port = &vnet.ports[idx];
    port->active = 1;
    port->type = type;
    strncpy(port->name, name ? name : "unknown", sizeof(port->name) - 1);
    port->name[sizeof(port->name) - 1] = '\0';
    port->rx_fn = rx_fn;
    port->ctx = ctx;

    if (mac) {
        memcpy(port->mac, mac, VNET_MAC_LEN);
    } else {
        /* Auto-assign a locally-administered MAC */
        vnet_generate_mac(port->mac, idx + 10);
    }

    fprintf(stderr, "[VNet] Port %d registered: %s (MAC=%02X:%02X:%02X:%02X:%02X:%02X)\n",
            idx, port->name,
            port->mac[0], port->mac[1], port->mac[2],
            port->mac[3], port->mac[4], port->mac[5]);
    return idx;
}

void vnet_unregister_port(int port_idx) {
    if (port_idx < 0 || port_idx >= vnet.port_count) return;
    /* L11: compact the array so slots are reusable */
    for (int i = port_idx; i + 1 < vnet.port_count; i++)
        vnet.ports[i] = vnet.ports[i + 1];
    vnet.port_count--;
    memset(&vnet.ports[vnet.port_count], 0, sizeof(vnet.ports[0]));
}

/* ========================================================================
 * Frame Transmission
 * ======================================================================== */

/* Forward a frame to the TAP interface */
static void vnet_tap_tx(const uint8_t *frame, int len) {
    if (vnet.tap_fd >= 0 && len > 0) {
        tapif_write(vnet.tap_fd, frame, len);
    }
}

/* Write one length-prefixed frame to a connected peer fd.
 * Returns 1 if fully written, 0 otherwise (EAGAIN: retry later). */
static int vnet_peer_write(int fd, const uint8_t *frame, int len) {
    uint32_t le_len = (uint32_t)len;
    uint8_t buf[4 + VNET_MAX_FRAME];
    buf[0] = (uint8_t)((le_len >>  0) & 0xFF);
    buf[1] = (uint8_t)((le_len >>  8) & 0xFF);
    buf[2] = (uint8_t)((le_len >> 16) & 0xFF);
    buf[3] = (uint8_t)((le_len >> 24) & 0xFF);
    memcpy(buf + 4, frame, (size_t)len);
    size_t total = (size_t)4 + (size_t)len;
    size_t off = 0;
    while (off < total) {
        ssize_t n = write(fd, buf + off, total - off);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        return -1; /* hard error (caller disconnects) */
    }
    return 1;
}

/* Flush pre-accept backlog in order. Stops (keeping the rest) on EAGAIN. */
static void vnet_peer_flush(vnet_peer_t *p) {
    while (p->pend_count > 0) {
        int rc = vnet_peer_write(p->fd, p->pend[0], p->pend_len[0]);
        if (rc == 0) return; /* not ready; keep backlog */
        if (rc < 0) {
            fprintf(stderr, "[VNet] Peer %s: write error, disconnecting\n", p->path);
            close(p->fd);
            p->fd = -1;
            return;
        }
        vnet.frames_peer_tx++;
        memmove(p->pend[0], p->pend[1], (size_t)(p->pend_count - 1) * VNET_MAX_FRAME);
        memmove(p->pend_len, p->pend_len + 1, (size_t)(p->pend_count - 1) * sizeof(p->pend_len[0]));
        p->pend_count--;
    }
}

/* Forward a frame to all connected peers (C9: single-buffer atomic write) */
static void vnet_peers_tx(const uint8_t *frame, int len) {
    for (int i = 0; i < vnet.peer_count; i++) {
        vnet_peer_t *p = &vnet.peers[i];
        /* Opportunistic accept: the periodic vnet_poll may lag (e.g. host
         * blocked in WFE sleep) while a peer already waits in the listen
         * backlog. Accept now so this frame is delivered, not dropped. */
        if (p->fd < 0 && p->listen_fd >= 0) {
            int cfd = accept(p->listen_fd, NULL, NULL);
            if (cfd >= 0) {
                set_nonblock(cfd);
                p->fd = cfd;
                p->rx_len = 0;
                fprintf(stderr, "[VNet] Peer %s: connected (via TX)\n", p->path);
                vnet_peer_flush(p);
            }
        }
        if (p->fd < 0) {
            /* No peer yet: stash for post-accept flush (drop-new if full). */
            if (p->pend_count < 16) {
                memcpy(p->pend[p->pend_count], frame, (size_t)len);
                p->pend_len[p->pend_count] = (uint16_t)len;
                p->pend_count++;
            }
            continue;
        }

        int rc = vnet_peer_write(p->fd, frame, len);
        if (rc < 0) {
            fprintf(stderr, "[VNet] Peer %s: write error, disconnecting\n", p->path);
            close(p->fd);
            p->fd = -1;
        } else if (rc > 0) {
            vnet.frames_peer_tx++;
        }
        /* rc == 0 (EAGAIN): frame dropped; backlog only covers pre-accept. */
    }
}

void vnet_tx_frame(int src_port, const uint8_t *frame, int len) {
    if (len < 14) return;  /* Minimum Ethernet header */
    if (len > VNET_MAX_FRAME) return;

    vnet.frames_tx++;

    const uint8_t *dst_mac = frame;

    /* Deliver to registered ports (except the sender) */
    for (int i = 0; i < vnet.port_count; i++) {
        if (i == src_port) continue;
        vnet_port_t *port = &vnet.ports[i];
        if (!port->active || !port->rx_fn) continue;

        /* Deliver if broadcast/multicast or MAC matches */
        if (is_broadcast(dst_mac) || (dst_mac[0] & 0x01) ||
            mac_match(dst_mac, port->mac)) {
            port->rx_fn(port->ctx, frame, len);
        }
    }

    /* Forward to TAP. M11: all vnet_tx_frame callers (wire links, WASM
     * ETH inject) are non-TAP sources — TAP-originated frames are
     * delivered directly by vnet_poll_tap and never pass through here,
     * so unconditional forward cannot loop back. */
    vnet_tap_tx(frame, len);

    /* WS gateway uplink mirror (WASM browser -> Go gateway). */
    if (vnet_ws_mirror) vnet_ws_mirror(frame, len);

    /* Forward to peers */
    vnet_peers_tx(frame, len);
}

/* ========================================================================
 * Peer Mesh
 * ======================================================================== */

static int peer_try_connect(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    set_nonblock(fd);
    return fd;
}

int vnet_add_peer(const char *path) {
    if (vnet.peer_count >= VNET_MAX_PEERS) {
        fprintf(stderr, "[VNet] Maximum peers (%d) reached\n", VNET_MAX_PEERS);
        return -1;
    }

    /* Record only — sockets are established in vnet_poll. Doing socket
     * work here races with vnet_init (which memsets this array) and with
     * the peer's own startup (connect-first-else-listen on one path can
     * self-connect or fight over the socket file). */
    vnet_peer_t *p = &vnet.peers[vnet.peer_count++];
    p->fd = -1;
    p->listen_fd = -1;
    p->owns_file = 0;
    p->rx_len = 0;
    strncpy(p->path, path, sizeof(p->path) - 1);
    p->path[sizeof(p->path) - 1] = '\0';
    return 0;
}

/* Establish the peer link (called from poll until connected). First
 * starter listens, later starter connects; EADDRINUSE on bind means the
 * peer won the listen race, so fall back to connect. Never connects
 * while holding a listener (avoids self-connect). */
static void vnet_peer_maintain(vnet_peer_t *p) {
    if (p->fd >= 0) return;
    if (p->listen_fd < 0) {
        int fd = peer_try_connect(p->path);
        if (fd >= 0) {
            p->fd = fd;
            p->rx_len = 0;
            fprintf(stderr, "[VNet] Peer %s: connected\n", p->path);
            vnet_peer_flush(p);
            return;
        }
        /* Nobody listening: become the listener. Do NOT unlink first —
         * that would steal a live peer's socket file and split-brain.
         * Bind directly; on EADDRINUSE retry connect (race or live peer),
         * and only unlink-then-bind as a last resort (stale file). */
        int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (lfd >= 0) {
            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, p->path, sizeof(addr.sun_path) - 1);
            if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
                listen(lfd, 1) == 0) {
                set_nonblock(lfd);
                p->listen_fd = lfd;
                p->owns_file = 1;
                fprintf(stderr, "[VNet] Peer %s: waiting for connection\n",
                        p->path);
                return;
            }
            close(lfd);
            if (errno == EADDRINUSE) {
                fd = peer_try_connect(p->path);
                if (fd >= 0) {
                    p->fd = fd;
                    p->rx_len = 0;
                    fprintf(stderr, "[VNet] Peer %s: connected\n", p->path);
                    vnet_peer_flush(p);
                    return;
                }
                /* Live file but nobody accepts: stale socket, reclaim it. */
                unlink(p->path);
            }
        }
    }
}

/* ========================================================================
 * Polling
 * ======================================================================== */

/* Process received peer data: extract length-prefixed frames */
static void peer_process_rx(vnet_peer_t *p) {
    while (p->rx_len >= 4) {
        /* Read 4-byte LE length */
        uint32_t frame_len = (uint32_t)p->rx_buf[0] |
                             ((uint32_t)p->rx_buf[1] << 8) |
                             ((uint32_t)p->rx_buf[2] << 16) |
                             ((uint32_t)p->rx_buf[3] << 24);
        if (frame_len > VNET_MAX_FRAME) {
            fprintf(stderr, "[VNet] Peer %s: oversized frame (%u bytes)\n",
                    p->path, frame_len);
            p->rx_len = 0;
            return;
        }

        if ((uint32_t)p->rx_len < 4 + frame_len) {
            return;  /* Incomplete frame, wait for more data */
        }

        /* L13: drop runt frames (minimum Ethernet header is 14 bytes) */
        if (frame_len < 14) {
            size_t total = 4 + frame_len;
            if ((size_t)p->rx_len > total)
                memmove(p->rx_buf, p->rx_buf + total, (size_t)p->rx_len - total);
            p->rx_len -= (int)total;
            continue;
        }

        /* Deliver frame to all ports (src_port = -1 means "from peer") */
        vnet.frames_peer_rx++;
        vnet.frames_rx++;

        /* Deliver to registered ports */
        const uint8_t *frame = p->rx_buf + 4;
        const uint8_t *dst_mac = frame;
        for (int i = 0; i < vnet.port_count; i++) {
            vnet_port_t *port = &vnet.ports[i];
            if (!port->active || !port->rx_fn) continue;
            if (is_broadcast(dst_mac) || (dst_mac[0] & 0x01) ||
                mac_match(dst_mac, port->mac)) {
                port->rx_fn(port->ctx, frame, (int)frame_len);
            }
        }

        /* Also forward to TAP */
        vnet_tap_tx(frame, (int)frame_len);

        /* Consume this frame from the buffer */
        size_t total = 4 + frame_len;
        if ((size_t)p->rx_len > total) {
            memmove(p->rx_buf, p->rx_buf + total, (size_t)p->rx_len - total);
        }
        p->rx_len -= (int)total;
    }
}

static void vnet_poll_peers(void) {
    for (int i = 0; i < vnet.peer_count; i++) {
        vnet_peer_t *p = &vnet.peers[i];

        /* Establish the link (first starter listens, later connects). */
        if (p->fd < 0)
            vnet_peer_maintain(p);

        /* Accept pending connections */
        if (p->listen_fd >= 0 && p->fd < 0) {
            int cfd = accept(p->listen_fd, NULL, NULL);
            if (cfd >= 0) {
                set_nonblock(cfd);
                p->fd = cfd;
                p->rx_len = 0;
                fprintf(stderr, "[VNet] Peer %s: connected\n", p->path);
                vnet_peer_flush(p);
            }
        }

        /* Read data from connected peer */
        if (p->fd >= 0) {
            struct pollfd pfd = { .fd = p->fd, .events = POLLIN };
            if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
                int space = (int)sizeof(p->rx_buf) - p->rx_len;
                if (space > 0) {
                    ssize_t n = read(p->fd, p->rx_buf + p->rx_len, (size_t)space);
                    if (n > 0) {
                        p->rx_len += (int)n;
                        peer_process_rx(p);
                    } else if (n == 0) {
                        fprintf(stderr, "[VNet] Peer %s: disconnected\n", p->path);
                        close(p->fd);
                        p->fd = -1;
                        p->rx_len = 0;
                    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        fprintf(stderr, "[VNet] Peer %s: read error: %s\n",
                                p->path, strerror(errno));
                        close(p->fd);
                        p->fd = -1;
                        p->rx_len = 0;
                    }
                }
            }

            /* Check for peer errors */
            if (p->fd >= 0) {
                struct pollfd pfd2 = { .fd = p->fd, .events = 0 };
                if (poll(&pfd2, 1, 0) > 0 &&
                    (pfd2.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                    fprintf(stderr, "[VNet] Peer %s: connection error\n", p->path);
                    close(p->fd);
                    p->fd = -1;
                    p->rx_len = 0;
                }
            }
        }

    }
}

static void vnet_poll_tap(void) {
    if (vnet.tap_fd < 0) return;

    /* Read frames from TAP (host → emulated devices) */
    uint8_t frame[VNET_MAX_FRAME];
    int n;
    /* Drain up to 16 frames per poll cycle to avoid starvation */
    for (int burst = 0; burst < 16; burst++) {
        n = tapif_read(vnet.tap_fd, frame, sizeof(frame));
        if (n <= 0) break;
        if (n < 14) continue;  /* Too small for Ethernet header */

        vnet.frames_rx++;

        const uint8_t *dst_mac = frame;

        /* Deliver to all registered ports */
        for (int i = 0; i < vnet.port_count; i++) {
            vnet_port_t *port = &vnet.ports[i];
            if (!port->active || !port->rx_fn) continue;
            if (is_broadcast(dst_mac) || (dst_mac[0] & 0x01) ||
                mac_match(dst_mac, port->mac)) {
                port->rx_fn(port->ctx, frame, n);
            }
        }

        /* Forward to peers */
        vnet_peers_tx(frame, n);
    }
}

void vnet_poll(void) {
    if (!vnet.enabled) return;
    vnet_poll_tap();
    vnet_poll_peers();
}

/* ========================================================================
 * Statistics
 * ======================================================================== */

void vnet_report_stats(void) {
    fprintf(stderr, "[VNet] Frames: TX=%u RX=%u Peer-TX=%u Peer-RX=%u\n",
            vnet.frames_tx, vnet.frames_rx,
            vnet.frames_peer_tx, vnet.frames_peer_rx);
    fprintf(stderr, "[VNet] Ports: %d, Peers: %d, TAP: %s\n",
            vnet.port_count, vnet.peer_count,
            vnet.tap_fd >= 0 ? vnet.tap_name : "none");
}
