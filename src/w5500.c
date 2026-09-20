/*
 * W5500 Ethernet Controller (SPI Device Plugin)
 *
 * Emulates the WIZnet W5500 hardwired TCP/IP Ethernet controller.
 * Processes SPI frames with a 3-byte header (2 address + 1 control)
 * followed by data bytes for read or write operations.
 *
 * Block Select Byte (BSB) routes access to common registers,
 * per-socket registers, or per-socket TX/RX buffers. The W5500
 * supports 8 independent sockets, each with 2KB TX and 2KB RX
 * buffer space.
 *
 * This is a register-level model for firmware development. No actual
 * network I/O is performed -- socket commands update status registers
 * to simulate expected state transitions.
 */

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "w5500.h"
#include "vnet.h"

/* WASM proxy queue: CONNECT/LISTEN/CLOSE/SEND control + data bytes are
 * queued here by the socket-command path and drained by JS through
 * bramble_w5500_pop_tx() (pumped each frame in web/index.html and in
 * web/cli.js). Same pop/push pairing as the ETH (bramble_eth_pop_tx)
 * and BLE-HCI (bramble_bt_hci_pop_tx) uplinks. Node-safe: no EM_ASM,
 * no window access — works in browsers AND Node (cli.js). */
#define W5500_WS_TX_SIZE 8192
static uint8_t w5500_ws_tx_buf[W5500_WS_TX_SIZE];
static int w5500_ws_tx_head = 0, w5500_ws_tx_tail = 0;

void w5500_ws_tx_push(const uint8_t *data, int len) {
    if (!data || len <= 0) return;
    for (int i = 0; i < len; i++) {
        int nxt = (w5500_ws_tx_head + 1) % W5500_WS_TX_SIZE;
        if (nxt == w5500_ws_tx_tail) break;  /* full: drop remainder */
        w5500_ws_tx_buf[w5500_ws_tx_head] = data[i];
        w5500_ws_tx_head = nxt;
    }
}

/* Drain queued proxy bytes into out[] (up to maxlen). Returns bytes
 * drained, 0 when empty. framing: caller passes the raw queue through
 * to the proxy socket (messages are self-delimiting). */
int bramble_w5500_pop_tx(uint8_t *out, int maxlen) {
    int n = 0;
    while (n < maxlen && w5500_ws_tx_tail != w5500_ws_tx_head) {
        out[n++] = w5500_ws_tx_buf[w5500_ws_tx_tail];
        w5500_ws_tx_tail = (w5500_ws_tx_tail + 1) % W5500_WS_TX_SIZE;
    }
    return n;
}

int bramble_w5500_tx_len(void) {
    int n = w5500_ws_tx_head - w5500_ws_tx_tail;
    if (n < 0) n += W5500_WS_TX_SIZE;
    return n;
}

/* ========================================================================
 * BSB decoding helpers
 *
 * BSB encoding (5 bits):
 *   0        = common registers
 *   n*4 + 1  = socket n registers   (n = 0..7)
 *   n*4 + 2  = socket n TX buffer
 *   n*4 + 3  = socket n RX buffer
 * ======================================================================== */

/* Returns block type: 0=common, 1=socket reg, 2=TX buf, 3=RX buf */
static int bsb_type(uint8_t bsb) {
    if (bsb == 0) return 0;
    return ((bsb - 1) & 3) + 1;  /* 1=reg, 2=TX, 3=RX */
}

/* Returns socket number for non-common BSBs (0-7) */
static int bsb_socket(uint8_t bsb) {
    if (bsb == 0) return -1;
    return (bsb - 1) >> 2;
}

/* ========================================================================
 * Socket command processing
 * ======================================================================== */

/* Forward: static INTn refresh for the pico-eth board (defined with the
 * board state near the end of this file). */
static void w5500_board_refresh_int(void);

static void set_sock_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Build a sockaddr_in from socket registers */
static void w5500_build_addr(w5500_socket_t *s, struct sockaddr_in *addr) {
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    /* Dest IP from regs (big-endian on wire) */
    uint32_t ip = ((uint32_t)s->regs[W5500_Sn_DIPR0] << 24) |
                  ((uint32_t)s->regs[W5500_Sn_DIPR0 + 1] << 16) |
                  ((uint32_t)s->regs[W5500_Sn_DIPR0 + 2] << 8) |
                  (uint32_t)s->regs[W5500_Sn_DIPR0 + 3];
    addr->sin_addr.s_addr = htonl(ip);
    /* Dest port */
    uint16_t port = ((uint16_t)s->regs[W5500_Sn_DPORT0] << 8) |
                    s->regs[W5500_Sn_DPORT0 + 1];
    addr->sin_port = htons(port);
}

/* Close any host socket associated with this W5500 socket */
static void w5500_close_host_sock(w5500_socket_t *s) {
    if (s->host_fd >= 0) {
        close(s->host_fd);
        s->host_fd = -1;
    }
    if (s->host_listen_fd >= 0) {
        close(s->host_listen_fd);
        s->host_listen_fd = -1;
    }
}

/* ========================================================================
 * MACRAW gateway path (single-gateway Ethernet)
 *
 * Socket 0 in MACRAW mode is the raw-Ethernet door to the shared vnet
 * bus — the SAME bus (and the SAME Go gateway / TAP / peers) the CYW43
 * WiFi path uses. Guest frames written to socket 0's TX buffer with a
 * SEND command are emitted as vnet frames; vnet frames for our MAC (or
 * broadcast) land in socket 0's RX path with the 2-byte length prefix
 * real W5500 MACRAW hardware prepends, plus RECV interrupt + INTn.
 *
 * Why this is the right unification: the old per-socket "live" path
 * dials host TCP/UDP sockets directly (host-stack NAT, no gateway DHCP,
 * no room LAN, diverges from WiFi). MACRAW instead lets the GUEST's own
 * lwIP stack speak DHCP/ARP/IP straight to the gateway — one gateway
 * for WiFi and Ethernet, identical guest-visible network.
 * ======================================================================== */

/* vnet RX callback for a MACRAW socket: wrap [len_hi,len_lo,frame...].
 * Stream layout: frames append back-to-back at the ring tail (tail =
 * s->rx_base + queued, kept in the RSR shadow regs); the guest reads
 * the head at s->rx_base. Consumption advances the internal rx_base
 * plus the guest-visible RX_RD (see RECV below), so guest RX_RD writes
 * (Arduino advances RX_RD per burst) land on already-consumed ground
 * and never disturb framing. RSR shadow regs keep the queue depth —
 * the RX_WR/RD registers mirror tail/head for guests that poll them. */
static void w5500_macraw_vnet_rx(void *ctx, const uint8_t *frame, int len) {
    w5500_t *dev = NULL;
    int sock = -1;
    /* ctx packs (dev ptr, sock idx) via w5500_macraw_attach_ctx(). */
    extern void w5500_macraw_dispatch(void *ctx, w5500_t **dev_out, int *sock_out);
    w5500_macraw_dispatch(ctx, &dev, &sock);
    if (!dev || sock < 0 || sock >= W5500_NUM_SOCKETS) return;
    if (!frame || len < 14 || len > 1514) return;
    w5500_socket_t *s = &dev->sockets[sock];
    if (((s->regs[W5500_Sn_MR] & 0x0F) != W5500_MR_MACRAW) ||
        s->regs[W5500_Sn_SR] != W5500_SOCK_MACRAW)
        return;
    uint16_t rx_rsr = ((uint16_t)s->regs[W5500_Sn_RX_RSR0] << 8) |
                      s->regs[W5500_Sn_RX_RSR0 + 1];
    uint16_t free_space = W5500_RX_BUF_SIZE - rx_rsr;
    uint16_t need = (uint16_t)(len + 2);
    if (free_space < need) return;  /* drop when full ( bombardment-safe) */
    /* Tail of the unread queue; the head lives at s->rx_base. */
    uint16_t rx_wr = (uint16_t)(s->rx_base + rx_rsr);
    s->rx_buf[rx_wr % W5500_RX_BUF_SIZE] = (uint8_t)((len >> 8) & 0xFF);
    s->rx_buf[(rx_wr + 1) % W5500_RX_BUF_SIZE] = (uint8_t)(len & 0xFF);
    for (int i = 0; i < len; i++)
        s->rx_buf[(rx_wr + 2 + (uint16_t)i) % W5500_RX_BUF_SIZE] = frame[i];
    rx_wr = (uint16_t)(rx_wr + need);
    s->regs[W5500_Sn_RX_WR0]     = (rx_wr >> 8) & 0xFF;
    s->regs[W5500_Sn_RX_WR0 + 1] = rx_wr & 0xFF;
    rx_rsr = (uint16_t)(rx_rsr + need);
    s->regs[W5500_Sn_RX_RSR0]     = (rx_rsr >> 8) & 0xFF;
    s->regs[W5500_Sn_RX_RSR0 + 1] = rx_rsr & 0xFF;
    s->regs[W5500_Sn_IR] |= 0x04;  /* RECV */
    w5500_board_refresh_int();
}

