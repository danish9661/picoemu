#ifndef W5500_H
#define W5500_H

#include <stdint.h>

/* ========================================================================
 * W5500 Ethernet Controller (SPI Device Plugin)
 *
 * Emulates the WIZnet W5500 hardwired TCP/IP Ethernet controller.
 * Communicates via SPI using variable-length data frames with a
 * 3-byte header (2 address + 1 control).
 *
 * Attaches to an SPI bus via spi_attach_device().
 *
 * SPI frame format:
 *   Byte 0-1: 16-bit offset address (big-endian)
 *   Byte 2:   Control byte - BSB[4:0] | RW | OM[1:0]
 *             RW: 0=read, 1=write
 *             OM: 00=VDM (variable), 01=FDM 1B, 10=FDM 2B, 11=FDM 4B
 *   Byte 3+:  Data phase (read returns register, write stores)
 *
 * Block Select Byte (BSB) mapping:
 *   0       = Common registers
 *   1,5,9,13,17,21,25,29  = Socket 0-7 registers
 *   2,6,10,14,18,22,26,30 = Socket 0-7 TX buffer
 *   3,7,11,15,19,23,27,31 = Socket 0-7 RX buffer
 *
 * Usage:
 *   w5500_t w5500;
 *   w5500_init(&w5500);
 *   spi_attach_device(spi_num, w5500_spi_xfer, w5500_spi_cs, &w5500);
 * ======================================================================== */

#define W5500_NUM_SOCKETS   8
#define W5500_TX_BUF_SIZE   2048    /* Per socket */
#define W5500_RX_BUF_SIZE   2048    /* Per socket */

/* Common register offsets */
#define W5500_MR        0x0000  /* Mode register */
#define W5500_GAR0      0x0001  /* Gateway address (4 bytes) */
#define W5500_SUBR0     0x0005  /* Subnet mask (4 bytes) */
#define W5500_SHAR0     0x0009  /* Source MAC address (6 bytes) */
#define W5500_SIPR0     0x000F  /* Source IP address (4 bytes) */
#define W5500_IR        0x0015  /* Interrupt register */
#define W5500_IMR       0x0016  /* Interrupt mask */
#define W5500_SIR       0x0017  /* Socket interrupt */
#define W5500_SIMR      0x0018  /* Socket interrupt mask */
#define W5500_RTR0      0x0019  /* Retry time (2 bytes) */
#define W5500_RCR       0x001B  /* Retry count */
#define W5500_PHYCFGR   0x002E  /* PHY configuration */
#define W5500_VERSIONR  0x0039  /* Chip version (read-only, 0x04) */

/* Common register block size */
#define W5500_COMMON_REG_SIZE   0x0040

/* Socket register offsets (within each socket register block) */
#define W5500_Sn_MR     0x0000  /* Socket mode */
#define W5500_Sn_CR     0x0001  /* Socket command */
#define W5500_Sn_IR     0x0002  /* Socket interrupt */
#define W5500_Sn_SR     0x0003  /* Socket status */
#define W5500_Sn_PORT0  0x0004  /* Source port (2 bytes) */
#define W5500_Sn_DHAR0  0x0006  /* Dest MAC (6 bytes) */
#define W5500_Sn_DIPR0  0x000C  /* Dest IP (4 bytes) */
#define W5500_Sn_DPORT0 0x0010  /* Dest port (2 bytes) */
#define W5500_Sn_MSSR0  0x0012  /* Max segment size (2 bytes) */
#define W5500_Sn_TOS    0x0015  /* TOS */
#define W5500_Sn_TTL    0x0016  /* TTL */
#define W5500_Sn_RXBUF_SIZE 0x001E  /* RX buffer size */
#define W5500_Sn_TXBUF_SIZE 0x001F  /* TX buffer size */
#define W5500_Sn_TX_FSR0    0x0020  /* TX free size (2 bytes) */
#define W5500_Sn_TX_RD0     0x0022  /* TX read pointer (2 bytes) */
#define W5500_Sn_TX_WR0     0x0024  /* TX write pointer (2 bytes) */
#define W5500_Sn_RX_RSR0    0x0026  /* RX received size (2 bytes) */
#define W5500_Sn_RX_RD0     0x0028  /* RX read pointer (2 bytes) */
#define W5500_Sn_RX_WR0     0x002A  /* RX write pointer (2 bytes) */
#define W5500_Sn_IMR    0x002C  /* Socket interrupt mask */

