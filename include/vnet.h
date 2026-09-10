#ifndef VNET_H
#define VNET_H

#include <stdint.h>

/* ========================================================================
 * Bramble Virtual Network Bus
 *
 * Central routing layer for all emulated Ethernet traffic. Device models
 * (CYW43, W5500, software-defined devices) register as ports. Incoming
 * frames from any port are delivered to all other ports and to the host
 * TAP interface (if attached).
 *
 * Usage:
 *   -net                     Create TAP + NAT in one step (auto-sudo)
 *   -net-peer <path>         Mesh with another Bramble instance
 *
 * Architecture:
 *   ┌─────────┐  ┌─────────┐  ┌───────┐  ┌──────────┐
 *   │  CYW43  │  │  W5500  │  │  SDD  │  │ Peer(s)  │
 *   └────┬────┘  └────┬────┘  └───┬───┘  └────┬─────┘
 *        │            │           │            │
 *        └──────┬─────┴─────┬─────┘            │
 *               │   vnet    │                  │
 *               │  router   ├──────────────────┘
 *               │           │
 *               └─────┬─────┘
 *                     │
 *               ┌─────┴─────┐
 *               │  Host TAP │
 *               └───────────┘
 * ======================================================================== */

#define VNET_MAX_PORTS     8
#define VNET_MAX_PEERS     4
#define VNET_MAX_FRAME   1522    /* Ethernet max + VLAN tag headroom */
#define VNET_MAC_LEN        6

/* Default virtual subnet (same as tapif.c) */
#define VNET_HOST_IP       "192.168.4.1"
#define VNET_GUEST_IP      "192.168.4.100"
#define VNET_SUBNET        "192.168.4.0/24"
#define VNET_NETMASK       "255.255.255.0"
#define VNET_TAP_NAME      "bramble0"

/* Port types for logging/debug */
typedef enum {
    VNET_PORT_CYW43,
    VNET_PORT_W5500,
    VNET_PORT_SDD,
    VNET_PORT_CUSTOM,
} vnet_port_type_t;

/* Frame receive callback: called when a frame arrives for this port */
typedef void (*vnet_rx_fn)(void *ctx, const uint8_t *frame, int len);

/* Registered port (device) */
typedef struct {
    int active;
    vnet_port_type_t type;
    char name[32];
    uint8_t mac[VNET_MAC_LEN];
    vnet_rx_fn rx_fn;
    void *ctx;
} vnet_port_t;

/* Peer connection (another Bramble instance) */
typedef struct {
    int fd;                     /* Connected socket fd (-1 if unused) */
    int listen_fd;              /* Listening socket fd (-1 if client) */
    int owns_file;              /* We bound the socket file (safe to unlink) */
    char path[256];             /* Unix socket path */
    uint8_t rx_buf[4 + VNET_MAX_FRAME]; /* Length-prefixed frame buffer */
    int rx_len;                 /* Bytes received so far */
} vnet_peer_t;

/* Global vnet state */
typedef struct {
    int enabled;                /* 1 if vnet is active */

    /* TAP interface */
    int tap_fd;                 /* TAP file descriptor (-1 if none) */
    char tap_name[32];          /* TAP interface name */

    /* Registered device ports */
    vnet_port_t ports[VNET_MAX_PORTS];
    int port_count;

    /* Peer connections */
    vnet_peer_t peers[VNET_MAX_PEERS];
    int peer_count;

    /* Default gateway MAC for ARP responses */
    uint8_t gateway_mac[VNET_MAC_LEN];

    /* Statistics */
    uint32_t frames_tx;
    uint32_t frames_rx;
    uint32_t frames_peer_tx;
    uint32_t frames_peer_rx;
} vnet_state_t;

extern vnet_state_t vnet;

/* ======================================================================== */
/* Lifecycle                                                                 */
/* ======================================================================== */

/* Initialize the vnet bus (call after argument parsing) */
int  vnet_init(void);

/* Cleanup: close TAP, disconnect peers, free resources */
void vnet_cleanup(void);

/* ======================================================================== */
/* TAP Bridge                                                                */
/* ======================================================================== */

/* Attach a TAP interface and configure IP + NAT.
 * If name is NULL, uses VNET_TAP_NAME ("bramble0").
 * Returns 0 on success, -1 on error. */
int  vnet_attach_tap(const char *name);

/* ======================================================================== */
/* Port Registration                                                         */
/* ======================================================================== */

/* Register a device port. Returns port index or -1 on error.
 * The rx_fn callback is invoked when a frame arrives for this port.
 * If mac is NULL, a locally-administered MAC is auto-assigned. */
int  vnet_register_port(const char *name, vnet_port_type_t type,
                        const uint8_t *mac, vnet_rx_fn rx_fn, void *ctx);

/* Unregister a port by index */
void vnet_unregister_port(int port_idx);

/* ======================================================================== */
/* Frame Transmission                                                        */
/* ======================================================================== */

/* Send an Ethernet frame from the given port.
 * The frame is delivered to:
 *   - All other registered ports (if MAC matches or broadcast)
 *   - The TAP interface (if attached)
 *   - All connected peers
 * src_port: the port index that is sending (-1 for TAP/peer origin) */
void vnet_tx_frame(int src_port, const uint8_t *frame, int len);

/* WS gateway uplink mirror (WASM): called for every transmitted frame
 * when non-NULL. The owner must avoid re-injecting gateway-originated
 * frames (see bramble_wasm.c eth_from_gateway guard). */
typedef void (*vnet_mirror_fn)(const uint8_t *frame, int len);
extern vnet_mirror_fn vnet_ws_mirror;

/* ======================================================================== */
/* Polling                                                                   */
/* ======================================================================== */

/* Poll TAP and peer sockets for incoming frames, deliver to ports.
 * Call from main loop at regular intervals. */
void vnet_poll(void);

/* ======================================================================== */
/* Peer Mesh                                                                 */
/* ======================================================================== */

/* Add a peer connection (Unix socket path).
 * Like wire.c: first process creates socket, second connects. */
int  vnet_add_peer(const char *path);

/* ======================================================================== */
/* Utility                                                                   */
/* ======================================================================== */

/* Generate a locally-administered MAC address from an index */
void vnet_generate_mac(uint8_t *mac, int index);

/* Print vnet statistics */
void vnet_report_stats(void);

#endif /* VNET_H */