/* Per-device MACRAW attach records (max 2 live devices: legacy + board).
 * NOTE: indexed two ways — the vnet port callback gets the table index as
 * ctx, while w5500_macraw_attach() dedups per (dev,sock). Table slots are
 * never reused (vnet ports are append-only), so a stale ctx can never
 * alias a different device. */
#define W5500_MACRAW_MAXDEVS 2
static struct {
    w5500_t *dev;
    int sock;
    uint8_t mac[6];
} w5500_macraw_devs[W5500_MACRAW_MAXDEVS];
static int w5500_macraw_ndevs = 0;

/* Single-gateway switch (default ON): socket-0 MACRAW joins the shared
 * vnet bus. Turn off only to isolate Ethernet from WiFi/gateway traffic
 * (debug). WASM mirrors through bramble_w5500_gw_enable(). */
static int w5500_gw_enable = 1;
void w5500_gw_enable_set(int on) { w5500_gw_enable = on ? 1 : 0; }
int w5500_gw_enabled(void) { return w5500_gw_enable; }

/* Dispatch helper for the vnet callback (ctx = index into table). */
void w5500_macraw_dispatch(void *ctx, w5500_t **dev_out, int *sock_out) {
    intptr_t idx = (intptr_t)ctx;
    if (idx < 0 || idx >= w5500_macraw_ndevs) {
        *dev_out = NULL; *sock_out = -1;
        return;
    }
    *dev_out = w5500_macraw_devs[idx].dev;
    *sock_out = w5500_macraw_devs[idx].sock;
}

/* Attach socket `sock` of `dev` to vnet as a MACRAW port using the
 * W5500 SHAR MAC. Idempotent per (dev,sock); returns port index or -1.
 * NOTE: on WASM the shared vnet may already be up (wifi_enable /
 * eth_set_uplink call vnet_init + set wasm_vnet_on); vnet_init preserves
 * peers across re-init, so calling it here is safe anywhere. */
int w5500_macraw_attach(w5500_t *dev, int sock) {
    if (!dev || sock < 0 || sock >= W5500_NUM_SOCKETS) return -1;
    if (!w5500_gw_enable) return -1;  /* isolated mode: no vnet join */
    uint8_t mac[6];
    for (int i = 0; i < 6; i++) mac[i] = dev->common[W5500_SHAR0 + i];
    /* SHAR may not be programmed yet at OPEN time (Arduino ioLibrary
     * sets SHAR AFTER OPEN via wizchip_sw_reset order... actually
     * before; but a re-OPEN or MAC change must refresh the port MAC
     * or inbound unicast (OFFER to the real MAC) misses a stale port
     * and DHCP stalls after DISCOVER). Refresh a live port's MAC. */
    for (int i = 0; i < w5500_macraw_ndevs; i++) {
        if (w5500_macraw_devs[i].dev == dev && w5500_macraw_devs[i].sock == sock) {
            if (memcmp(w5500_macraw_devs[i].mac, mac, 6) != 0) {
                memcpy(w5500_macraw_devs[i].mac, mac, 6);
                if (dev->vnet_port >= 0)
                    vnet_update_port_mac(dev->vnet_port, mac);
            }
            return dev->vnet_port;  /* already attached */
        }
    }
    if (w5500_macraw_ndevs >= W5500_MACRAW_MAXDEVS) return -1;
    if (!vnet.enabled) vnet_init();
    int idx = w5500_macraw_ndevs;
    w5500_macraw_devs[idx].dev = dev;
    w5500_macraw_devs[idx].sock = sock;
    memcpy(w5500_macraw_devs[idx].mac, mac, 6);
    w5500_macraw_ndevs++;
    int port = vnet_register_port("w5500-macraw", VNET_PORT_W5500, mac,
                                  w5500_macraw_vnet_rx, (void *)(intptr_t)idx);
    dev->vnet_port = port;
    /* WASM note: bramble_wasm.c provides w5500_macraw_vnet_mark() to set
     * wasm_vnet_on when the MACRAW path brings vnet up itself. Weak ref
     * keeps native/test/WASM all linking (native has no such symbol). */
    extern void w5500_macraw_vnet_mark(void) __attribute__((weak));
    if (w5500_macraw_vnet_mark) w5500_macraw_vnet_mark();
    fprintf(stderr, "[W5500] MACRAW socket %d on vnet port %d\n", sock, port);
    return port;
}

/* Emit a raw Ethernet frame from a MACRAW socket's TX buffer to vnet.
 * MACRAW TX is a RING keyed by TX_RD/TX_WR (same as RX): ioLibrary
 * writes each frame at TX_WR (send_data: write_buf at getSn_TX_WR, then
 * setSn_TX_WR(ptr+len)) and SEND emits [TX_RD, TX_WR). The in-tree
 * bare-metal guests instead rewrite each frame at offset 0 and set
 * TX_WR=len (never touching TX_RD). SEND therefore emits the byte
 * window the guest actually dirtied since the last SEND/OPEN
 * (tx_dirty_base/tx_dirty_len tracked on TX writes): for in-tree that
 * is [0, len), for ioLibrary [old_TX_WR, new_TX_WR) — identical to the
 * [TX_RD, TX_WR) window because SEND syncs TX_RD=TX_WR. Copy from the
 * dirty window, then sync TX_RD=TX_WR like hardware. */
static void w5500_macraw_send(w5500_t *dev, int sock) {
    w5500_socket_t *s = &dev->sockets[sock];
    uint16_t tx_wr = ((uint16_t)s->regs[W5500_Sn_TX_WR0] << 8) |
                     s->regs[W5500_Sn_TX_WR0 + 1];
    uint16_t base = s->tx_dirty_valid ? s->tx_dirty_base : 0;
    uint16_t data_len = s->tx_dirty_valid ? s->tx_dirty_len : 0;
    /* Fall back to the pointer window when nothing was tracked (e.g.
     * unit tests that poke TX_WR directly without TX writes). */
    if (!s->tx_dirty_valid) {
        uint16_t tx_rd = ((uint16_t)s->regs[W5500_Sn_TX_RD0] << 8) |
                         s->regs[W5500_Sn_TX_RD0 + 1];
        base = tx_rd;
        data_len = (uint16_t)(tx_wr - tx_rd);
    }
    if (data_len > W5500_TX_BUF_SIZE) data_len = W5500_TX_BUF_SIZE;
    if (data_len < 14 || data_len > 1514) {
        /* Still consume + SEND_OK so firmware doesn't wedge. */
        s->regs[W5500_Sn_TX_RD0] = s->regs[W5500_Sn_TX_WR0];
        s->regs[W5500_Sn_TX_RD0 + 1] = s->regs[W5500_Sn_TX_WR0 + 1];
        s->regs[W5500_Sn_TX_FSR0] = (W5500_TX_BUF_SIZE >> 8) & 0xFF;
        s->regs[W5500_Sn_TX_FSR0 + 1] = W5500_TX_BUF_SIZE & 0xFF;
        s->regs[W5500_Sn_IR] |= 0x10;
        s->tx_dirty_valid = 0;
        s->tx_dirty_len = 0;
        return;
    }
    uint8_t frame[1514];
    for (uint16_t i = 0; i < data_len; i++)
        frame[i] = s->tx_buf[(uint16_t)(base + i) % W5500_TX_BUF_SIZE];
    if (vnet.enabled && dev->vnet_port >= 0)
        vnet_tx_frame(dev->vnet_port, frame, data_len);
    s->regs[W5500_Sn_TX_RD0] = s->regs[W5500_Sn_TX_WR0];
    s->regs[W5500_Sn_TX_RD0 + 1] = s->regs[W5500_Sn_TX_WR0 + 1];
    s->regs[W5500_Sn_TX_FSR0] = (W5500_TX_BUF_SIZE >> 8) & 0xFF;
    s->regs[W5500_Sn_TX_FSR0 + 1] = W5500_TX_BUF_SIZE & 0xFF;
    s->regs[W5500_Sn_IR] |= 0x10;  /* SEND_OK */
    s->tx_dirty_valid = 0;
    s->tx_dirty_len = 0;
}