/* Socket register block size */
#define W5500_SOCKET_REG_SIZE   0x0030

/* Socket status values */
#define W5500_SOCK_CLOSED   0x00
#define W5500_SOCK_INIT     0x13
#define W5500_SOCK_LISTEN   0x14
#define W5500_SOCK_SYNSENT  0x15   /* M15: non-blocking connect in progress */
#define W5500_SOCK_ESTABLISHED 0x17
#define W5500_SOCK_UDP      0x22
#define W5500_SOCK_MACRAW   0x42

/* Socket commands */
#define W5500_CMD_OPEN      0x01
#define W5500_CMD_LISTEN    0x02
#define W5500_CMD_CONNECT   0x04
#define W5500_CMD_CLOSE     0x08
#define W5500_CMD_SEND      0x20
#define W5500_CMD_RECV      0x40

/* Socket modes */
#define W5500_MR_TCP    0x01
#define W5500_MR_UDP    0x02
#define W5500_MR_MACRAW 0x04

/* PHYCFGR bits */
#define W5500_PHY_LINK  (1u << 0)   /* Link status */
#define W5500_PHY_SPD   (1u << 1)   /* Speed: 1=100Mbps */
#define W5500_PHY_DPX   (1u << 2)   /* Duplex: 1=full */
#define W5500_PHY_OPMDC (7u << 3)   /* Operation mode */
#define W5500_PHY_RST   (1u << 7)   /* Reset (active low) */

/* SPI frame phase */
typedef enum {
    W5500_PHASE_ADDR_HI,    /* Collecting address byte 1 */
    W5500_PHASE_ADDR_LO,    /* Collecting address byte 2 */
    W5500_PHASE_CONTROL,    /* Collecting control byte */
    W5500_PHASE_DATA,       /* Data transfer phase */
} w5500_phase_t;

/* Per-socket state */
typedef struct {
    uint8_t regs[W5500_SOCKET_REG_SIZE];
    uint8_t tx_buf[W5500_TX_BUF_SIZE];
    uint8_t rx_buf[W5500_RX_BUF_SIZE];
    uint16_t rx_base;     /* MACRAW RX stream base: ring offset of the
                           * oldest unconsumed [len+frame] entry. Real
                           * silicon advances an internal read pointer on
                           * RECV while RX_RD stays guest-writable, so the
                           * model tracks them separately (see w5500.c). */
    uint16_t rx_cursor;      /* Per-CS-frame read offset: bytes consumed
                              * in the current SPI frame (reset on CS). */
    uint16_t rx_cursor_base; /* VDM address of the frame's first DATA
                              * byte: the guest's cursor anchor. */
    uint8_t  rx_cursor_valid;/* Set after the first DATA byte. */
    int     host_fd;        /* Host socket fd for live networking (-1 if none) */
    int     host_listen_fd; /* Host listen fd for TCP server (-1 if none) */
} w5500_socket_t;

/* W5500 device state */
typedef struct {
    /* Common registers */
    uint8_t common[W5500_COMMON_REG_SIZE];

    /* Socket state */
    w5500_socket_t sockets[W5500_NUM_SOCKETS];

    /* SPI frame state machine */
    w5500_phase_t phase;
    uint16_t addr;          /* Current offset address */
    uint8_t  bsb;           /* Block select bits [4:0] (socket/block) */
    uint8_t  ctl;           /* Full control byte (kept for tracing) */
    int      rw;            /* 0=read, 1=write */
    int      cs_active;     /* Chip select state */

    /* Live networking mode */
    int      live;          /* 1 = host sockets enabled, 0 = stub only */
    int      vnet_port;     /* vnet port index (-1 if not registered) */
} w5500_t;

/* Initialize W5500 device with default register values */
void w5500_init(w5500_t *dev);

/* SPI transfer callback (for spi_attach_device) */
uint8_t w5500_spi_xfer(void *ctx, uint8_t mosi);

/* SPI chip-select callback */
void w5500_spi_cs(void *ctx, int cs_active);