static void w5500_process_socket_cmd(w5500_t *dev, int sock) {
    w5500_socket_t *s = &dev->sockets[sock];
    uint8_t cmd = s->regs[W5500_Sn_CR];
    uint8_t mode = s->regs[W5500_Sn_MR];

    if (cmd == 0) return;

    switch (cmd) {
    case W5500_CMD_OPEN:
        if ((mode & 0x0F) == W5500_MR_TCP) {
            s->regs[W5500_Sn_SR] = W5500_SOCK_INIT;
            if (dev->live) {
                w5500_close_host_sock(s);
                s->host_fd = socket(AF_INET, SOCK_STREAM, 0);
                if (s->host_fd >= 0) set_sock_nonblock(s->host_fd);
            }
        } else if ((mode & 0x0F) == W5500_MR_UDP) {
            s->regs[W5500_Sn_SR] = W5500_SOCK_UDP;
            if (dev->live) {
                w5500_close_host_sock(s);
                s->host_fd = socket(AF_INET, SOCK_DGRAM, 0);
                if (s->host_fd >= 0) {
                    set_sock_nonblock(s->host_fd);
                    /* Bind to source port if set */
                    uint16_t src_port = ((uint16_t)s->regs[W5500_Sn_PORT0] << 8) |
                                        s->regs[W5500_Sn_PORT0 + 1];
                    if (src_port > 0) {
                        struct sockaddr_in bind_addr;
                        memset(&bind_addr, 0, sizeof(bind_addr));
                        bind_addr.sin_family = AF_INET;
                        bind_addr.sin_addr.s_addr = INADDR_ANY;
                        bind_addr.sin_port = htons(src_port);
                        int opt = 1;
                        setsockopt(s->host_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
                        bind(s->host_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
                    }
                }
            }
        } else if ((mode & 0x0F) == W5500_MR_MACRAW) {
            s->regs[W5500_Sn_SR] = W5500_SOCK_MACRAW;
            /* Single-gateway path: attach socket 0 to vnet so guest
             * Ethernet (DHCP/ARP/IP from the guest's own lwIP) flows to
             * the SAME gateway/TAP/peers as CYW43 WiFi. Other sockets
             * stay available for TCP/UDP offload mode. */
            if (sock == 0)
                w5500_macraw_attach(dev, sock);
        }
        /* TX free = full buffer size; fresh frame starts a new burst.
         * RX queue resets too (fresh stream, mirrors RECV-slide-to-head)
         * and rx_base re-arms as the RD-advance detector (see RECV). */
        s->regs[W5500_Sn_TX_FSR0] = (W5500_TX_BUF_SIZE >> 8) & 0xFF;
        s->regs[W5500_Sn_TX_FSR0 + 1] = W5500_TX_BUF_SIZE & 0xFF;
        s->tx_dirty_valid = 0;
        s->tx_dirty_len = 0;
        if ((mode & 0x0F) == W5500_MR_MACRAW) {
            s->rx_base = 0;
            s->regs[W5500_Sn_RX_RD0] = 0;
            s->regs[W5500_Sn_RX_RD0 + 1] = 0;
            s->regs[W5500_Sn_RX_WR0] = 0;
            s->regs[W5500_Sn_RX_WR0 + 1] = 0;
            s->regs[W5500_Sn_RX_RSR0] = 0;
            s->regs[W5500_Sn_RX_RSR0 + 1] = 0;
        }
        break;

    case W5500_CMD_LISTEN:
        if (s->regs[W5500_Sn_SR] == W5500_SOCK_INIT) {
            s->regs[W5500_Sn_SR] = W5500_SOCK_LISTEN;
            if (dev->live && s->host_fd >= 0) {
                uint16_t src_port = ((uint16_t)s->regs[W5500_Sn_PORT0] << 8) |
                                    s->regs[W5500_Sn_PORT0 + 1];
                struct sockaddr_in bind_addr;
                memset(&bind_addr, 0, sizeof(bind_addr));
                bind_addr.sin_family = AF_INET;
                bind_addr.sin_addr.s_addr = INADDR_ANY;
                bind_addr.sin_port = htons(src_port);
                int opt = 1;
                setsockopt(s->host_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
                if (bind(s->host_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) == 0 &&
                    listen(s->host_fd, 1) == 0) {
                    s->host_listen_fd = s->host_fd;
                    s->host_fd = -1;
                }
            }
#ifdef __EMSCRIPTEN__
            /* WASM live: queue LISTEN for the proxy pump (Node-safe). */
            if (dev->live) {
                int widx = (int)(s - dev->sockets);
                int wport = ((int)s->regs[W5500_Sn_PORT0] << 8) | (int)s->regs[W5500_Sn_PORT0 + 1];
                uint8_t msg[4];
                msg[0] = 0x4C; msg[1] = (uint8_t)(widx & 0xFF);
                msg[2] = (uint8_t)(wport & 0xFF); msg[3] = (uint8_t)((wport >> 8) & 0xFF);
                w5500_ws_tx_push(msg, 4);
            }
#endif
        }
        break;

    case W5500_CMD_CONNECT:
        if (s->regs[W5500_Sn_SR] == W5500_SOCK_INIT) {
            /* M15: don't claim ESTABLISHED until the connect completes.
             * Offline (no live backend) keeps the old instant model. */
            if (dev->live && s->host_fd >= 0) {
                struct sockaddr_in dest;
                w5500_build_addr(s, &dest);
                /* Non-blocking connect — may succeed or be in progress */
                int rc = connect(s->host_fd, (struct sockaddr *)&dest, sizeof(dest));
                if (rc == 0) {
                    s->regs[W5500_Sn_SR] = W5500_SOCK_ESTABLISHED;
                } else if (errno == EINPROGRESS) {
                    s->regs[W5500_Sn_SR] = W5500_SOCK_SYNSENT;
                } else {
                    fprintf(stderr, "[W5500] Socket %d connect failed: %s\n",
                            sock, strerror(errno));
                    s->regs[W5500_Sn_SR] = W5500_SOCK_CLOSED;
                }
            } else {
                s->regs[W5500_Sn_SR] = W5500_SOCK_ESTABLISHED;
            }
#ifdef __EMSCRIPTEN__
            /* WASM live: queue CONNECT for the proxy pump (Node-safe).
             * Format (matches net_proxy.py): [0x43,sock,6,0,udp,
             * a0,a1,a2,a3,port_lo,port_hi] (11B). */
            if (dev->live) {
                int widx = (int)(s - dev->sockets);
                uint8_t msg[11];
                msg[0] = 0x43; msg[1] = (uint8_t)(widx & 0xFF);
                msg[2] = 6; msg[3] = 0;
                msg[4] = (((int)s->regs[W5500_Sn_MR] & 0x0F) == W5500_MR_UDP) ? 1 : 0;
                msg[5] = s->regs[W5500_Sn_DIPR0];
                msg[6] = s->regs[W5500_Sn_DIPR0 + 1];
                msg[7] = s->regs[W5500_Sn_DIPR0 + 2];
                msg[8] = s->regs[W5500_Sn_DIPR0 + 3];
                {
                    /* DPORT0 regs are big-endian (hi,lo); the proxy wire
                     * format is little-endian (port_lo,port_hi). */
                    int wport = ((int)s->regs[W5500_Sn_DPORT0] << 8) |
                                (int)s->regs[W5500_Sn_DPORT0 + 1];
                    msg[9] = (uint8_t)(wport & 0xFF);
                    msg[10] = (uint8_t)((wport >> 8) & 0xFF);
                }
                w5500_ws_tx_push(msg, 11);
            }
#endif
        }
        break;

    case W5500_CMD_CLOSE:
        s->regs[W5500_Sn_SR] = W5500_SOCK_CLOSED;
        if (dev->live) w5500_close_host_sock(s);
#ifdef __EMSCRIPTEN__
        /* WASM live: queue CLOSE for the proxy pump (Node-safe). */
        if (dev->live) {
            int widx = (int)(s - dev->sockets);
            uint8_t msg[2];
            msg[0] = 0x58; msg[1] = (uint8_t)(widx & 0xFF);
            w5500_ws_tx_push(msg, 2);
        }
#endif
        break;

    case W5500_CMD_SEND: {
        /* Read TX data from buffer (C5: clamp to avoid stack overflow) */
        uint16_t tx_rd = ((uint16_t)s->regs[W5500_Sn_TX_RD0] << 8) |
                         s->regs[W5500_Sn_TX_RD0 + 1];
        uint16_t tx_wr = ((uint16_t)s->regs[W5500_Sn_TX_WR0] << 8) |
                         s->regs[W5500_Sn_TX_WR0 + 1];
        uint16_t data_len = (uint16_t)(tx_wr - tx_rd);
        if (data_len > W5500_TX_BUF_SIZE) data_len = W5500_TX_BUF_SIZE;

        /* MACRAW socket 0: raw Ethernet straight to the shared vnet bus
         * (single gateway). Takes precedence over host-socket live mode. */
        if (((s->regs[W5500_Sn_MR] & 0x0F) == W5500_MR_MACRAW) &&
            sock == 0 && dev->vnet_port >= 0) {
            w5500_macraw_send(dev, sock);
            break;
        }

        if (dev->live && s->host_fd >= 0 && data_len > 0) {
            uint8_t send_buf[W5500_TX_BUF_SIZE];
            for (uint16_t i = 0; i < data_len; i++) {
                send_buf[i] = s->tx_buf[(tx_rd + i) % W5500_TX_BUF_SIZE];
            }

            uint8_t mode_val = s->regs[W5500_Sn_MR] & 0x0F;
            if (mode_val == W5500_MR_UDP) {
                struct sockaddr_in dest;
                w5500_build_addr(s, &dest);
                sendto(s->host_fd, send_buf, data_len, 0,
                       (struct sockaddr *)&dest, sizeof(dest));
            } else {
                send(s->host_fd, send_buf, data_len, MSG_NOSIGNAL);
            }
        }
#ifdef __EMSCRIPTEN__
        /* WASM live via WebSocket proxy pump: queue SEND even when host_fd<0.
         * The JS pump (index.html frame loop / cli.js tick) forwards queued
         * bytes to the proxy; the proxy performs real TCP/UDP and returns
         * data via bramble_w5500_dev_push_rx().
         * Format: [0x57,sock:1][len:2 LE][payload]. */
        if (dev->live && data_len > 0) {
            int widx = (int)(s - dev->sockets);
            /* Header + payload may exceed the queue remainder: push header
             * first, then as much payload as fits (proxy uses len prefix). */
            uint8_t hdr[4];
            hdr[0] = 0x57; hdr[1] = (uint8_t)(widx & 0xFF);
            hdr[2] = (uint8_t)(data_len & 0xFF);
            hdr[3] = (uint8_t)((data_len >> 8) & 0xFF);
            w5500_ws_tx_push(hdr, 4);
            for (uint16_t i = 0; i < data_len; i++) {
                uint8_t b = s->tx_buf[(tx_rd + i) % W5500_TX_BUF_SIZE];
                w5500_ws_tx_push(&b, 1);
            }
        }
#endif

        /* Advance TX read pointer to write pointer */
        s->regs[W5500_Sn_TX_RD0] = s->regs[W5500_Sn_TX_WR0];
        s->regs[W5500_Sn_TX_RD0 + 1] = s->regs[W5500_Sn_TX_WR0 + 1];
        /* Restore full TX free space */
        s->regs[W5500_Sn_TX_FSR0] = (W5500_TX_BUF_SIZE >> 8) & 0xFF;
        s->regs[W5500_Sn_TX_FSR0 + 1] = W5500_TX_BUF_SIZE & 0xFF;
        /* Set SEND_OK interrupt */
        s->regs[W5500_Sn_IR] |= 0x10;
        break;
    }

    case W5500_CMD_RECV:
        /* Advance RX read pointer, clear received size.
         * MACRAW: the RX stream is an append-only queue whose head is
         * the internal rx_base (absolute ring offset); appends land at
         * rx_base + queued and reads resolve rx_base + (addr -
         * cursor_base) (see read path). RECV advances the head — two
         * guest rhythms, detected via RX_RD:
         * - In-tree bare-metal guests read addr-0 bursts and NEVER
         *   touch RX_RD (RX_RD == rx_base): consume ONE
         *   length-prefixed entry ([len_hi,len_lo,frame...]) by parsing
         *   the prefix at the head and advancing past it. No sliding:
         *   the head pointer does the work, so back-to-back frames
         *   (OFFER then ACK) stay addressable.
         * - Arduino ioLibrary (wizchip_recv_data) pulls bursts at RX_RD
         *   and advances RX_RD per burst (RX_RD != rx_base): commit that
         *   advance (drop the pulled bytes, even a bare 2-byte prefix
         *   pull — the body follows at the new RX_RD and the next RECV
         *   commits the rest). This is also why a bare RECV with
         *   neither queued bytes nor an RX_RD advance is a no-op: it
         *   must NOT fabricate a length from stale ring bytes (Arduino
         *   issues RECV after every burst INCLUDING the len-prefix
         *   read, when RSR is already 0; consuming phantom bytes then
         *   shifts the real frame and DHCP stalls after DISCOVER).
         * RSR shadow regs keep the queue depth; RX_WR/RX_RD mirror
         * tail/head for polling guests. RECV interrupt clears only
         * when empty (level semantics: pending frames keep INTn
         * asserted, like hardware). */
        if (((s->regs[W5500_Sn_MR] & 0x0F) == W5500_MR_MACRAW) &&
            sock == 0 && dev->vnet_port >= 0) {
            uint16_t rx_rd = ((uint16_t)s->regs[W5500_Sn_RX_RD0] << 8) |
                             s->regs[W5500_Sn_RX_RD0 + 1];
            uint16_t rx_rsr = ((uint16_t)s->regs[W5500_Sn_RX_RSR0] << 8) |
                              s->regs[W5500_Sn_RX_RSR0 + 1];
            /* Arduino path: RECV after burst pulls (RX_RD advanced past
             * consumed bytes while RSR still counts them). Commit the
             * advance; the head follows RX_RD and the tail follows. */
            if (rx_rd != s->rx_base && rx_rsr > 0) {
                uint16_t pulled = (uint16_t)(rx_rd - s->rx_base);
                if (pulled > rx_rsr) pulled = rx_rsr;
                uint16_t remain = (uint16_t)(rx_rsr - pulled);
                s->rx_base = rx_rd;
                uint16_t tail = (uint16_t)(rx_rd + remain);
                s->regs[W5500_Sn_RX_WR0]     = (tail >> 8) & 0xFF;
                s->regs[W5500_Sn_RX_WR0 + 1] = tail & 0xFF;
                s->regs[W5500_Sn_RX_RSR0]     = (remain >> 8) & 0xFF;
                s->regs[W5500_Sn_RX_RSR0 + 1] = remain & 0xFF;
                if (remain == 0)
                    s->regs[W5500_Sn_IR] &= (uint8_t)~0x04;  /* RECV done */
                break;
            }
            if (rx_rsr == 0) {
                /* Empty queue: no-op (do NOT touch RSR/IR). */
                break;
            }
            /* In-tree path: consume one length-prefixed entry by
             * advancing the head past it (no slide — the head pointer
             * keeps back-to-back frames addressable). */
            if (rx_rsr >= 2) {
                uint16_t base = s->rx_base % W5500_RX_BUF_SIZE;
                uint16_t flen = ((uint16_t)s->rx_buf[base] << 8) |
                                s->rx_buf[(base + 1) % W5500_RX_BUF_SIZE];
                uint16_t total = (uint16_t)(flen + 2);
                if (total > rx_rsr) total = rx_rsr;
                uint16_t remain = (uint16_t)(rx_rsr - total);
                uint16_t new_rd = (uint16_t)(s->rx_base + total);
                s->rx_base = new_rd;
                uint16_t tail = (uint16_t)(new_rd + remain);
                s->regs[W5500_Sn_RX_RD0]     = (new_rd >> 8) & 0xFF;
                s->regs[W5500_Sn_RX_RD0 + 1] = new_rd & 0xFF;
                s->regs[W5500_Sn_RX_WR0]     = (tail >> 8) & 0xFF;
                s->regs[W5500_Sn_RX_WR0 + 1] = tail & 0xFF;
                s->regs[W5500_Sn_RX_RSR0]     = (remain >> 8) & 0xFF;
                s->regs[W5500_Sn_RX_RSR0 + 1] = remain & 0xFF;
                if (remain == 0)
                    s->regs[W5500_Sn_IR] &= (uint8_t)~0x04;  /* RECV done */
            } else {
                s->regs[W5500_Sn_RX_RSR0] = 0;
                s->regs[W5500_Sn_RX_RSR0 + 1] = 0;
                s->regs[W5500_Sn_IR] &= (uint8_t)~0x04;
            }
            break;
        }
        s->regs[W5500_Sn_RX_RSR0] = 0;
        s->regs[W5500_Sn_RX_RSR0 + 1] = 0;
        break;

    default:
        break;
    }

    /* Command register auto-clears after execution */
    s->regs[W5500_Sn_CR] = 0x00;
    /* pico-eth: INTn (active-low GPIO21) follows socket IR. The static
     * refresh probes whether this command targeted the board device. */
    w5500_board_refresh_int();
}

/* ========================================================================
 * Read/write a single byte from the appropriate block
 * ======================================================================== */

static uint8_t w5500_read_byte(w5500_t *dev, uint8_t bsb, uint16_t addr) {
    int type = bsb_type(bsb);
    int sock = bsb_socket(bsb);

    switch (type) {
    case 0: /* Common registers */
        /* SIR is computed: bit n = socket n Sn_IR nonzero. */
        if (addr == W5500_SIR) {
            uint8_t sir = 0;
            for (int i = 0; i < W5500_NUM_SOCKETS; i++)
                if (dev->sockets[i].regs[W5500_Sn_IR]) sir |= (uint8_t)(1u << i);
            return sir;
        }
        if (addr < W5500_COMMON_REG_SIZE)
            return dev->common[addr];
        return 0x00;

    case 1: /* Socket registers */
        if (sock >= 0 && sock < W5500_NUM_SOCKETS &&
            addr < W5500_SOCKET_REG_SIZE) {
            /* TX_FSR is computed from the buffer-size register (units
             * of 1KB: sock0 owns the full 16KB when the guest sets
             * Sn_TXBUF_SIZE=16 like ioLibrary's begin() does; default
             * 2 = 2KB). A fixed 2048 constant wedges any guest that
             * asks for more (sendFrame spins on FSR < len forever and
             * DHCP never transmits). RX_RSR is the shadow queue depth
             * (see append/RECV); RX_WR/RX_RD mirror tail/head.
             * NOTE: both are 16-bit word registers — the ioLibrary
             * read_word path reads them as TWO single-byte frames
             * (addr, addr+1), so serve each byte independently. */
            if (addr == W5500_Sn_TX_FSR0 || addr == W5500_Sn_TX_FSR0 + 1) {
                uint32_t kb = dev->sockets[sock].regs[W5500_Sn_TXBUF_SIZE];
                if (kb == 0) kb = 1;
                if (kb > 16) kb = 16;
                uint16_t tx_rd = ((uint16_t)dev->sockets[sock].regs[W5500_Sn_TX_RD0] << 8) |
                                 dev->sockets[sock].regs[W5500_Sn_TX_RD0 + 1];
                uint16_t tx_wr = ((uint16_t)dev->sockets[sock].regs[W5500_Sn_TX_WR0] << 8) |
                                 dev->sockets[sock].regs[W5500_Sn_TX_WR0 + 1];
                uint16_t used = (uint16_t)(tx_wr - tx_rd);
                uint32_t cap = kb * 1024u;
                uint16_t free = used >= cap ? 0 : (uint16_t)(cap - used);
                return (addr == W5500_Sn_TX_FSR0) ? (free >> 8) & 0xFF : free & 0xFF;
            }
            return dev->sockets[sock].regs[addr];
        }
        return 0x00;

    case 2: /* Socket TX buffer */
        if (sock >= 0 && sock < W5500_NUM_SOCKETS)
            return dev->sockets[sock].tx_buf[addr % W5500_TX_BUF_SIZE];
        return 0x00;

    case 3: /* Socket RX buffer */
        if (sock >= 0 && sock < W5500_NUM_SOCKETS) {
            /* MACRAW RX is a WRAP-AROUND ring keyed by the internal
             * stream head (rx_base): the oldest unconsumed [len+frame]
             * entry is at rx_base, and RECV advances the head past the
             * consumed entry (no slide — the head pointer keeps
             * back-to-back frames addressable). VDM DATA bytes stream in
             * order (dev->addr++ per byte), so the j-th byte of the
             * frame is simply at base+j where j = addr - cursor_base
             * (the frame's start VDM address, latched on the first DATA
             * byte of each CS frame). A separate per-byte counter would
             * double-count (addr already advances). Guest RX_RD writes
             * (ioLibrary advances RX_RD per burst without RECV) are
             * only an advance detector for RECV (see above); they never
             * disturb framing — consumption happens via RECV, matching
             * the append-at-rx_base+rsr design. This serves BOTH guest
             * rhythms: in-tree guests read addr-0 bursts (their RX_RD
             * stays 0) and Arduino readFrameSize/readFrameData bursts
             * at RX_RD with commit-on-RECV both land on real bytes. */
            if (sock == 0 &&
                ((dev->sockets[sock].regs[W5500_Sn_MR] & 0x0F) == W5500_MR_MACRAW) &&
                dev->sockets[sock].regs[W5500_Sn_SR] == W5500_SOCK_MACRAW) {
                w5500_socket_t *ms = &dev->sockets[sock];
                uint16_t base = ms->rx_base % W5500_RX_BUF_SIZE;
                uint16_t off = (uint16_t)(addr - ms->rx_cursor_base);
                return ms->rx_buf[(base + off) % W5500_RX_BUF_SIZE];
            }
            return dev->sockets[sock].rx_buf[addr % W5500_RX_BUF_SIZE];
        }
        return 0x00;
    }

    return 0x00;
}

/* Socket Sn_IR writes are write-1-to-clear on real silicon. Without
 * this, firmware that ACKs RECV/CON (e.g. WIZnet ioLibrary) leaves the
 * bit stuck and INTn never deasserts. Applies to both the legacy -net-live
 * device and the pico-eth board (same register model). */
static void w5500_write_sn_ir(w5500_t *dev, int sock, uint8_t val) {
    dev->sockets[sock].regs[W5500_Sn_IR] &= (uint8_t)~val;
    w5500_board_refresh_int();
}

static void w5500_write_byte(w5500_t *dev, uint8_t bsb, uint16_t addr,
                             uint8_t val) {
    int type = bsb_type(bsb);
    int sock = bsb_socket(bsb);

    switch (type) {
    case 0: /* Common registers */
        if (addr == W5500_VERSIONR) return;  /* Read-only */
        if (addr == W5500_IR) { dev->common[W5500_IR] &= (uint8_t)~val; break; }
        if (addr == W5500_SIR) break;  /* Read-only socket-interrupt flags */
        if (addr < W5500_COMMON_REG_SIZE) {
            dev->common[addr] = val;
            /* SHAR (MAC) may be programmed after MACRAW OPEN (Arduino
             * ioLibrary: sw_reset -> SHAR -> OPEN... but any reprogram
             * order must refresh the vnet port MAC or inbound unicast
             * misses a stale port and DHCP stalls after DISCOVER). */
            if (addr >= W5500_SHAR0 && addr < W5500_SHAR0 + 6 &&
                dev->vnet_port >= 0) {
                uint8_t mac[6];
                for (int i = 0; i < 6; i++)
                    mac[i] = dev->common[W5500_SHAR0 + i];
                vnet_update_port_mac(dev->vnet_port, mac);
                for (int i = 0; i < w5500_macraw_ndevs; i++) {
                    if (w5500_macraw_devs[i].dev == dev) {
                        memcpy(w5500_macraw_devs[i].mac, mac, 6);
                        break;
                    }
                }
            }
        }
        break;

    case 1: /* Socket registers */
        if (sock >= 0 && sock < W5500_NUM_SOCKETS &&
            addr < W5500_SOCKET_REG_SIZE) {
            if (addr == W5500_Sn_IR) {
                w5500_write_sn_ir(dev, sock, val);
                break;
            }
            dev->sockets[sock].regs[addr] = val;
            /* Process command register writes */
            if (addr == W5500_Sn_CR)
                w5500_process_socket_cmd(dev, sock);
        }
        break;

    case 2: /* Socket TX buffer */
        if (sock >= 0 && sock < W5500_NUM_SOCKETS) {
            /* TX is a RING keyed by TX_RD/TX_WR: the guest's VDM burst
             * address IS the ring offset (wizchip_send_data writes at
             * getSn_TX_WR, then advances TX_WR by len). Store at the raw
             * VDM address mod ring size and extend the dirty window SEND
             * emits (see w5500_macraw_send). */
            w5500_socket_t *ts = &dev->sockets[sock];
            ts->tx_buf[addr % W5500_TX_BUF_SIZE] = val;
            if (!ts->tx_dirty_valid) {
                ts->tx_dirty_base = addr % W5500_TX_BUF_SIZE;
                ts->tx_dirty_len = 1;
                ts->tx_dirty_valid = 1;
            } else {
                uint16_t end = (uint16_t)(ts->tx_dirty_base + ts->tx_dirty_len);
                uint16_t a = addr % W5500_TX_BUF_SIZE;
                /* Contiguous sequential burst: extend. A fresh frame
                 * starting back at 0 (in-tree rhythm) restarts it. */
                if (a == (uint16_t)(end % W5500_TX_BUF_SIZE))
                    ts->tx_dirty_len++;
                else if (a == 0 && ts->tx_dirty_len > 0 &&
                         ts->tx_dirty_base != 0) {
                    ts->tx_dirty_base = 0;
                    ts->tx_dirty_len = 1;
                } else {
                    /* Non-sequential poke: widen conservatively. */
                    ts->tx_dirty_len = W5500_TX_BUF_SIZE;
                }
                if (ts->tx_dirty_len > W5500_TX_BUF_SIZE)
                    ts->tx_dirty_len = W5500_TX_BUF_SIZE;
            }
        }
        break;

    case 3: /* Socket RX buffer */
        if (sock >= 0 && sock < W5500_NUM_SOCKETS) {
            /* MACRAW RX is a length-prefixed stream, not random-access
             * memory: ignore direct RX-buffer writes on MACRAW sockets
             * (real hardware advances an internal read pointer instead;
             * consumption happens via RECV below). EXCEPTION: the RX_RD
             * pointer registers themselves (Sn_RX_RD0/1) are guest-
             * writable (Arduino Wiznet5500::wizchip_recv_data() sets
             * RX_RD=old+len after every burst) — those go through the
             * socket-register path (type 1), not here. */
            if (sock == 0 &&
                ((dev->sockets[sock].regs[W5500_Sn_MR] & 0x0F) == W5500_MR_MACRAW) &&
                dev->sockets[sock].regs[W5500_Sn_SR] == W5500_SOCK_MACRAW)
                break;
            dev->sockets[sock].rx_buf[addr % W5500_RX_BUF_SIZE] = val;
        }
        break;
    }
}

/* ========================================================================
 * Initialization
 * ======================================================================== */

void w5500_init(w5500_t *dev) {
    memset(dev, 0, sizeof(*dev));

    /* Version register (read-only) */
    dev->common[W5500_VERSIONR] = 0x04;

    /* Default MAC: 02:00:00:00:00:01 */
    dev->common[W5500_SHAR0]     = 0x02;
    dev->common[W5500_SHAR0 + 1] = 0x00;
    dev->common[W5500_SHAR0 + 2] = 0x00;
    dev->common[W5500_SHAR0 + 3] = 0x00;
    dev->common[W5500_SHAR0 + 4] = 0x00;
    dev->common[W5500_SHAR0 + 5] = 0x01;

    /* IP defaults to 0.0.0.0 (already zero from memset) */

    /* PHY config: link up, 100Mbps full-duplex, not in reset */
    dev->common[W5500_PHYCFGR] = W5500_PHY_RST | W5500_PHY_LINK |
                                  W5500_PHY_SPD | W5500_PHY_DPX;

    /* Default retry time: 200ms (0x07D0 = 2000 * 100us) */
    dev->common[W5500_RTR0]     = 0x07;
    dev->common[W5500_RTR0 + 1] = 0xD0;

    /* Default retry count */
    dev->common[W5500_RCR] = 0x08;

    /* Live networking defaults */
    dev->live = 0;
    dev->vnet_port = -1;

    /* Initialize each socket with default buffer sizes and TTL */
    for (int i = 0; i < W5500_NUM_SOCKETS; i++) {
        w5500_socket_t *s = &dev->sockets[i];
        s->regs[W5500_Sn_RXBUF_SIZE] = 2;  /* 2KB */
        s->regs[W5500_Sn_TXBUF_SIZE] = 2;  /* 2KB */
        s->regs[W5500_Sn_TTL] = 128;
        s->regs[W5500_Sn_SR] = W5500_SOCK_CLOSED;
        /* TX free = full buffer */
        s->regs[W5500_Sn_TX_FSR0] = (W5500_TX_BUF_SIZE >> 8) & 0xFF;
        s->regs[W5500_Sn_TX_FSR0 + 1] = W5500_TX_BUF_SIZE & 0xFF;
        s->host_fd = -1;
        s->host_listen_fd = -1;
    }
}

/* ========================================================================
 * SPI interface callbacks
 * ======================================================================== */

uint8_t w5500_spi_xfer(void *ctx, uint8_t mosi) {
    w5500_t *dev = (w5500_t *)ctx;
    uint8_t miso = 0xFF;

    if (!dev->cs_active) return 0xFF;

#ifdef W5500_SPI_TRACE
    /* Byte-level SPI trace (CS frame aware): enable with
     * -DW5500_SPI_TRACE to watch a real guest driver frame-by-frame.
     * MISO is filled in the DATA-phase branch below. */
    if (dev->phase == W5500_PHASE_DATA)
        fprintf(stderr, "[W5500-SPI] MOSI=%02X bsb=%02X addr=%04X %s ",
                mosi, dev->bsb, dev->addr, dev->rw ? "W" : "R");
#endif

    switch (dev->phase) {
    case W5500_PHASE_ADDR_HI:
        dev->addr = (uint16_t)mosi << 8;
        dev->phase = W5500_PHASE_ADDR_LO;
        break;

    case W5500_PHASE_ADDR_LO:
        dev->addr |= mosi;
        dev->phase = W5500_PHASE_CONTROL;
        break;

    case W5500_PHASE_CONTROL:
        dev->ctl = mosi;
        dev->bsb = (mosi >> 3) & 0x1F;
        dev->rw  = (mosi >> 2) & 1;
        /* Data-offset field (mosi[1:0]): the W5500 control byte is
         * [BSB:5][R/W:1][OM:2], and this driver family (Arduino
         * Wiznet5500, ioLibrary) ALWAYS uses VDM (offset 0) for
         * multi-byte bursts — the low 2 bits you see here (e.g. 0x08
         * vs 0x0C on socket-reg reads) are the block-offset LSBs of
         * the 15-bit address extension, NOT a 1/2/4-byte FDM length.
         * Treating them as OM truncates every multi-byte read whose
         * VDM address is odd (RX/TX regs 0x26/0x28/0x24 came back
         * short), so stream all DATA bytes under one CS like the
         * hardware does for VDM. */
        dev->phase = W5500_PHASE_DATA;
        break;

    case W5500_PHASE_DATA:
        if (dev->rw) {
            /* Write */
            w5500_write_byte(dev, dev->bsb, dev->addr, mosi);
        } else {
            /* Read */
            miso = w5500_read_byte(dev, dev->bsb, dev->addr);
            /* Latch the RX frame start address on the first DATA byte of
             * each CS frame (MACRAW RX buffer only): the read path
             * resolves addr-cursor_base as the stream-relative offset,
             * so no per-byte counter is needed here (addr++ below already
             * advances; a second counter would double-count). */
            {
                int type = bsb_type(dev->bsb);
                int sock = bsb_socket(dev->bsb);
                if (type == 3 && sock == 0 &&
                    !dev->sockets[0].rx_cursor_valid) {
                    dev->sockets[0].rx_cursor_base = dev->addr;
                    dev->sockets[0].rx_cursor_valid = 1;
                }
            }
        }
#ifdef W5500_SPI_TRACE
        fprintf(stderr, "MISO=%02X\n", miso);
#endif
        dev->addr++;
        break;
    }

    return miso;
}

void w5500_spi_cs(void *ctx, int cs_active) {
    w5500_t *dev = (w5500_t *)ctx;
    int was = dev->cs_active;
    dev->cs_active = cs_active;

    if (cs_active && !was) {
        /* CS asserted: reset frame state machine + clear the RX frame
         * start latch (see DATA phase above). rx_cursor is retained for
         * compatibility but no longer used by the read path. */
        dev->phase = W5500_PHASE_ADDR_HI;
        for (int i = 0; i < W5500_NUM_SOCKETS; i++) {
            dev->sockets[i].rx_cursor_base = 0;
            dev->sockets[i].rx_cursor_valid = 0;
        }
    }
}

/* ========================================================================
 * Live Networking: Poll host sockets for incoming data
 * ======================================================================== */

void w5500_poll(w5500_t *dev) {
    if (!dev->live) return;

    for (int i = 0; i < W5500_NUM_SOCKETS; i++) {
        w5500_socket_t *s = &dev->sockets[i];

        /* Accept incoming TCP connections */
        if (s->host_listen_fd >= 0 &&
            s->regs[W5500_Sn_SR] == W5500_SOCK_LISTEN) {
            struct pollfd pfd = { .fd = s->host_listen_fd, .events = POLLIN };
            if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
                struct sockaddr_in client;
                socklen_t clen = sizeof(client);
                int cfd = accept(s->host_listen_fd, (struct sockaddr *)&client, &clen);
                if (cfd >= 0) {
                    set_sock_nonblock(cfd);
                    /* L18: close any stale connection before adopting the new fd */
                    if (s->host_fd >= 0) close(s->host_fd);
                    s->host_fd = cfd;
                    s->regs[W5500_Sn_SR] = W5500_SOCK_ESTABLISHED;
                    /* Store client IP/port in dest registers */
                    uint32_t ip = ntohl(client.sin_addr.s_addr);
                    s->regs[W5500_Sn_DIPR0]     = (ip >> 24) & 0xFF;
                    s->regs[W5500_Sn_DIPR0 + 1] = (ip >> 16) & 0xFF;
                    s->regs[W5500_Sn_DIPR0 + 2] = (ip >>  8) & 0xFF;
                    s->regs[W5500_Sn_DIPR0 + 3] = ip & 0xFF;
                    uint16_t port = ntohs(client.sin_port);
                    s->regs[W5500_Sn_DPORT0]     = (port >> 8) & 0xFF;
                    s->regs[W5500_Sn_DPORT0 + 1] = port & 0xFF;
                    /* Set CON interrupt */
                    s->regs[W5500_Sn_IR] |= 0x01;
                }
            }
        }

        /* M15: complete non-blocking connects (POLLOUT + SO_ERROR) */
        if (s->host_fd >= 0 && s->regs[W5500_Sn_SR] == W5500_SOCK_SYNSENT) {
            struct pollfd pfd = { .fd = s->host_fd, .events = POLLOUT };
            if (poll(&pfd, 1, 0) > 0 &&
                (pfd.revents & (POLLOUT | POLLERR | POLLHUP))) {
                int err = 0;
                socklen_t elen = sizeof(err);
                getsockopt(s->host_fd, SOL_SOCKET, SO_ERROR, &err, &elen);
                if (err == 0) {
                    s->regs[W5500_Sn_SR] = W5500_SOCK_ESTABLISHED;
                    s->regs[W5500_Sn_IR] |= 0x01;  /* CON interrupt */
                } else {
                    s->regs[W5500_Sn_SR] = W5500_SOCK_CLOSED;
                    close(s->host_fd);
                    s->host_fd = -1;
                }
            }
        }

        /* Read incoming data into RX buffer */
        if (s->host_fd >= 0 &&
            (s->regs[W5500_Sn_SR] == W5500_SOCK_ESTABLISHED ||
             s->regs[W5500_Sn_SR] == W5500_SOCK_UDP)) {

            uint16_t rx_rsr = ((uint16_t)s->regs[W5500_Sn_RX_RSR0] << 8) |
                              s->regs[W5500_Sn_RX_RSR0 + 1];
            uint16_t free_space = W5500_RX_BUF_SIZE - rx_rsr;
            if (free_space == 0) continue;

            struct pollfd pfd = { .fd = s->host_fd, .events = POLLIN };
            if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
                uint16_t rx_wr = ((uint16_t)s->regs[W5500_Sn_RX_WR0] << 8) |
                                 s->regs[W5500_Sn_RX_WR0 + 1];

                uint8_t tmp[W5500_RX_BUF_SIZE];
                /* M16: UDP datagrams carry an 8-byte header (src IP/port/len) */
                int is_udp = (s->regs[W5500_Sn_SR] == W5500_SOCK_UDP);
                uint8_t hdr[8];
                int hl = 0;
                ssize_t n = -1;  /* -1 = nothing attempted (no room / EAGAIN) */
                if (is_udp) {
                    if (free_space > 8) {
                        struct sockaddr_in src;
                        socklen_t slen = sizeof(src);
                        n = recvfrom(s->host_fd, tmp,
                                     free_space - 8 < sizeof(tmp) ? free_space - 8 : sizeof(tmp),
                                     0, (struct sockaddr *)&src, &slen);
                        if (n > 0) {
                            uint32_t sip = ntohl(src.sin_addr.s_addr);
                            uint16_t sport = ntohs(src.sin_port);
                            hdr[0] = (sip >> 24) & 0xFF; hdr[1] = (sip >> 16) & 0xFF;
                            hdr[2] = (sip >> 8) & 0xFF;  hdr[3] = sip & 0xFF;
                            hdr[4] = (sport >> 8) & 0xFF; hdr[5] = sport & 0xFF;
                            hdr[6] = ((uint16_t)n >> 8) & 0xFF; hdr[7] = (uint16_t)n & 0xFF;
                            hl = 8;
                        }
                    }
                } else {
                    n = recv(s->host_fd, tmp,
                             free_space < sizeof(tmp) ? free_space : sizeof(tmp), 0);
                }
                if (n > 0) {
                    for (int j = 0; j < hl; j++) {
                        s->rx_buf[(rx_wr + (uint16_t)j) % W5500_RX_BUF_SIZE] = hdr[j];
                    }
                    for (ssize_t j = 0; j < n; j++) {
                        s->rx_buf[(rx_wr + (uint16_t)(hl + j)) % W5500_RX_BUF_SIZE] = tmp[j];
                    }
                    uint16_t total = (uint16_t)(hl + n);
                    rx_wr = (uint16_t)(rx_wr + total);
                    s->regs[W5500_Sn_RX_WR0]     = (rx_wr >> 8) & 0xFF;
                    s->regs[W5500_Sn_RX_WR0 + 1] = rx_wr & 0xFF;
                    rx_rsr = (uint16_t)(rx_rsr + total);
                    s->regs[W5500_Sn_RX_RSR0]     = (rx_rsr >> 8) & 0xFF;
                    s->regs[W5500_Sn_RX_RSR0 + 1] = rx_rsr & 0xFF;
                    /* Set RECV interrupt */
                    s->regs[W5500_Sn_IR] |= 0x04;
                } else if (n == 0) {
                    /* Peer disconnected */
                    s->regs[W5500_Sn_SR] = W5500_SOCK_CLOSED;
                    s->regs[W5500_Sn_IR] |= 0x02;  /* DISCON interrupt */
                    close(s->host_fd);
                    s->host_fd = -1;
                }
            }

            /* Check for errors/disconnect */
            if (s->host_fd >= 0) {
                struct pollfd pfd2 = { .fd = s->host_fd, .events = 0 };
                if (poll(&pfd2, 1, 0) > 0 &&
                    (pfd2.revents & (POLLERR | POLLHUP))) {
                    s->regs[W5500_Sn_SR] = W5500_SOCK_CLOSED;
                    s->regs[W5500_Sn_IR] |= 0x02;
                    close(s->host_fd);
                    s->host_fd = -1;
                }
            }
        }
    }
}

void w5500_set_live(w5500_t *dev, int enable) {
    dev->live = enable;
    if (enable) {
        fprintf(stderr, "[W5500] Live networking enabled\n");
    }
}

/* ========================================================================
 * pico-eth board variant (WIZnet W5500-EVB-Pico)
 *
 * Separate SPI board: W5500 on SPI0 (SCK18/MOSI19/MISO16, SPI mode 0/3)
 * with CSn=GPIO17, RSTn=GPIO20, INTn=GPIO21. Zero cost when off: the
 * hot hooks (gpio_write notify, poll) return on a single disabled-flag
 * test; no SPI device is attached; no sockets are polled.
 *
 * CS handling: real firmware bit-bangs CSn as a GPIO. The PL022 device
 * callback model has no GPIO line, so the board watches GPIO17 from
 * gpio_write32's tail call and mirrors it into w5500_spi_cs().
 * RST handling: GPIO20 low holds the W5500 in reset (registers cleared
 * on live edge via a guest-visible reset pulse).
 * INT handling: GPIO21 is emulated as an input pulled high; any socket
 * IR bit set drives it low (active-low). Socket IR clears must
 * recompute the line via w5500_board_update_int().
 * ======================================================================== */

#include "spi.h"
#include "gpio.h"

static w5500_t w5500_board_dev_state;
static int w5500_board_on = 0;
static int w5500_board_spi_num = W5500_BOARD_SPI_DEFAULT;

/* Static INTn refresh used by the shared register paths above. */
static void w5500_board_refresh_int(void) {
    if (!w5500_board_on) return;
    int pending = 0;
    for (int i = 0; i < W5500_NUM_SOCKETS; i++) {
        if (w5500_board_dev_state.sockets[i].regs[W5500_Sn_IR]) {
            pending = 1;
            break;
        }
    }
    if (w5500_board_dev_state.common[W5500_IR]) pending = 1;
    gpio_set_direction(W5500_BOARD_INT_PIN, 0);
    gpio_mark_driven(W5500_BOARD_INT_PIN);
    gpio_set_input_pin(W5500_BOARD_INT_PIN, pending ? 0 : 1);
}

int w5500_board_enabled(void) { return w5500_board_on; }
int w5500_board_spi(void) { return w5500_board_spi_num; }
w5500_t *w5500_board_dev(void) { return &w5500_board_dev_state; }

void w5500_board_update_int(void) {
    if (!w5500_board_on) return;
    int pending = 0;
    for (int i = 0; i < W5500_NUM_SOCKETS; i++) {
        if (w5500_board_dev_state.sockets[i].regs[W5500_Sn_IR]) {
            pending = 1;
            break;
        }
    }
    if (w5500_board_dev_state.common[W5500_IR]) pending = 1;
    /* INTn is active-low: asserted = input low, idle = input high. */
    gpio_set_direction(W5500_BOARD_INT_PIN, 0);
    gpio_mark_driven(W5500_BOARD_INT_PIN);
    gpio_set_input_pin(W5500_BOARD_INT_PIN, pending ? 0 : 1);
}

/* Edge memory: the GPIO watch fires on EVERY gpio_write32 (it always
 * reports both pins), so level-triggered RST handling would re-init the
 * chip on every unrelated CS toggle (RST reads high every time). CS has
 * the same hazard in reverse: re-asserting mid-frame would reset the
 * SPI frame state machine without the line ever toggling. Both are
 * edge-triggered: only act when the level actually changed. Idle = 1. */
static int w5500_board_prev_cs = 1;
static int w5500_board_prev_rst = 1;

void w5500_board_gpio_write(uint32_t pin, uint32_t value) {
    /* No enabled-guard: gpio.c already gates callers on
     * w5500_board_enabled(). Harmless when off: the board device is
     * unreachable with no SPI slot attached.
     * RV32 path: the RV32 SIO handler (rv_membus.c) reports CS/RST
     * transitions directly (shared gpio_write32 never sees RV32 SIO
     * writes), so accept those reports too — dedup via edge memory. */
    value = value ? 1 : 0;
    if (pin == W5500_BOARD_CS_PIN) {
        if (value == w5500_board_prev_cs) return;
        w5500_board_prev_cs = value;
        /* level 0 = CS asserted (active low) */
        w5500_spi_cs(&w5500_board_dev_state, value ? 0 : 1);
    } else if (pin == W5500_BOARD_RST_PIN) {
        if (value == w5500_board_prev_rst) return;
        w5500_board_prev_rst = value;
        if (!value) {
            /* RSTn falling edge: hold in reset — clear volatile state. */
            memset(&w5500_board_dev_state.common, 0,
                   sizeof(w5500_board_dev_state.common));
            for (int i = 0; i < W5500_NUM_SOCKETS; i++)
                memset(w5500_board_dev_state.sockets[i].regs, 0,
                       sizeof(w5500_board_dev_state.sockets[i].regs));
            w5500_board_dev_state.phase = W5500_PHASE_ADDR_HI;
        } else {
            /* RSTn rising edge: re-init defaults, keep live flag. */
            int live = w5500_board_dev_state.live;
            w5500_init(&w5500_board_dev_state);
            w5500_board_dev_state.live = live;
            w5500_board_update_int();
        }
    }
}

void w5500_board_poll(void) {
    if (!w5500_board_on) return;  /* zero-cost guard when off */
    if (w5500_board_dev_state.live)
        w5500_poll(&w5500_board_dev_state);
    w5500_board_update_int();
}

void w5500_board_attach(int spi_num, int live) {
    if (spi_num < 0 || spi_num > 1) spi_num = W5500_BOARD_SPI_DEFAULT;
    w5500_init(&w5500_board_dev_state);
    w5500_board_dev_state.live = live ? 1 : 0;
    w5500_board_spi_num = spi_num;
    spi_attach_device(spi_num, w5500_spi_xfer, w5500_spi_cs,
                      &w5500_board_dev_state);
    /* Board power-on state: CS high (idle), RST high (run), INT idle high.
     * Real firmware runs gpio_init + gpio_set_dir(OUT) before gpio_put,
     * so claim OE + drive the idle levels through the normal SIO path
     * (which also notifies the CS/RST watch). CS starts deasserted. */
    w5500_board_on = 1;
    /* INTn is an INPUT to the SoC (driven by the W5500 die): direction
     * IN, idle level high via the input latch. Do NOT touch OE here —
     * gpio_set_direction(pin, 0) only clears OE (fine), but never set
     * OE=1 on this pin or the guest's SIO_GPIO_IN read returns the
     * stale OUT latch instead of the driven INT level (Arduino
     * digitalRead(21) then sticks high and ioLibrary DHCP never sees
     * the OFFER). */
    gpio_set_direction(W5500_BOARD_INT_PIN, 0);
    gpio_mark_driven(W5500_BOARD_INT_PIN);
    gpio_set_input_pin(W5500_BOARD_INT_PIN, 1);
    /* Claim OE first (syncs IN latch to OUT=0), then drive idle-high.
     * Two steps because OE_SET syncs IN:=OUT for newly-enabled pins:
     * doing it in one OE_SET|OUT_SET pair would sample OUT before the
     * OUT_SET lands. Order matters; each step goes through gpio_write32
     * so the CS/RST watch sees every transition. */
    gpio_write32(SIO_BASE_GPIO + 0x24,
                 (1u << W5500_BOARD_CS_PIN) | (1u << W5500_BOARD_RST_PIN));
    gpio_write32(SIO_BASE_GPIO + 0x14,
                 (1u << W5500_BOARD_CS_PIN) | (1u << W5500_BOARD_RST_PIN));
    /* Sync edge memory with the driven idle levels (both notifications
     * above carried level 1, so prev state is already 1/1 — but the
     * static starts at 1/1 anyway; belt and suspenders after re-attach). */
    w5500_board_prev_cs = 1;
    w5500_board_prev_rst = 1;
    w5500_spi_cs(&w5500_board_dev_state, 0);
    w5500_board_update_int();
    fprintf(stderr, "[W5500] pico-eth board on SPI%d (CS17/RST20/INT21)%s\n",
            spi_num, live ? " live" : " (stub)");
}

void w5500_board_detach(void) {
    if (!w5500_board_on) return;
    w5500_board_on = 0;
    gpio_unmark_driven(W5500_BOARD_INT_PIN);
    /* Close any live host sockets, then clear the SPI slot only if we
     * still own it (a later -sdcard/-emmc attach may have replaced it). */
    w5500_board_dev_state.live = 0;
    for (int i = 0; i < W5500_NUM_SOCKETS; i++) {
        w5500_socket_t *s = &w5500_board_dev_state.sockets[i];
        if (s->host_fd >= 0) { close(s->host_fd); s->host_fd = -1; }
        if (s->host_listen_fd >= 0) { close(s->host_listen_fd); s->host_listen_fd = -1; }
    }
    if (w5500_board_spi_num >= 0 && w5500_board_spi_num <= 1 &&
        spi_state[w5500_board_spi_num].device.ctx == &w5500_board_dev_state &&
        spi_state[w5500_board_spi_num].device.xfer == w5500_spi_xfer) {
        spi_state[w5500_board_spi_num].device.xfer = NULL;
        spi_state[w5500_board_spi_num].device.cs = NULL;
        spi_state[w5500_board_spi_num].device.ctx = NULL;
    }
}

void w5500_board_reattach(void) {
    if (!w5500_board_on) return;
    spi_attach_device(w5500_board_spi_num, w5500_spi_xfer, w5500_spi_cs,
                      &w5500_board_dev_state);
}

void w5500_board_set_live(int live) {
    if (!w5500_board_on) return;
    w5500_board_dev_state.live = live ? 1 : 0;
}

#ifdef __EMSCRIPTEN__
/* Push proxy-received bytes into socket RX buffer (called from JS via export).
 * Sets RECV interrupt like native recv path. */
int bramble_w5500_dev_push_rx(w5500_t *dev, int sock, const uint8_t *data, int len) {
    if (!dev || sock < 0 || sock >= W5500_NUM_SOCKETS || !data || len <= 0) return -1;
    w5500_socket_t *s = &dev->sockets[sock];
    uint16_t rx_rsr = ((uint16_t)s->regs[W5500_Sn_RX_RSR0] << 8) |
                      s->regs[W5500_Sn_RX_RSR0 + 1];
    uint16_t free_space = W5500_RX_BUF_SIZE - rx_rsr;
    if (free_space == 0) return 0;
    if (len > free_space) len = free_space;
    uint16_t rx_wr = ((uint16_t)s->regs[W5500_Sn_RX_WR0] << 8) |
                     s->regs[W5500_Sn_RX_WR0 + 1];
    for (int i = 0; i < len; i++)
        s->rx_buf[(rx_wr + (uint16_t)i) % W5500_RX_BUF_SIZE] = data[i];
    rx_wr = (uint16_t)(rx_wr + (uint16_t)len);
    s->regs[W5500_Sn_RX_WR0]     = (rx_wr >> 8) & 0xFF;
    s->regs[W5500_Sn_RX_WR0 + 1] = rx_wr & 0xFF;
    rx_rsr = (uint16_t)(rx_rsr + (uint16_t)len);
    s->regs[W5500_Sn_RX_RSR0]     = (rx_rsr >> 8) & 0xFF;
    s->regs[W5500_Sn_RX_RSR0 + 1] = rx_rsr & 0xFF;
    s->regs[W5500_Sn_IR] |= 0x04;
    return len;
}
/* Default device for JS-friendly export (wraps wasm_w5500 in bramble_wasm.c) */
int bramble_w5500_dev_push_status(w5500_t *dev, int sock, int code) {
    if (!dev || sock < 0 || sock >= W5500_NUM_SOCKETS) return -1;
    w5500_socket_t *s = &dev->sockets[sock];
    if (code) {
        if (s->regs[W5500_Sn_SR] == W5500_SOCK_INIT ||
            s->regs[W5500_Sn_SR] == W5500_SOCK_LISTEN ||
            s->regs[W5500_Sn_SR] == W5500_SOCK_CLOSED) {
            s->regs[W5500_Sn_SR] = W5500_SOCK_ESTABLISHED;
        }
        s->regs[W5500_Sn_IR] |= 0x01; /* CON */
    } else {
        s->regs[W5500_Sn_SR] = W5500_SOCK_CLOSED;
        s->regs[W5500_Sn_IR] |= 0x02; /* DISCON */
    }
    return 0;
}
#endif