/* Poll host sockets for incoming data (call from main loop when live=1) */
void w5500_poll(w5500_t *dev);

/* Enable live networking mode (creates real host sockets) */
void w5500_set_live(w5500_t *dev, int enable);

/* WASM proxy pump: drain queued CONNECT/LISTEN/CLOSE/SEND bytes into
 * out[] (up to maxlen). Returns bytes drained, 0 when empty. JS pumps
 * this each frame/tick to the proxy socket (same pairing as
 * bramble_eth_pop_tx / bramble_bt_hci_pop_tx). Always linked (queue is
 * plain C); only non-empty when live WASM traffic queued. */
int bramble_w5500_pop_tx(uint8_t *out, int maxlen);
/* Queued proxy bytes waiting (for pump loop budgeting). */
int bramble_w5500_tx_len(void);
/* Queue raw proxy bytes (w5500.c command path + wasm_net.c compat shim). */
void w5500_ws_tx_push(const uint8_t *data, int len);

/* Single-gateway MACRAW path: attach socket `sock` of `dev` to the shared
 * vnet bus (same bus/gateway as CYW43). Idempotent per (dev,sock). */
int w5500_macraw_attach(w5500_t *dev, int sock);
/* Single-gateway switch (default ON): MACRAW joins the shared vnet bus.
 * 0 isolates Ethernet from WiFi/gateway traffic (debug only). */
void w5500_gw_enable_set(int on);
int w5500_gw_enabled(void);

/* ========================================================================
 * pico-eth board variant (WIZnet W5500-EVB-Pico, RP2040)
 * pico-eth2 board variant (WIZnet W5500-EVB-Pico2, RP2350)
 *
 * Separate SPI board: W5500 on SPI0 (SCK18/MOSI19/MISO16) with
 * CSn=GPIO17, RSTn=GPIO20, INTn=GPIO21. Both boards use IDENTICAL
 * wiring/pins (verified against WIZnet docs + Zephyr DTS + MicroPython
 * board ports); only the SoC differs (RP2040 vs RP2350). One model
 * serves both: on RP2350 (M33/RV32) the RP2350 SPI bases
 * (0x40080000/0x40088000) route to the same SPI instances (spi_match
 * is RP2350-aware, same as UART). Zero cost when off: a single
 * disabled-flag branch in the GPIO/poll hooks, no SPI overhead (no
 * device attached), no sockets polled.
 *
 * Usage (native):
 *   ./bramble fw.uf2 -board pico-eth            # stub (instant ESTABLISHED)
 *   ./bramble fw.uf2 -board pico-eth2           # same model, RP2350 label
 *   ./bramble fw.uf2 -board pico-eth -net-live  # real host TCP/UDP sockets
 * Usage (WASM):
 *   bramble_board_eth(1, live, 0)
 * ======================================================================== */

#define W5500_BOARD_SPI_DEFAULT  0
#define W5500_BOARD_CS_PIN       17
#define W5500_BOARD_RST_PIN      20
#define W5500_BOARD_INT_PIN      21

/* Attach the board to an SPI bus (init + spi_attach + INT drive). */
void w5500_board_attach(int spi_num, int live);
/* Detach (clear SPI slot if ours, close live fds). */
void w5500_board_detach(void);
/* Re-attach SPI callback after spi_init clears it (watchdog reboot).
 * Preserves device state (no re-init). */
void w5500_board_reattach(void);
/* 1 when the pico-eth board is on, 0 otherwise (zero-cost guard). */
int w5500_board_enabled(void);
/* Attached SPI bus (valid only when enabled). */
int w5500_board_spi(void);
/* Board device instance (valid only when enabled). */
w5500_t *w5500_board_dev(void);
/* Set live flag without re-init (UI toggle). */
void w5500_board_set_live(int live);
/* Guest GPIO OUT change hook (CSn/RSTn). value is the pin level 0/1. */
void w5500_board_gpio_write(uint32_t pin, uint32_t value);
/* Recompute INTn from socket IR state (active-low). */
void w5500_board_update_int(void);
/* Poll live sockets (returns immediately when off). */
void w5500_board_poll(void);

#endif /* W5500_H */
