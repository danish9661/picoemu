#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include "cyw43.h"
#include "tapif.h"
#include "emulator.h"
#include "gpio.h"
#include "vnet.h"

/* -nodhcp: bridge DHCP to vnet/TAP instead of the fake server. */
int cyw43_no_fake_dhcp = 0;

/* -mac preset (survives cyw43_reset, which restores defaults). */
static uint8_t preset_mac[6];
static int has_preset_mac = 0;
static void cyw43_ap_set_up(int up);
static void cyw43_del_scan_result(const char *ssid);void cyw43_set_mac(const uint8_t mac[6]) {
    memcpy(preset_mac, mac, 6);
    has_preset_mac = 1;
    memcpy(cyw43.mac_addr, mac, 6);
}

/* Env-gated trace (BRAMBLE_CYW43_TRACE=1): CYW43-only logging at full speed,
 * without the crushing overhead of -debug (CPU step tracing). */
static int cyw43_trace_en(void) {
    static int en = -1;
    if (en < 0) en = getenv("BRAMBLE_CYW43_TRACE") ? 1 : 0;
    return en;
}
#define CYW43_DBG (cpu.debug_enabled || cyw43_trace_en())

/* CYW43439 chip ID */
#define CYW43439_CHIP_ID 0x00A9A6A7

/* WiFi GPIO pin assignments (Pico W) */
#define WL_CS         23
#define WL_CLK        24
#define WL_DIO        25
/* WL_HOST_WAKE = GPIO 24 (shared with CLK/DIO in different phases).
 * Active HIGH: CYW43 asserts this to tell RP2040 that data is available.
 * cyw43_ll.c checks gpio_get(24) != 0 before polling the interrupt register. */
#define WL_HOST_WAKE  24

/* Core wrapper full addresses (BASE + WRAPPER_REGISTER_OFFSET + register_offset) */
/* WLAN ARM CM3: 0x18003000 + 0x100000 = 0x18103000 */
#define CYW43_WLAN_RESETCTRL  0x18103800u  /* WLAN ARM + WRAPPER + AI_RESETCTRL (0x800) */
#define CYW43_WLAN_IOCTRL     0x18103408u  /* WLAN ARM + WRAPPER + AI_IOCTRL (0x408) */
/* SOCRAM: 0x18004000 + 0x100000 = 0x18104000 */
#define CYW43_SOCRAM_RESETCTRL 0x18104800u
#define CYW43_SOCRAM_IOCTRL    0x18104408u
#define CYW43_AIRC_RESET       0x01u

/* SPI_STATUS_REGISTER (0x0008) bits */
#define SPI_STATUS_F2_RX_READY      0x00000020u
#define SPI_STATUS_F2_PKT_AVAILABLE 0x00000100u
#define SPI_STATUS_F2_PKT_LEN_SHIFT 9

cyw43_state_t cyw43;

/* Default MAC address (locally administered) */
static const uint8_t default_mac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};

/* ========================================================================
 * RX Queue Management
 * ======================================================================== */

static int rx_queue_count(void) {
    return (cyw43.rx_head - cyw43.rx_tail + CYW43_RX_QUEUE_SIZE) % CYW43_RX_QUEUE_SIZE;
}

/* Drive WL_HOST_WAKE (GPIO 24) HIGH when data is queued, LOW when empty.
 * cyw43_ll.c's sdpcm_poll_device checks gpio_get(WL_HOST_WAKE) == 1 before
 * reading the SPI interrupt register, so we must assert this whenever there
 * is a frame waiting in rx_queue. BT shares the same wake line: pending
 * BT->host bytes assert it too, so the poll path picks up HCI events. */
static int cyw43_bt_pending(void) {
    uint32_t out = (uint32_t)cyw43.bt_ram[0x200C] |
                   ((uint32_t)cyw43.bt_ram[0x200D] << 8) |
                   ((uint32_t)cyw43.bt_ram[0x200E] << 16) |
                   ((uint32_t)cyw43.bt_ram[0x200F] << 24);
    return ((cyw43.bt_b2h_in - out) & 0xFFF) != 0;
}

static void cyw43_update_irq(void) {
    /* GPIO 24 is WL_DIO (SPI data, output during TX) shared with WL_HOST_WAKE
     * (input when idle). The PIO program does "set pindirs, 0" after RX to
     * switch it back to input, but since we intercept the FIFO without running
     * instructions, we must do this ourselves so gpio_get(24) returns our IRQ
     * state rather than the stale PIO output direction. */
    gpio_set_direction(WL_HOST_WAKE, 0);  /* 0 = input */
    /* Shared wake line: WiFi RX frames AND pending BT->host bytes both
     * assert it (level: re-evaluated on B2H consume + RX pop). The BT
     * consume path (cyw43_poll -> cyw43_ll_bt_has_work reads SDIO_INT_STATUS
     * -> cyw43_bluetooth_hci_process) is only reached via this IRQ, so BT
     * answers need the line too. The old "level storm" fear does not apply:
     * the guest IRQ handler disables the line until CYW43_POST_POLL_HOOK
     * re-enables it (mpnetworkport.c), so each assertion fires exactly once
     * per poll cycle. (A 2026-09-13 experiment that OR-ed bt_pending here
     * coincided with "no guest traffic", but that build also carried the
     * broken bulk-write trigger below; with the trigger fixed the line is
     * required — without it B2H CCs sit queued forever and the guest
     * repeats 0c03 x8.) */
    int val = (rx_queue_count() > 0 || cyw43_bt_pending()) ? 1 : 0;
    if (CYW43_DBG)
        fprintf(stderr, "[CYW43] update_irq: GPIO24=%d (q=%d btpend=%d)\n",
                val, rx_queue_count(), cyw43_bt_pending());
    gpio_set_input_pin(WL_HOST_WAKE, val);
}

static int rx_queue_push(const uint8_t *data, int len) {
    if (rx_queue_count() >= CYW43_RX_QUEUE_SIZE - 1) return -1;
    if (len > CYW43_MAX_FRAME_SIZE) return -1;

    cyw43_rx_frame_t *f = &cyw43.rx_queue[cyw43.rx_head];
    memcpy(f->data, data, len);
    f->len = len;
    cyw43.rx_head = (cyw43.rx_head + 1) % CYW43_RX_QUEUE_SIZE;
    cyw43_update_irq();
    return 0;
}

static cyw43_rx_frame_t *rx_queue_peek(void) {
    if (rx_queue_count() == 0) return NULL;
    return &cyw43.rx_queue[cyw43.rx_tail];
}

static void rx_queue_pop(void) {
    if (rx_queue_count() > 0) {
        cyw43.rx_tail = (cyw43.rx_tail + 1) % CYW43_RX_QUEUE_SIZE;
        cyw43_update_irq();
    }
}

/* ========================================================================
 * SDPCM Frame Construction Helpers
 * ======================================================================== */

static void sdpcm_fill_header(uint8_t *buf, int total_size, uint8_t channel, uint8_t hdr_len) {
    sdpcm_header_t *h = (sdpcm_header_t *)buf;
    memset(h, 0, sizeof(*h));
    h->size = (uint16_t)total_size;
    h->size_com = ~h->size;
    h->sequence = cyw43.tx_seq++;
    h->channel_and_flags = channel;
    h->header_length = hdr_len;
    h->bus_data_credit = (uint8_t)(cyw43.last_fw_seq + 20);
}

/* Build an IOCTL response and queue it */
static void cyw43_queue_ioctl_response(uint32_t cmd, uint16_t ioctl_id,
                                        const uint8_t *payload, int payload_len,
                                        uint32_t status) {
    uint8_t frame[CYW43_MAX_FRAME_SIZE];
    int offset = 0;

    /* SDPCM header (12 bytes) - control channel, no pad */
    int total = 12 + 16 + payload_len;
    sdpcm_fill_header(frame, total, SDPCM_CONTROL_CHANNEL, 12);
    offset = 12;

    /* CDC header (16 bytes) */
    cdc_header_t *cdc = (cdc_header_t *)(frame + offset);
    cdc->cmd = cmd;
    cdc->len = (uint32_t)payload_len;
    cdc->flags = ((uint32_t)ioctl_id << CDC_ID_SHIFT) | CDC_REPLY;
    cdc->status = status;
    offset += 16;

    /* Payload */
    if (payload && payload_len > 0) {
        memcpy(frame + offset, payload, payload_len);
        offset += payload_len;
    }

    rx_queue_push(frame, offset);
}

/* Build an async event and queue it.
 *
 * Frame layout (offsets relative to BDC payload start = "payload"):
 *   payload[0..13]  : Ethernet header (14 bytes)
 *   payload[14..23] : bcmilcp header: subtype(2)+length(2)+pad(1)+OUI(3)+usr_subtype(2)
 *   payload[24..]   : event body = wl_event_msg_t / cyw43_async_event_t layout:
 *                     _0/version(2), flags(2), event_type(4), status(4), reason(4),
 *                     auth_type(4), datalen(4), addr(6), ifname(16), ifidx(1), bsscfgidx(1)
 *
 * SDK passes buf=payload+24 to cyw43_ll_parse_async_event, which does a memmove
 * (alignment fix) copying buf -> buf-2 (payload+22), then casts to cyw43_async_event_t:
 *   ev._0        = payload[22..23] = was at payload[24..25] = version (0x0002)
 *   ev.flags     = payload[24..25] = was at payload[26..27] (BE uint16)
 *   ev.event_type = payload[26..29] = was at payload[28..31] (BE uint32)
 *   ev.status    = payload[30..33] = was at payload[32..35] (BE uint32)
 *
 * NOTE: that comment describes an older layout. The driver actually does
 * buf=payload+24, ev=buf-2 (realignment only, content stays), so ev content
 * starts at payload+24 with NO extra pad. Do not add pad bytes here.
 */
static void cyw43_queue_event_if(uint32_t event_type, uint32_t status,
                                   uint32_t reason, uint16_t flags,
                                   uint8_t ifidx);
static void cyw43_queue_event(uint32_t event_type, uint32_t status,
                               uint32_t reason, uint16_t flags) {
    cyw43_queue_event_if(event_type, status, reason, flags, 0);
}
/* BT HCI ring (defined below; used early by the vnet ADV hook). */
static void cyw43_bt_queue_hci(uint8_t pkt_type, const uint8_t *payload,
                               int paylen);
/* HCI forward drain (defined below with the bridge pump). */
static int bt_hci_drain(void);
/* Internal GATT link layer (defined below with the HCI responder).
 * NOTE: the multi-link table lives below too; the room-RX path only
 * needs link_up/down/is_up at this point (declared non-static). */
static void bt_gatt_db_reset(void);
static void bt_gatt_handle_room_att(const uint8_t *att, int att_len,
                                    const uint8_t *src_mac);
void bt_gatt_link_up(const uint8_t *peer);
void bt_gatt_link_down(void);
int bt_gatt_link_is_up(void);

/* Interface-aware event (ifidx: 0=STA, 1=AP). ev.interface selects the
 * driver's STA join-state vs AP link-up path. */
static void cyw43_queue_event_if(uint32_t event_type, uint32_t status,
                                  uint32_t reason, uint16_t flags,
                                  uint8_t ifidx) {
    uint8_t frame[256];
    int offset = 0;

    /* Reserve space for SDPCM (12) + pad (2) + BDC (4) = 18 bytes */
    offset = 18;

    /* Ethernet header (14 bytes) — payload[0..13] */
    memset(frame + offset, 0xFF, 6);                    /* dst: broadcast */
    memcpy(frame + offset + 6, cyw43.mac_addr, 6);      /* src: device MAC */
    frame[offset + 12] = 0x88;                           /* ethertype: 0x886C */
    frame[offset + 13] = 0x6C;
    offset += 14;

    /* bcmilcp header — payload[14..23] */
    frame[offset++] = 0x00; frame[offset++] = 0x01;     /* subtype = 1 (BE) */
    int len_offset = offset;
    frame[offset++] = 0x00; frame[offset++] = 0x00;     /* length placeholder */
    frame[offset++] = 0x00;                              /* pad byte — payload[18] */
    frame[offset++] = 0x00; frame[offset++] = 0x10; frame[offset++] = 0x18; /* OUI — payload[19-21] */
    frame[offset++] = 0x80; frame[offset++] = 0x02;     /* usr_subtype — payload[22-23] */

    /* Event body — payload[24..] */

    /* ev._0 = version = 0x0002 (BE uint16; SDK ignores this field) */
    frame[offset++] = 0x00;
    frame[offset++] = 0x02;

    /* ev.flags (BE uint16): bit 0 = link-up for EV_LINK — payload[26..27] */
    frame[offset++] = (flags >> 8) & 0xFF;
    frame[offset++] = flags & 0xFF;

    /* ev.event_type (BE u32) — payload[28..31] */
    frame[offset++] = (event_type >> 24) & 0xFF;
    frame[offset++] = (event_type >> 16) & 0xFF;
    frame[offset++] = (event_type >>  8) & 0xFF;
    frame[offset++] = (event_type >>  0) & 0xFF;

    /* ev.status (BE u32) — payload[32..35] */
    frame[offset++] = (status >> 24) & 0xFF;
    frame[offset++] = (status >> 16) & 0xFF;
    frame[offset++] = (status >>  8) & 0xFF;
    frame[offset++] = (status >>  0) & 0xFF;

    /* ev.reason (BE u32) — payload[36..39] */
    frame[offset++] = (reason >> 24) & 0xFF;
    frame[offset++] = (reason >> 16) & 0xFF;
    frame[offset++] = (reason >>  8) & 0xFF;
    frame[offset++] = (reason >>  0) & 0xFF;

    /* ev._1[0..3]: auth_type (BE u32) = 0 — payload[40..43] */
    frame[offset++] = 0; frame[offset++] = 0;
    frame[offset++] = 0; frame[offset++] = 0;

    /* ev._1[4..7]: datalen (BE u32) = 0 */
    frame[offset++] = 0; frame[offset++] = 0;
    frame[offset++] = 0; frame[offset++] = 0;

    /* ev._1[8..13]: addr (6 bytes) = device MAC */
    memcpy(frame + offset, cyw43.mac_addr, 6);
    offset += 6;

    /* ev._1[14..29]: ifname (16 bytes) = "wl0" */
    memset(frame + offset, 0, 16);
    frame[offset] = 'w'; frame[offset+1] = 'l'; frame[offset+2] = '0';
    offset += 16;

    /* ifidx + bsscfgidx (= ev.interface selects STA/AP path) */
    frame[offset++] = ifidx;  /* ifidx */
    frame[offset++] = ifidx;  /* bsscfgidx */

    /* Fill bcmilcp length field: total bytes following the length field */
    uint16_t ev_len = (uint16_t)(offset - len_offset - 2);
    frame[len_offset]     = (ev_len >> 8) & 0xFF;
    frame[len_offset + 1] = ev_len & 0xFF;

    /* Fill SDPCM + pad + BDC at the beginning */
    sdpcm_fill_header(frame, offset, SDPCM_EVENT_CHANNEL, 14);
    frame[12] = 0; frame[13] = 0;  /* 2-byte pad */

    bdc_header_t *bdc = (bdc_header_t *)(frame + 14);
    bdc->flags = 0x20;
    bdc->priority = 0;
    bdc->flags2 = 0;
    bdc->data_offset = 0;

    rx_queue_push(frame, offset);
}

/* Minimal RSN IE (WPA2-PSK, CCMP) for secured scan results */
static const uint8_t escan_rsn_ie[22] = {
    0x30, 0x14, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00,
    0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02,
    0x00, 0x00
};

/* Queue one escan PARTIAL result event + parsing matches the driver's
 * bss overlay (ev->u.scan_result aliases the blob: bssid@[20],
 * ssid_len@[30], ssid@[31], chanspec@[83], rssi@[88]; all LE). */
static void cyw43_queue_escan_result(const cyw43_scan_result_t *r) {
    uint8_t frame[512];
    int off = 0;
    /* Reserve SDPCM(12) + pad(2) + BDC(4) = 18 */
    off = 18;
    /* ETH header (broadcast) */
    memset(frame + off, 0xFF, 6);
    memcpy(frame + off + 6, cyw43.mac_addr, 6);
    frame[off + 12] = 0x88; frame[off + 13] = 0x6C;
    off += 14;
    /* bcmilcp header (mirrors cyw43_queue_event) */
    frame[off++] = 0x00; frame[off++] = 0x01;
    int len_offset = off;
    frame[off++] = 0x00; frame[off++] = 0x00;
    frame[off++] = 0x00;
    frame[off++] = 0x00; frame[off++] = 0x10; frame[off++] = 0x18;
    frame[off++] = 0x80; frame[off++] = 0x02;
    /* ev body starts here (payload+24 == driver ev; see queue_event) */
    frame[off++] = 0x00; frame[off++] = 0x02;
    frame[off++] = 0x00; frame[off++] = 0x00;
    frame[off++] = 0x00; frame[off++] = 0x00; frame[off++] = 0x00; frame[off++] = 69;
    frame[off++] = 0x00; frame[off++] = 0x00; frame[off++] = 0x00; frame[off++] = 8;
    frame[off++] = 0x00; frame[off++] = 0x00; frame[off++] = 0x00; frame[off++] = 0x00;
    memset(frame + off, 0, 4 + 4 + 6 + 16);
    off += 4 + 4 + 6 + 16;
    frame[off++] = 0; frame[off++] = 0;
    /* BSS blob (LE, overlays ev->u.scan_result) */
    /* BSS blob with TRUE compiler offsets (no packing! the driver reads
     * via struct: rateset_count@52, chanspec@72, rssi@78, ie_offset@116,
     * ie_length@120, total 128 bytes). */
    int secured = (r->auth_mode != 0);
    int ie_len = secured ? (int)sizeof(escan_rsn_ie) : 0;
    frame[off++] = (140 + ie_len) & 0xFF; frame[off++] = 0; frame[off++] = 0; frame[off++] = 0;  /* buflen */
    frame[off++] = 107; frame[off++] = 0; frame[off++] = 0; frame[off++] = 0;         /* version */
    frame[off++] = 0; frame[off++] = 0;                                              /* sync_id */
    frame[off++] = 1; frame[off++] = 0;                                              /* bss_count */
    frame[off++] = 107; frame[off++] = 0; frame[off++] = 0; frame[off++] = 0;         /* bss.version */
    frame[off++] = (128 + ie_len) & 0xFF; frame[off++] = 0; frame[off++] = 0; frame[off++] = 0; /* length */
    memcpy(frame + off, r->bssid, 6); off += 6;
    frame[off++] = 100; frame[off++] = 0;                                            /* beacon_period */
    frame[off++] = secured ? 0x31 : 0x21; frame[off++] = 0x04;                        /* capability */
    size_t slen = strlen(r->ssid);
    if (slen > CYW43_MAX_SSID_LEN) slen = CYW43_MAX_SSID_LEN;
    frame[off++] = (uint8_t)slen;
    memset(frame + off, 0, 32);
    memcpy(frame + off, r->ssid, slen);
    off += 32;                               /* off == bss+51 */
    frame[off++] = 0;                        /* pad -> rateset_count@52 */
    frame[off++] = 8; frame[off++] = 0; frame[off++] = 0; frame[off++] = 0;
    memset(frame + off, 0, 16);
    frame[off] = 0x82; frame[off + 1] = 0x84; frame[off + 2] = 0x8B; frame[off + 3] = 0x96;
    off += 16;                               /* off == bss+72 */
    frame[off++] = r->channel; frame[off++] = 0;   /* chanspec@72 */
    frame[off++] = 0; frame[off++] = 0;            /* atim@74 */
    frame[off++] = 1;                              /* dtim@76 */
    frame[off++] = 0;                              /* pad -> rssi@78 */
    frame[off++] = (uint8_t)(r->rssi & 0xFF); frame[off++] = (uint8_t)((r->rssi >> 8) & 0xFF);
    frame[off++] = 0xB0;                           /* phy_noise@80 */
    frame[off++] = 0;                              /* n_cap@81 */
    frame[off++] = 0; frame[off++] = 0;            /* pad -> nbss_cap@84 */
    frame[off++] = 0; frame[off++] = 0; frame[off++] = 0; frame[off++] = 0;
    frame[off++] = r->channel;                     /* ctl_ch@88 */
    frame[off++] = 0; frame[off++] = 0; frame[off++] = 0; /* pad -> reserved32@92 */
    frame[off++] = 0; frame[off++] = 0; frame[off++] = 0; frame[off++] = 0;
    frame[off++] = 0;                              /* flags@96 */
    frame[off++] = 0; frame[off++] = 0; frame[off++] = 0; /* reserved@97 */
    memset(frame + off, 0, 16);                    /* basic_mcs@100 */
    off += 16;                               /* off == bss+116 */
    frame[off++] = 128; frame[off++] = 0;          /* ie_offset@116 */
    frame[off++] = 0; frame[off++] = 0;            /* pad -> ie_length@120 */
    frame[off++] = ie_len & 0xFF; frame[off++] = 0; frame[off++] = 0; frame[off++] = 0;
    frame[off++] = 0; frame[off++] = 0;            /* SNR@124 */
    frame[off++] = 0; frame[off++] = 0;            /* pad -> bss end@128 */
    if (ie_len) {
        memcpy(frame + off, escan_rsn_ie, sizeof(escan_rsn_ie));
        off += sizeof(escan_rsn_ie);
    }
    uint16_t ev_len = (uint16_t)(off - len_offset - 2);
    frame[len_offset] = (ev_len >> 8) & 0xFF;
    frame[len_offset + 1] = ev_len & 0xFF;
    sdpcm_fill_header(frame, off, SDPCM_EVENT_CHANNEL, 14);
    frame[12] = 0; frame[13] = 0;
    bdc_header_t *bdc = (bdc_header_t *)(frame + 14);
    bdc->flags = 0x20;
    bdc->priority = 0;
    bdc->flags2 = 0;
    bdc->data_offset = 0;
    rx_queue_push(frame, off);
    if (CYW43_DBG)
        fprintf(stderr, "[CYW43] escan result: '%s' ch=%d rssi=%d\n", r->ssid, r->channel, r->rssi);
}

/* Answer an escan request with the fake AP list + completion. */
static void cyw43_send_scan_results(void) {
    for (int i = 0; i < cyw43.scan_count; i++)
        cyw43_queue_escan_result(&cyw43.scan_results[i]);
    cyw43_queue_event(CYW43_EV_ESCAN_RESULT, CYW43_STATUS_SUCCESS, 0, 0);
}

/* Queue connection events (simulates successful WiFi join) */
static void cyw43_queue_connect_events(void) {
    cyw43_queue_event(CYW43_EV_AUTH, CYW43_STATUS_SUCCESS, 0, 0);
    cyw43_queue_event(CYW43_EV_LINK, CYW43_STATUS_SUCCESS, 0, 1);  /* flags=1: link-up */
    cyw43_queue_event(CYW43_EV_SET_SSID, CYW43_STATUS_SUCCESS, 0, 0);
    /* PSK_SUP/KEYED only for secured networks: sending it on an open
     * join wedges the driver's state machine (it never reaches the
     * connected combination the join-wait loop wants). */
    if (cyw43.last_wpa_auth != 0)
        cyw43_queue_event(CYW43_EV_PSK_SUP, CYW43_SUP_KEYED, 0, 0);
    cyw43.wifi_state = CYW43_WIFI_CONNECTED;

    if (CYW43_DBG)
        fprintf(stderr, "[CYW43] Queued connection events for SSID '%s'\n",
                cyw43.connected_ssid);
}

/* Wrap an Ethernet frame in SDPCM + BDC and queue for firmware, tagged
 * for interface itf (0=STA, 1=AP selects the driver's netif). */
static void cyw43_queue_rx_data_itf(const uint8_t *eth_frame, int eth_len,
                                     uint8_t itf) {
    uint8_t frame[CYW43_MAX_FRAME_SIZE];
    int total = 18 + eth_len;  /* SDPCM(12) + pad(2) + BDC(4) + ethernet */

    if (total > CYW43_MAX_FRAME_SIZE) return;

    sdpcm_fill_header(frame, total, SDPCM_DATA_CHANNEL, 14);

    /* 2-byte pad */
    frame[12] = 0; frame[13] = 0;

    /* BDC header */
    bdc_header_t *bdc = (bdc_header_t *)(frame + 14);
    bdc->flags = 0x20;
    bdc->priority = 0;
    bdc->flags2 = itf;
    bdc->data_offset = 0;

    /* Ethernet frame */
    memcpy(frame + 18, eth_frame, eth_len);

    rx_queue_push(frame, total);
}

/* Wrap an Ethernet frame from TAP in SDPCM + BDC and queue for firmware */
static void cyw43_queue_rx_data(const uint8_t *eth_frame, int eth_len) {
    cyw43_queue_rx_data_itf(eth_frame, eth_len, 0);
}

/* vnet/TAP RX for a possibly-AP device: STA copy always, plus an AP copy
 * while the soft-AP is up (each netif filters by IP/MAC; harmless dup). */
static void cyw43_queue_rx_vnet(const uint8_t *eth_frame, int eth_len) {
    cyw43_queue_rx_data_itf(eth_frame, eth_len, 0);
    if (cyw43.ap_up)
        cyw43_queue_rx_data_itf(eth_frame, eth_len, 1);
}

/* vnet port receive: wrap a vnet Ethernet frame and queue for firmware.
 * BT link state + helpers are defined with the HCI responder below. */
/* BLE GATT room protocol (ethertype 0x88B6) — see the ATT engine below
 * for the full layout. These fwd declarations let the room-RX path call
 * into the link table before its storage (which lives with the engine). */
#define BT_GATT_MAX_LINKS 4
#define BT_GATT_BASE_HANDLE 0x0042
struct bt_gatt_link_state {
    int active;
    uint16_t handle;
    uint8_t peer[6];
    uint16_t peer_mtu;
    uint16_t cccd_cfg;
    int ind_pending;
    uint8_t ntf_value[32];
    uint8_t ntf_vlen;
    uint16_t ntf_handle;
    int ntf_indicate;
    int ntf_queued;
    uint8_t prep_buf[32];
    uint16_t prep_handle;
    int prep_len;
};
static int bt_gatt_link_by_peer(const uint8_t *peer);
static int bt_gatt_link_by_handle(uint16_t h);
static void bt_gatt_sync_link0(void);
static void bt_gatt_link_down_peer(const uint8_t *peer);
static int bt_gatt_att_is_response(uint8_t op);
static void bt_gatt_deliver_room_resp(int li, const uint8_t *att, int att_len);
static void bt_gatt_route_room_att(const uint8_t *att, int att_len);
static void bt_gatt_route_room_att_on(int li, const uint8_t *att, int att_len);
static void bt_gatt_handle_room_att_on(int li, const uint8_t *att, int att_len,
                                       const uint8_t *src_mac);
extern struct bt_gatt_link_state bt_gatt_links[BT_GATT_MAX_LINKS];
static void cyw43_vnet_rx(void *ctx, const uint8_t *frame, int len) {
    (void)ctx;
    /* BLE ADV share (ethertype 0x88B5): a scanning peer turns a room
     * advertisement into an LE Advertising Report HCI event. */
    if (len >= 14 + 38 && frame[12] == 0x88 && frame[13] == 0xB5 &&
        cyw43.bt_scan_enabled &&
        memcmp(frame + 14, cyw43.mac_addr, 6) != 0) {
        int dlen = frame[20];
        if (dlen > 31) dlen = 31;
        uint8_t ev[2 + 12 + 31 + 1];
        ev[0] = 0x3E;
        ev[1] = (uint8_t)(12 + dlen);
        ev[2] = 0x02; ev[3] = 0x01;       /* ADV_REPORT, 1 report */
        ev[4] = 0x00; ev[5] = 0x00;       /* ADV_IND, public addr */
        memcpy(ev + 6, frame + 14, 6);
        ev[12] = (uint8_t)dlen;
        memcpy(ev + 13, frame + 21, (size_t)dlen);
        ev[13 + dlen] = 0xC8;             /* RSSI -56 dBm */
        cyw43_bt_queue_hci(0x04, ev, 13 + dlen + 1);
        if (CYW43_DBG)
            fprintf(stderr, "[CYW43] BT ADV report from %02X:%02X:%02X:%02X:%02X:%02X\n",
                    frame[14], frame[15], frame[16],
                    frame[17], frame[18], frame[19]);
        return;
    }
    /* BLE GATT share (ethertype 0x88B6): virtual LE-U link frames from a
     * peer central. Layout: [peerMAC 6][att_len 1][ATT PDU...].
     * CONNECT_REQ (att PDU opcode 0xF0) brings the link up + raises LE
     * Connection Complete; ATT requests are served from bt_gatt_db and
     * answered as HCI ACL; DISCONNECT (opcode 0xF1) drops the link with
     * Disconnection Complete. Frames not for us are ignored.
     * ATT responses (first byte 0x01/0x03/0x05/0x07/0x09/0x0B/0x0D/0x0F/
     * 0x11/0x13/0x17/0x19/0x1B/0x1D) are the peer-server's answer to OUR
     * client request: deliver as HCI ACL so the guest stack completes
     * its ATT client op (two-instance GATT: A requests, B serves + routes
     * the response frame back to A's MAC). */
    if (len >= 14 + 6 + 1 && frame[12] == 0x88 && frame[13] == 0xB6) {
        int alen = frame[20];
        const uint8_t *att = frame + 21;
        if (alen > 0 && 21 + alen <= len) {
            if (att[0] == 0xF0 && alen >= 8) {
                /* CONNECT_REQ: [F0][peer_type][peerMAC 6]. */
                bt_gatt_link_up(att + 2);
                return;
            }
            if (att[0] == 0xF1) {
                bt_gatt_link_down_peer(frame + 6);
                bt_gatt_sync_link0();
                return;
            }
            if (bt_gatt_att_is_response(att[0])) {
                /* Peer's answer to our request: must be link-up, must be
                 * addressed to us (dst MAC == our MAC), and must not be
                 * our own echo (src MAC == our MAC). Routed to the link
                 * whence the request came (by source peer MAC). */
                if (memcmp(frame, cyw43.mac_addr, 6) == 0 &&
                    memcmp(frame + 6, cyw43.mac_addr, 6) != 0) {
                    int li = bt_gatt_link_by_peer(frame + 6);
                    if (li >= 0)
                        bt_gatt_deliver_room_resp(li, att, alen);
                    return;
                }
                return;
            }
            /* ATT request addressed to our link: serve it. */
            if (memcmp(frame + 14, cyw43.mac_addr, 6) != 0) {
                int li = bt_gatt_link_by_peer(frame + 6);
                if (li < 0) li = bt_gatt_link_by_peer(frame + 14);
                if (li < 0 && bt_gatt_links[0].active) li = 0;
                if (li >= 0)
                    bt_gatt_handle_room_att_on(li, att, alen, frame + 14);
                return;
            }
        }
        return;
    }
    cyw43_queue_rx_vnet(frame, len);
}

/* Attach CYW43 to the vnet bus (native gateway path). Idempotent. */
void cyw43_vnet_attach(void) {
    if (cyw43.vnet_port >= 0) return;
    cyw43.vnet_port = vnet_register_port("cyw43", VNET_PORT_CYW43,
                                         cyw43.mac_addr, cyw43_vnet_rx, NULL);
    if (CYW43_DBG)
        fprintf(stderr, "[CYW43] vnet port %d attached (nodhcp=%d)\n",
                cyw43.vnet_port, cyw43_no_fake_dhcp);
}

/* ========================================================================
 * IOCTL Handler
 * ======================================================================== */

static void cyw43_handle_ioctl(const uint8_t *buf, int len) {
    if (len < 12 + 16) return;  /* SDPCM + CDC minimum */

    sdpcm_header_t *sdpcm = (sdpcm_header_t *)buf;
    cdc_header_t *cdc = (cdc_header_t *)(buf + 12);

    cyw43.last_fw_seq = sdpcm->sequence;

    uint32_t cmd = cdc->cmd;
    uint16_t ioctl_id = (uint16_t)(cdc->flags >> CDC_ID_SHIFT);
    int is_set = (cdc->flags & 0x02) != 0;
    const uint8_t *payload = buf + 28;  /* After SDPCM(12) + CDC(16) */
    int payload_len = len - 28;

    if (CYW43_DBG)
        fprintf(stderr, "[CYW43] IOCTL cmd=%d %s id=%d payload_len=%d\n",
                cmd, is_set ? "SET" : "GET", ioctl_id, payload_len);

    switch (cmd) {
    case WLC_SET_VAR: {
        /* iovar SET: varname\0 + value. "escan" triggers scan results. */
        const char *varname = (const char *)payload;
        size_t vlen = strnlen(varname, (size_t)(payload_len > 0 ? payload_len : 0));
        if (vlen == strlen("escan") && memcmp(varname, "escan", vlen) == 0)
            cyw43_send_scan_results();
        /* AP bringup: bsscfg:ssid (ifidx, ssid_len, ssid) records the
         * beacon; bss (ifidx, up) raises/lowers the soft-AP. */
        if (vlen == strlen("bsscfg:ssid") && memcmp(varname, "bsscfg:ssid", vlen) == 0 &&
            payload_len >= (int)(vlen + 1 + 8)) {
            const uint8_t *v = payload + vlen + 1;
            uint32_t ifidx = v[0] | (v[1] << 8) | (v[2] << 16) | (v[3] << 24);
            uint32_t slen = v[4] | (v[5] << 8) | (v[6] << 16) | (v[7] << 24);
            if (ifidx == 1 && slen > 0 && slen <= CYW43_MAX_SSID_LEN &&
                payload_len >= (int)(vlen + 1 + 8 + slen)) {
                memcpy(cyw43.ap_ssid, v + 8, slen);
                cyw43.ap_ssid[slen] = '\0';
            }
        }
        if (vlen == strlen("bss") && memcmp(varname, "bss", vlen) == 0 &&
            payload_len >= (int)(vlen + 1 + 8)) {
            const uint8_t *v = payload + vlen + 1;
            uint32_t ifidx = v[0] | (v[1] << 8) | (v[2] << 16) | (v[3] << 24);
            uint32_t up = v[4] | (v[5] << 8) | (v[6] << 16) | (v[7] << 24);
            if (ifidx == 1)
                cyw43_ap_set_up(up != 0);
        }
        cyw43_queue_ioctl_response(cmd, ioctl_id, NULL, 0, 0);
        return;
    }
    case WLC_GET_VAR: {
        /* payload is null-terminated iovar name */
        const char *varname = (const char *)payload;

        /* L30: payload comes from guest memory and may lack a NUL —
         * compare bounded by payload_len (requires the terminator). */
        size_t vlen = strnlen(varname, (size_t)(payload_len > 0 ? payload_len : 0));
        if (vlen == strlen("cur_etheraddr") &&
            memcmp(varname, "cur_etheraddr", vlen) == 0) {
            cyw43_queue_ioctl_response(cmd, ioctl_id, cyw43.mac_addr, 6, 0);
            return;
        }
        if (vlen == strlen("ver") &&
            memcmp(varname, "ver", vlen) == 0) {
            const char *ver = "wl0: Bramble CYW43 Emulator\n";
            cyw43_queue_ioctl_response(cmd, ioctl_id,
                                        (const uint8_t *)ver, (int)strlen(ver) + 1, 0);
            return;
        }
        if (vlen == strlen("bss") &&
            memcmp(varname, "bss", vlen) == 0) {
            /* AP state query (preceded by "bss\0" + ifidx): 0 = down. */
            uint8_t up[4] = { (uint8_t)(cyw43.ap_up ? 1 : 0), 0, 0, 0 };
            cyw43_queue_ioctl_response(cmd, ioctl_id, up, 4, 0);
            return;
        }
        /* Default: return zeros */
        uint8_t zeros[256];
        memset(zeros, 0, sizeof(zeros));
        int resp_len = payload_len > 0 ? payload_len : 4;
        if (resp_len > (int)sizeof(zeros)) resp_len = (int)sizeof(zeros);
        cyw43_queue_ioctl_response(cmd, ioctl_id, zeros, resp_len, 0);
        return;
    }

    case WLC_SET_SSID: {
        /* Payload: 4 bytes SSID length + SSID string (32 bytes) */
        if (payload_len >= 36) {
            uint32_t ssid_len = payload[0] | (payload[1] << 8) |
                                (payload[2] << 16) | (payload[3] << 24);
            if (ssid_len > CYW43_MAX_SSID_LEN) ssid_len = CYW43_MAX_SSID_LEN;
            memcpy(cyw43.connected_ssid, payload + 4, ssid_len);
            cyw43.connected_ssid[ssid_len] = '\0';

            if (cpu.debug_enabled)
                fprintf(stderr, "[CYW43] JOIN SSID: '%s'\n", cyw43.connected_ssid);
        }

        /* Respond success */
        cyw43_queue_ioctl_response(cmd, ioctl_id, NULL, 0, 0);

        /* Arm join events: they queue when this response is popped (see
         * connect_pending_id), i.e. strictly after ACTIVE. */
        cyw43.connect_pending_id = (int)ioctl_id;
        return;
    }

    case WLC_GET_SSID: {
        /* Return current SSID: 4 bytes length + 32 bytes SSID */
        uint8_t resp[36];
        memset(resp, 0, sizeof(resp));
        uint32_t slen = (uint32_t)strlen(cyw43.connected_ssid);
        resp[0] = slen & 0xFF;
        resp[1] = (slen >> 8) & 0xFF;
        resp[2] = (slen >> 16) & 0xFF;
        resp[3] = (slen >> 24) & 0xFF;
        memcpy(resp + 4, cyw43.connected_ssid, slen);
        cyw43_queue_ioctl_response(cmd, ioctl_id, resp, 36, 0);
        return;
    }

    case WLC_GET_BSSID: {
        /* Return a fake BSSID */
        uint8_t bssid[6] = {0x02, 0xCA, 0xFE, 0xBA, 0xBE, 0x01};
        cyw43_queue_ioctl_response(cmd, ioctl_id, bssid, 6, 0);
        return;
    }

    case WLC_SET_WPA_AUTH: {
        /* u32 WPA auth mode (0 = open). Remember for join events. */
        if (payload_len >= 4)
            cyw43.last_wpa_auth = payload[0] | (payload[1] << 8) |
                                  (payload[2] << 16) | (payload[3] << 24);
        cyw43_queue_ioctl_response(cmd, ioctl_id, NULL, 0, 0);
        return;
    }

    case WLC_SET_CHANNEL: {        /* u32 channel (AP bringup targets the AP bss via STA iface).
         * Remember it for the AP beacon once an AP SSID is known. */
        if (payload_len >= 4 && cyw43.ap_ssid[0]) {
            uint32_t ch = payload[0] | (payload[1] << 8) |
                          (payload[2] << 16) | (payload[3] << 24);
            if (ch >= 1 && ch <= 14)
                cyw43.ap_channel = (int)ch;
        }
        cyw43_queue_ioctl_response(cmd, ioctl_id, NULL, 0, 0);
        return;
    }

    default:
        /* All other IOCTLs: return success with echo payload for GET, empty for SET */
        if (is_set) {
            cyw43_queue_ioctl_response(cmd, ioctl_id, NULL, 0, 0);
        } else {
            /* GET: return payload echoed (or zeros) */
            uint8_t zeros[256];
            memset(zeros, 0, sizeof(zeros));
            int resp_len = payload_len > 0 ? payload_len : 4;
            if (resp_len > (int)sizeof(zeros)) resp_len = (int)sizeof(zeros);
            cyw43_queue_ioctl_response(cmd, ioctl_id, zeros, resp_len, 0);
        }
        return;
    }
}

/* ========================================================================
 * Fake DHCP Server (assigns 192.168.4.2 to firmware, no real network needed)
 * ======================================================================== */

/* Fixed IP addressing for the virtual WiFi network */
static const uint8_t dhcp_client_ip[4]  = {192, 168,   4, 2};
static const uint8_t dhcp_server_ip[4]  = {192, 168,   4, 1};
static const uint8_t dhcp_subnet[4]     = {255, 255, 255, 0};
static const uint8_t dhcp_lease_time[4] = {0, 0, 14, 16};  /* 3600 s */

/* Build and queue a DHCP OFFER (msg_type=2) or ACK (msg_type=5) reply. */
static void cyw43_send_dhcp_reply(const uint8_t *eth_req, int eth_len, uint8_t reply_type) {
    if (eth_len < 14 + 20 + 8 + 240) return;

    const uint8_t *ip_req  = eth_req + 14;
    int ip_hlen = (ip_req[0] & 0x0F) * 4;
    if (ip_hlen < 20 || ip_hlen > 60) return;  /* L47: validate IHL */
    const uint8_t *dhcp    = ip_req + ip_hlen + 8;  /* skip IP+UDP */
    int dhcp_len           = eth_len - 14 - ip_hlen - 8;
    if (dhcp_len < 236) return;

    uint32_t xid = ((uint32_t)dhcp[4] << 24) | ((uint32_t)dhcp[5] << 16) |
                   ((uint32_t)dhcp[6] <<  8) |  (uint32_t)dhcp[7];
    const uint8_t *chaddr  = dhcp + 28;  /* client MAC (first 6 bytes) */

    uint8_t frame[512];
    memset(frame, 0, sizeof(frame));
    int off = 0;

    /* Ethernet header: broadcast dst, server MAC src */
    memset(frame + off, 0xFF, 6);               off += 6;
    memcpy(frame + off, cyw43.mac_addr, 6);     off += 6;
    frame[off++] = 0x08; frame[off++] = 0x00;   /* IPv4 */

    /* IPv4 header */
    int ip_off = off;
    frame[off++] = 0x45;                        /* v4, IHL=5 */
    frame[off++] = 0x00;                        /* DSCP/ECN */
    int ip_len_off = off; off += 2;             /* total length (filled later) */
    frame[off++] = 0x00; frame[off++] = 0x01;  /* ID */
    frame[off++] = 0x00; frame[off++] = 0x00;  /* flags/frag */
    frame[off++] = 0x80;                        /* TTL=128 */
    frame[off++] = 0x11;                        /* proto=UDP */
    int ip_csum_off = off; off += 2;            /* checksum (filled later) */
    memcpy(frame + off, dhcp_server_ip, 4);     off += 4;  /* src = server */
    frame[off++] = 255; frame[off++] = 255;
    frame[off++] = 255; frame[off++] = 255;    /* dst = broadcast */

    /* UDP header */
    int udp_off = off;
    frame[off++] = 0x00; frame[off++] = 67;    /* src port = 67 */
    frame[off++] = 0x00; frame[off++] = 68;    /* dst port = 68 */
    int udp_len_off = off; off += 2;            /* UDP length (filled later) */
    frame[off++] = 0x00; frame[off++] = 0x00;  /* checksum = 0 */

    /* DHCP message */
    int dhcp_off = off;
    frame[off++] = 0x02;                        /* op = BOOTREPLY */
    frame[off++] = 0x01; frame[off++] = 0x06;  /* htype=1, hlen=6 */
    frame[off++] = 0x00;                        /* hops */
    frame[off++] = (xid >> 24) & 0xFF;
    frame[off++] = (xid >> 16) & 0xFF;
    frame[off++] = (xid >>  8) & 0xFF;
    frame[off++] = (xid >>  0) & 0xFF;
    frame[off++] = 0x00; frame[off++] = 0x00;  /* secs */
    frame[off++] = 0x80; frame[off++] = 0x00;  /* flags: broadcast */
    frame[off++] = 0x00; frame[off++] = 0x00;
    frame[off++] = 0x00; frame[off++] = 0x00;  /* ciaddr = 0 */
    memcpy(frame + off, dhcp_client_ip, 4);     off += 4;  /* yiaddr */
    memcpy(frame + off, dhcp_server_ip, 4);     off += 4;  /* siaddr */
    frame[off++] = 0x00; frame[off++] = 0x00;
    frame[off++] = 0x00; frame[off++] = 0x00;  /* giaddr = 0 */
    memcpy(frame + off, chaddr, 16);            off += 16; /* chaddr */
    memset(frame + off, 0, 64);                 off += 64; /* sname */
    memset(frame + off, 0, 128);                off += 128;/* file */
    /* Magic cookie */
    frame[off++] = 0x63; frame[off++] = 0x82;
    frame[off++] = 0x53; frame[off++] = 0x63;
    /* Options */
    frame[off++] = 53; frame[off++] = 1; frame[off++] = reply_type;  /* msg type */
    frame[off++] = 54; frame[off++] = 4;
    memcpy(frame + off, dhcp_server_ip, 4);     off += 4;  /* server ID */
    frame[off++] = 51; frame[off++] = 4;
    memcpy(frame + off, dhcp_lease_time, 4);    off += 4;  /* lease time */
    frame[off++] =  1; frame[off++] = 4;
    memcpy(frame + off, dhcp_subnet, 4);        off += 4;  /* subnet mask */
    frame[off++] =  3; frame[off++] = 4;
    memcpy(frame + off, dhcp_server_ip, 4);     off += 4;  /* router */
    frame[off++] =  6; frame[off++] = 4;
    memcpy(frame + off, dhcp_server_ip, 4);     off += 4;  /* DNS */
    frame[off++] = 255;                                    /* end */

    /* Fill in lengths */
    int ip_total  = off - ip_off;
    int udp_total = off - udp_off;
    frame[ip_len_off]     = (ip_total  >> 8) & 0xFF;
    frame[ip_len_off + 1] =  ip_total        & 0xFF;
    frame[udp_len_off]    = (udp_total >> 8) & 0xFF;
    frame[udp_len_off + 1] = udp_total       & 0xFF;

    /* IPv4 header checksum */
    uint32_t csum = 0;
    for (int i = 0; i < 20; i += 2)
        csum += ((uint32_t)frame[ip_off + i] << 8) | frame[ip_off + i + 1];
    while (csum >> 16) csum = (csum & 0xFFFF) + (csum >> 16);
    csum = ~csum & 0xFFFF;
    frame[ip_csum_off]     = (csum >> 8) & 0xFF;
    frame[ip_csum_off + 1] =  csum       & 0xFF;

    (void)udp_off; (void)dhcp_off;

    cyw43_queue_rx_data(frame, off);

    if (cpu.debug_enabled)
        fprintf(stderr, "[CYW43] DHCP %s → offer %d.%d.%d.%d\n",
                reply_type == 2 ? "DISCOVER" : "REQUEST",
                dhcp_client_ip[0], dhcp_client_ip[1],
                dhcp_client_ip[2], dhcp_client_ip[3]);
}

/* Virtual gateway MAC (distinct from the device MAC). */
static const uint8_t fake_gw_mac[6] = {0x02, 0x12, 0x34, 0x56, 0x78, 0x01};
static const uint8_t fake_gw_ip[4] = {192, 168, 4, 1};

/* Fake ARP responder for the virtual gateway: who-has .1 -> is-at gwmac.
 * Lets offline guests resolve the gateway (ping/DHCP-less static setups).
 * Returns 1 if handled. */
static int cyw43_handle_fake_arp(const uint8_t *eth_frame, int eth_len) {
    if (eth_len < 14 + 28) return 0;
    if (eth_frame[12] != 0x08 || eth_frame[13] != 0x06) return 0;  /* ARP */
    const uint8_t *a = eth_frame + 14;
    if (a[0] != 0 || a[1] != 1) return 0;             /* Ethernet */
    if (a[2] != 0x08 || a[3] != 0x00) return 0;       /* IPv4 */
    if (a[4] != 6 || a[5] != 4) return 0;
    if (a[6] != 0 || a[7] != 1) return 0;             /* request? */
    if (memcmp(a + 24, fake_gw_ip, 4) != 0) return 0; /* who-has .1? */

    uint8_t frame[64];
    int off = 0;
    memcpy(frame + off, a + 8, 6);  off += 6;    /* dst = requester */
    memcpy(frame + off, fake_gw_mac, 6); off += 6; /* src = gateway */
    frame[off++] = 0x08; frame[off++] = 0x06;
    frame[off++] = 0x00; frame[off++] = 0x01;
    frame[off++] = 0x08; frame[off++] = 0x00;
    frame[off++] = 0x06; frame[off++] = 0x04;
    frame[off++] = 0x00; frame[off++] = 0x02;    /* reply */
    memcpy(frame + off, fake_gw_mac, 6); off += 6;
    memcpy(frame + off, fake_gw_ip, 4);  off += 4;
    memcpy(frame + off, a + 8, 6);       off += 6;  /* target = requester */
    memcpy(frame + off, a + 14, 4);      off += 4;
    while (off < 60) frame[off++] = 0;            /* min frame pad */

    cyw43_queue_rx_vnet(frame, off);
    return 1;
}

/* Fake ICMP echo responder for the virtual gateway (.1): lets offline
 * guests (browser demos) ping something that always answers. Only used
 * when the fake network is active (see -nodhcp). Returns 1 if handled. */
static int cyw43_handle_fake_icmp(const uint8_t *eth_frame, int eth_len) {    static const uint8_t gw_ip[4] = {192, 168, 4, 1};
    if (eth_len < 14 + 20 + 8) return 0;
    if (eth_frame[12] != 0x08 || eth_frame[13] != 0x00) return 0;  /* IPv4 */
    const uint8_t *ip = eth_frame + 14;
    if ((ip[0] >> 4) != 4) return 0;
    int ip_hlen = (ip[0] & 0x0F) * 4;
    if (ip_hlen < 20 || ip_hlen > 60) return 0;
    if (ip[9] != 1) return 0;  /* not ICMP */
    if (memcmp(ip + 16, gw_ip, 4) != 0) return 0;  /* not for us */
    const uint8_t *icmp = ip + ip_hlen;
    int icmp_len = eth_len - 14 - ip_hlen;
    if (icmp_len < 8 || icmp[0] != 8 || icmp[1] != 0) return 0;  /* echo req? */

    uint8_t frame[CYW43_MAX_FRAME_SIZE];
    if (14 + ip_hlen + icmp_len > (int)sizeof(frame)) return 0;
    int off = 0;
    memcpy(frame + off, eth_frame + 6, 6);  off += 6;   /* dst = requester */
    memcpy(frame + off, cyw43.mac_addr, 6); off += 6;   /* src = us */
    frame[off++] = 0x08; frame[off++] = 0x00;           /* IPv4 */
    int ip_off = off;
    memcpy(frame + off, ip, ip_hlen);                   off += ip_hlen;
    /* swap src/dst IP */
    memcpy(frame + ip_off + 12, ip + 16, 4);
    memcpy(frame + ip_off + 16, ip + 12, 4);
    int icmp_off = off;
    memcpy(frame + off, icmp, icmp_len);                off += icmp_len;
    frame[icmp_off] = 0;                                /* echo reply */
    frame[icmp_off + 2] = 0; frame[icmp_off + 3] = 0;   /* csum recompute */
    /* checksums */
    frame[ip_off + 10] = 0; frame[ip_off + 11] = 0;
    uint32_t csum = 0;
    for (int i = 0; i < ip_hlen; i += 2)
        csum += ((uint32_t)frame[ip_off + i] << 8) | frame[ip_off + i + 1];
    while (csum >> 16) csum = (csum & 0xFFFF) + (csum >> 16);
    csum = ~csum & 0xFFFF;
    frame[ip_off + 10] = (csum >> 8) & 0xFF;
    frame[ip_off + 11] = csum & 0xFF;
    csum = 0;
    for (int i = 0; i + 1 < icmp_len; i += 2)
        csum += ((uint32_t)frame[icmp_off + i] << 8) | frame[icmp_off + i + 1];
    if (icmp_len & 1) csum += (uint32_t)frame[icmp_off + icmp_len - 1] << 8;
    while (csum >> 16) csum = (csum & 0xFFFF) + (csum >> 16);
    csum = ~csum & 0xFFFF;
    frame[icmp_off + 2] = (csum >> 8) & 0xFF;
    frame[icmp_off + 3] = csum & 0xFF;

    cyw43_queue_rx_vnet(frame, off);
    return 1;
}

/* Fake IPv6 gateway identities (link-local + ULA). Guests resolve the
 * router and autoconfigure fd00:4::/64 via the RA below. */
static const uint8_t fake_gw_ip6_ll[16] =
    {0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
static const uint8_t fake_gw_ip6_ula[16] =
    {0xFD, 0x00, 0x00, 0x04, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
static const uint8_t fake_ip6_prefix[8] =
    {0xFD, 0x00, 0x00, 0x04, 0, 0, 0, 0};
static const uint8_t fake_ip6_mcast_all[16] =
    {0xFF, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
static const uint8_t fake_ip6_unspec[16] = {0};

/* ICMPv6 checksum over pseudo-header + payload (payload csum field zero). */
static uint16_t cyw43_icmp6_cksum(const uint8_t *src, const uint8_t *dst,
                                  const uint8_t *pl, int plen) {
    uint32_t csum = 0;
    for (int i = 0; i < 16; i += 2)
        csum += ((uint32_t)src[i] << 8) | src[i + 1];
    for (int i = 0; i < 16; i += 2)
        csum += ((uint32_t)dst[i] << 8) | dst[i + 1];
    csum += (uint32_t)plen;
    csum += 58;  /* next header: ICMPv6 */
    for (int i = 0; i + 1 < plen; i += 2)
        csum += ((uint32_t)pl[i] << 8) | pl[i + 1];
    if (plen & 1)
        csum += (uint32_t)pl[plen - 1] << 8;
    while (csum >> 16)
        csum = (csum & 0xFFFF) + (csum >> 16);
    return (uint16_t)(~csum & 0xFFFF);
}

/* Start an ETH+IPv6+ICMPv6 frame in buf; returns offset of ICMPv6 body. */
static int cyw43_icmp6_frame_start(uint8_t *buf, const uint8_t *dst_mac,
                                   const uint8_t *src_ip,
                                   const uint8_t *dst_ip, int icmp_len) {
    int off = 0;
    memcpy(buf + off, dst_mac, 6);  off += 6;
    memcpy(buf + off, fake_gw_mac, 6);  off += 6;
    buf[off++] = 0x86; buf[off++] = 0xDD;
    buf[off++] = 0x60; buf[off++] = 0; buf[off++] = 0; buf[off++] = 0;
    buf[off++] = (icmp_len >> 8) & 0xFF; buf[off++] = icmp_len & 0xFF;
    buf[off++] = 58; buf[off++] = 255;  /* ICMPv6, hop limit 255 (NDP) */
    memcpy(buf + off, src_ip, 16);  off += 16;
    memcpy(buf + off, dst_ip, 16);  off += 16;
    return off;  /* == 54 */
}

/* Fake NDP: Router Solicitation -> Advertisement (SLAAC for fd00:4::/64),
 * Neighbor Solicitation for a gateway address -> Advertisement.
 * Returns 1 if handled. */
static void cyw43_fake_send_ra(const uint8_t *dst_ip,
                               const uint8_t *rep_dst_mac) {
    /* RA body 16 + src-ll 8 + prefix 32 + mtu 8 = 64 */
    uint8_t frame[CYW43_MAX_FRAME_SIZE];
    int off = cyw43_icmp6_frame_start(frame, rep_dst_mac, fake_gw_ip6_ll,
                                      dst_ip, 64);
    int io = off;
    frame[off++] = 134; frame[off++] = 0;     /* RA */
    frame[off++] = 0; frame[off++] = 0;       /* cksum later */
    frame[off++] = 64;                        /* cur hop limit */
    frame[off++] = 0;                         /* M/O flags */
    frame[off++] = 0x07; frame[off++] = 0x08; /* router lifetime 1800 */
    frame[off++] = 0; frame[off++] = 0;
    frame[off++] = 0; frame[off++] = 0;       /* reachable */
    frame[off++] = 0; frame[off++] = 0;
    frame[off++] = 0; frame[off++] = 0;       /* retrans */
    frame[off++] = 1; frame[off++] = 1;       /* src link-layer */
    memcpy(frame + off, fake_gw_mac, 6); off += 6;
    frame[off++] = 3; frame[off++] = 4;       /* prefix info */
    frame[off++] = 64;                        /* prefix len */
    frame[off++] = 0xC0;                      /* L + A (SLAAC) */
    frame[off++] = 0; frame[off++] = 1;
    frame[off++] = 0x51; frame[off++] = 0x80; /* valid 86400 */
    frame[off++] = 0; frame[off++] = 0;
    frame[off++] = 0x38; frame[off++] = 0x40; /* preferred 14400 */
    frame[off++] = 0; frame[off++] = 0;
    frame[off++] = 0; frame[off++] = 0;       /* reserved */
    memcpy(frame + off, fake_ip6_prefix, 8); off += 8;
    memset(frame + off, 0, 8); off += 8;
    frame[off++] = 5; frame[off++] = 1;       /* MTU */
    frame[off++] = 0; frame[off++] = 0;
    frame[off++] = 0; frame[off++] = 0x05;
    frame[off++] = 0xDC; frame[off++] = 0x00; /* 1500 */
    uint16_t cs = cyw43_icmp6_cksum(fake_gw_ip6_ll, dst_ip,
                                    frame + io, 64);
    frame[io + 2] = (cs >> 8) & 0xFF; frame[io + 3] = cs & 0xFF;
    cyw43_queue_rx_vnet(frame, off);
}

/* Periodic unsolicited RA (all-nodes multicast): guests whose stack
 * never sends RS (e.g. no timer pump) still learn the prefix via SLAAC.
 * Host-clocked; fake-net mode only (the real gateway sends its own). */
static uint64_t ndp_ra_next_ms = 0;
/* ND address-resolution wait window (wall ms, monotonic). Set to now+4s
 * whenever guest transmits an ICMPv6 Neighbor Solicitation; while active,
 * dual_core_step freezes WFE fast-forward so guest ND timers
 * (1s INCOMPLETE lifetime) can't outrun host-speed peer answers. */
uint64_t bramble_nd_wait_until_ms = 0;
/* Snoop guest WLAN TX for ICMPv6 Neighbor Solicitations. Address
 * resolution is a wall-time race in the emulator: the peer/bridge
 * answers in wall ms, but WFE fast-forward can advance emulated time
 * seconds in the same span, letting nd6_tmr free the INCOMPLETE entry
 * before the solicited NA is processed (NA then drops: "no longer
 * care", queued UDP never flushes). Arming a 4s wait window makes the
 * fast-forward path freeze (host polls still run) until the NA arrives. */
static void bramble_nd_snoop_tx(const uint8_t *eth, int eth_len) {
    if (eth_len < 14 + 40 + 8) return;
    if (eth[12] != 0x86 || eth[13] != 0xDD) return;  /* IPv6 */
    const uint8_t *ip6 = eth + 14;
    if ((ip6[0] >> 4) != 6 || ip6[6] != 58) return;  /* not ICMPv6 */
    if (ip6[40] != 135 || ip6[41] != 0) return;      /* NS, code 0 */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    bramble_nd_wait_until_ms =
        (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u + 4000;
}
void cyw43_ndp_ra_poll(void) {
    if (cyw43_no_fake_dhcp) return;
    if (cyw43.wifi_state != CYW43_WIFI_CONNECTED) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
    if (now < ndp_ra_next_ms) return;
    ndp_ra_next_ms = now + 7000;
    uint8_t mcast_mac[6] = {0x33, 0x33, 0, 0, 0, 1};
    cyw43_fake_send_ra(fake_ip6_mcast_all, mcast_mac);
}

static int cyw43_handle_fake_icmp6(const uint8_t *eth_frame, int eth_len) {
    if (eth_len < 14 + 40 + 8) return 0;
    if (eth_frame[12] != 0x86 || eth_frame[13] != 0xDD) return 0;  /* IPv6 */
    const uint8_t *ip6 = eth_frame + 14;
    if ((ip6[0] >> 4) != 6) return 0;
    if (ip6[6] != 58) return 0;  /* not ICMPv6 */
    const uint8_t *ic = ip6 + 40;
    int icmp_len = eth_len - 14 - 40;
    uint8_t type = ic[0];

    uint8_t frame[CYW43_MAX_FRAME_SIZE];
    int off, io;

    if (type == 133) {  /* Router Solicitation -> Advertisement */
        const uint8_t *dst_ip = ip6 + 8;
        uint8_t mcast_mac[6] = {0x33, 0x33, 0, 0, 0, 1};
        const uint8_t *rep_dst_mac = eth_frame + 6;
        if (memcmp(ip6 + 8, fake_ip6_unspec, 16) == 0) {
            dst_ip = fake_ip6_mcast_all;
            rep_dst_mac = mcast_mac;
        }
        cyw43_fake_send_ra(dst_ip, rep_dst_mac);
        return 1;
    }

    if (type == 135 && icmp_len >= 28) {  /* Neighbor Solicitation */
        const uint8_t *tgt = ic + 8;
        const uint8_t *gw = NULL;
        if (memcmp(tgt, fake_gw_ip6_ll, 16) == 0) gw = fake_gw_ip6_ll;
        else if (memcmp(tgt, fake_gw_ip6_ula, 16) == 0) gw = fake_gw_ip6_ula;
        if (!gw) return 0;
        /* NA body 4 (hdr) + 4 (flags) + target 16 + tgt-ll 8 = 32, S+O */
        off = cyw43_icmp6_frame_start(frame, eth_frame + 6, gw, ip6 + 8, 32);
        io = off;
        frame[off++] = 136; frame[off++] = 0;     /* NA */
        frame[off++] = 0; frame[off++] = 0;       /* cksum later */
        frame[off++] = 0x60;                      /* S + O */
        frame[off++] = 0; frame[off++] = 0; frame[off++] = 0;
        memcpy(frame + off, tgt, 16); off += 16;
        frame[off++] = 2; frame[off++] = 1;       /* target link-layer */
        memcpy(frame + off, fake_gw_mac, 6); off += 6;
        uint16_t cs = cyw43_icmp6_cksum(gw, ip6 + 8, frame + io, 32);
        frame[io + 2] = (cs >> 8) & 0xFF; frame[io + 3] = cs & 0xFF;
        cyw43_queue_rx_vnet(frame, off);
        return 1;
    }
    return 0;
}

/* Returns 1 if this was a DHCP packet that we handled, 0 otherwise. */
static int cyw43_handle_dhcp(const uint8_t *eth_frame, int eth_len) {    if (eth_len < 14 + 20 + 8 + 240) return 0;
    /* IPv4 only */
    if (eth_frame[12] != 0x08 || eth_frame[13] != 0x00) return 0;

    const uint8_t *ip = eth_frame + 14;
    if ((ip[0] >> 4) != 4) return 0;
    int ip_hlen = (ip[0] & 0x0F) * 4;
    if (ip_hlen < 20 || ip_hlen > 60) return 0;  /* L47: validate IHL */
    if (ip[9] != 17) return 0;          /* not UDP */

    const uint8_t *udp = ip + ip_hlen;
    uint16_t src_port  = ((uint16_t)udp[0] << 8) | udp[1];
    uint16_t dst_port  = ((uint16_t)udp[2] << 8) | udp[3];
    if (src_port != 68 || dst_port != 67) return 0;

    const uint8_t *dhcp = udp + 8;
    int dhcp_len = eth_len - 14 - ip_hlen - 8;
    if (dhcp_len < 240) return 0;
    /* Verify magic cookie */
    if (dhcp[236] != 0x63 || dhcp[237] != 0x82 ||
        dhcp[238] != 0x53 || dhcp[239] != 0x63) return 0;

    /* Scan options for message type (opt 53) */
    uint8_t msg_type = 0;
    for (int i = 240; i < dhcp_len; ) {
        uint8_t opt = dhcp[i++];
        if (opt == 255) break;
        if (opt == 0) continue;
        if (i >= dhcp_len) break;
        uint8_t olen = dhcp[i++];
        if (opt == 53 && olen == 1 && i < dhcp_len)
            msg_type = dhcp[i];
        i += olen;
    }

    if (msg_type == 1) {        /* DISCOVER → OFFER */
        cyw43_send_dhcp_reply(eth_frame, eth_len, 2);
        return 1;
    } else if (msg_type == 3) { /* REQUEST → ACK */
        cyw43_send_dhcp_reply(eth_frame, eth_len, 5);
        return 1;
    }
    return 0;
}

/* ========================================================================
 * WLAN TX Processing (firmware -> CYW43)
 * ======================================================================== */

static void cyw43_wlan_tx_word(uint32_t val) {
    if (cyw43.wlan_tx_offset + 4 <= CYW43_WLAN_TX_BUF_SIZE) {
        cyw43.wlan_tx_buf[cyw43.wlan_tx_offset++] = (val >>  0) & 0xFF;
        cyw43.wlan_tx_buf[cyw43.wlan_tx_offset++] = (val >>  8) & 0xFF;
        cyw43.wlan_tx_buf[cyw43.wlan_tx_offset++] = (val >> 16) & 0xFF;
        cyw43.wlan_tx_buf[cyw43.wlan_tx_offset++] = (val >> 24) & 0xFF;
    }
}

static void cyw43_wlan_tx_complete(void) {
    int len = cyw43.wlan_tx_offset;
    if (len < 12) {
        cyw43.wlan_tx_offset = 0;
        return;
    }

    sdpcm_header_t *sdpcm = (sdpcm_header_t *)cyw43.wlan_tx_buf;
    cyw43.last_fw_seq = sdpcm->sequence;

    uint8_t channel = sdpcm->channel_and_flags & 0x0F;

    if (CYW43_DBG)
        fprintf(stderr, "[CYW43] WLAN TX: size=%d channel=%d seq=%d\n",
                sdpcm->size, channel, sdpcm->sequence);

    switch (channel) {
    case SDPCM_CONTROL_CHANNEL:
        cyw43_handle_ioctl(cyw43.wlan_tx_buf, len);
        break;

    case SDPCM_DATA_CHANNEL: {
        /* Extract Ethernet frame: skip SDPCM(12) + pad(2) + BDC(4+extra) bytes */
        if (len > 18) {
            int eth_offset = 18;
            bdc_header_t *bdc = (bdc_header_t *)(cyw43.wlan_tx_buf + 14);
            eth_offset += bdc->data_offset * 4;

            int eth_len = len - eth_offset;
            if (eth_len > 0) {
                const uint8_t *eth = cyw43.wlan_tx_buf + eth_offset;
                bramble_nd_snoop_tx(eth, eth_len);
                /* Fake DHCP/DNS+ICMP (+NDP) unless -nodhcp (gateway provides them). */
                if (!cyw43_no_fake_dhcp &&
                    (cyw43_handle_dhcp(eth, eth_len) ||
                     cyw43_handle_fake_arp(eth, eth_len) ||
                     cyw43_handle_fake_icmp(eth, eth_len) ||
                     cyw43_handle_fake_icmp6(eth, eth_len))) {
                    /* Handled by fake server. */
                } else {
                    /* Real uplink: vnet bus (gateway/peer/TAP) ... */
                    if (vnet.enabled && cyw43.vnet_port >= 0)
                        vnet_tx_frame(cyw43.vnet_port, eth, eth_len);
                    /* ... and legacy TAP socket if open. */
                    if (cyw43.tap_fd >= 0) {
                        tapif_write(cyw43.tap_fd, eth, eth_len);
                        if (CYW43_DBG)
                            fprintf(stderr, "[CYW43] TAP TX: %d bytes\n", eth_len);
                    }
                }
            }
        }
        break;
    }

    default:
        if (cpu.debug_enabled)
            fprintf(stderr, "[CYW43] WLAN TX unknown channel %d\n", channel);
        break;
    }

    cyw43.wlan_tx_offset = 0;
}

/* Forward declarations for PIO gSPI state (defined in PIO section below) */
static int pio_init_swap_remaining;
static int pio_cmd_is_swap;

/* Forward declarations for backplane core wrapper state */
static uint8_t cyw43_wlan_resetctrl;
static uint8_t cyw43_wlan_ioctrl;
static uint8_t cyw43_socram_resetctrl;
static uint8_t cyw43_socram_ioctrl;

/* ========================================================================
 * Init / Reset
 * ======================================================================== */

void cyw43_init(void) {
    /* vnet_port has no valid zero state (0 is a live port); the BSS
     * zero must become -1 exactly once — reboot preserves the port. */
    static int booted = 0;
    if (!booted) { cyw43.vnet_port = -1; booted = 1; }
    cyw43_reset();

    /* Add default fake APs for testing */
    cyw43_add_scan_result("BrambleNet", -45, 6, 3);
    cyw43_add_scan_result("PicoTestAP", -60, 1, 3);
    cyw43_add_scan_result("OpenNetwork", -70, 11, 0);
}

void cyw43_reset(void) {
    int saved_enabled = cyw43.enabled;
    int saved_tap_fd = cyw43.tap_fd;
    int saved_vnet_port = cyw43.vnet_port;
    char saved_tap_name[32];
    memcpy(saved_tap_name, cyw43.tap_name, sizeof(saved_tap_name));

    memset(&cyw43, 0, sizeof(cyw43_state_t));

    cyw43.enabled = saved_enabled;
    cyw43.tap_fd = saved_tap_fd;
    cyw43.vnet_port = saved_vnet_port;
    memcpy(cyw43.tap_name, saved_tap_name, sizeof(cyw43.tap_name));

    memcpy(cyw43.mac_addr, default_mac, 6);
    if (has_preset_mac)
        memcpy(cyw43.mac_addr, preset_mac, 6);
    snprintf(cyw43.country, sizeof(cyw43.country), "XX");
    cyw43.wifi_state = CYW43_WIFI_OFF;
    cyw43.chipclkcsr = CYW43_HT_AVAIL | CYW43_ALP_AVAIL;
    cyw43.sleepcsr = 0x03;  /* KSO_SET | DEVICE_ON */
    cyw43.connect_pending_id = -1;  /* no join in flight */
    cyw43.pio_num = -1;
    cyw43.pio_sm = -1;
    pio_init_swap_remaining = 2;  /* First 2 commands use SWAP32 encoding */
    pio_cmd_is_swap = 0;

    /* Core wrappers start in reset (RESETCTRL=1 = AIRC_RESET) */
    cyw43_wlan_resetctrl = CYW43_AIRC_RESET;
    cyw43_wlan_ioctrl = 0;
    cyw43_socram_resetctrl = CYW43_AIRC_RESET;
    cyw43_socram_ioctrl = 0;
}

int cyw43_is_wifi_gpio(uint32_t gpio) {
    return cyw43.enabled && (gpio == WL_CS || gpio == WL_CLK || gpio == WL_DIO);
}

/* ========================================================================
 * TAP Bridge
 * ======================================================================== */

int cyw43_tap_open(const char *name) {
    cyw43.tap_fd = tapif_open(name);
    if (cyw43.tap_fd >= 0) {
        snprintf(cyw43.tap_name, sizeof(cyw43.tap_name), "%s", name);
        fprintf(stderr, "[CYW43] TAP bridge enabled on '%s'\n", name);
    }
    return cyw43.tap_fd;
}

void cyw43_tap_close(void) {
    if (cyw43.tap_fd >= 0) {
        tapif_close(cyw43.tap_fd);
        cyw43.tap_fd = -1;
    }
}

void cyw43_tap_poll(void) {
    if (cyw43.tap_fd < 0) return;
    if (cyw43.wifi_state != CYW43_WIFI_CONNECTED) return;

    /* Read Ethernet frames from TAP and queue for firmware.
     * L46: drain a burst per poll (like vnet's 16) to avoid backpressure. */
    uint8_t eth_buf[1522];  /* Max Ethernet frame + VLAN headroom */
    for (int burst = 0; burst < 16; burst++) {
        int n = tapif_read(cyw43.tap_fd, eth_buf, (int)sizeof(eth_buf));
        if (n > 0) {
            cyw43_queue_rx_vnet(eth_buf, n);
            if (cpu.debug_enabled)
                fprintf(stderr, "[CYW43] TAP RX: %d bytes queued\n", n);
        } else {
            if (n < 0) {
                /* Persistent error — close TAP to avoid spin-polling a dead fd */
                fprintf(stderr, "[CYW43] TAP read error, closing interface\n");
                tapif_close(cyw43.tap_fd);
                cyw43.tap_fd = -1;
            }
            break;
        }
    }
}

/* ========================================================================
 * Bus Register Access (Function 0)
 * ======================================================================== */

static uint32_t cyw43_bus_read(uint32_t addr) {
    switch (addr & 0xFF) {
    case 0x00: /* SPI_BUS_CONTROL */
        return cyw43.bus_ctrl;
    case 0x04: /* SPI_INTERRUPT_REGISTER - F2_PACKET_AVAILABLE=0x20 when queued */
    {
        uint32_t val = cyw43.bus_int;
        if (rx_queue_count() > 0)
            val |= CYW43_BUS_INT_F2_PKT_AVAIL;  /* = 0x20 = F2_PACKET_AVAILABLE */
        if (cpu.debug_enabled)
            fprintf(stderr, "[CYW43] INT_REG read: 0x%02X (q=%d)\n", val, rx_queue_count());
        return val;
    }
    case 0x08: /* SPI_STATUS_REGISTER - F2_RX_READY + packet info */
    {
        /* F2 (WLAN) is always ready in our emulation */
        uint32_t val = SPI_STATUS_F2_RX_READY;
        cyw43_rx_frame_t *f = rx_queue_peek();
        if (f && f->len > 0) {
            val |= SPI_STATUS_F2_PKT_AVAILABLE;
            val |= ((uint32_t)(f->len & 0x7FF)) << SPI_STATUS_F2_PKT_LEN_SHIFT;
        }
        if (cpu.debug_enabled)
            fprintf(stderr, "[CYW43] STATUS_REG read: 0x%08X (q=%d)\n", val, rx_queue_count());
        return val;
    }
    case 0x14: /* SPI_READ_TEST_REGISTER = FEEDBEAD */
        return 0xFEEDBEAD;
    case 0x18:
        return cyw43.bus_test_reg;
    default:
        if (cpu.debug_enabled)
            fprintf(stderr, "[CYW43] Bus read unknown reg 0x%02X\n", addr & 0xFF);
        return 0;
    }
}

static void cyw43_bus_write(uint32_t addr, uint32_t val) {
    switch (addr & 0xFF) {
    case CYW43_REG_BUS_CTRL:
        cyw43.bus_ctrl = val;
        if (cpu.debug_enabled)
            fprintf(stderr, "[CYW43] Bus ctrl = 0x%08X\n", val);
        break;
    case CYW43_REG_BUS_INTERRUPT:
        cyw43.bus_int &= ~val;  /* W1C */
        break;
    case CYW43_REG_BUS_INTMASK:
        cyw43.bus_intmask = val;
        break;
    case CYW43_REG_BUS_TEST:
        cyw43.bus_test_reg = val;
        if (cpu.debug_enabled)
            fprintf(stderr, "[CYW43] Test reg = 0x%08X\n", val);
        break;
    default:
        if (cpu.debug_enabled)
            fprintf(stderr, "[CYW43] Bus write unknown reg 0x%02X = 0x%08X\n",
                    addr & 0xFF, val);
        break;
    }
}

/* ========================================================================
 * BT HCI (shared-bus circular buffers in BT RAM)
 *
 * Layout (offsets from CYW43_BT_RAM_BASE): H2B data [0,0x1000),
 * B2H data [0x1000,0x2000), indices H2B_IN +0x2000, H2B_OUT +0x2004,
 * B2H_IN +0x2008, B2H_OUT +0x200C. Packets are [len_lo,len_hi,0,type]
 * + payload, padded to 4. The host writes H2B_IN after appending
 * commands; we answer synchronously (responses are already queued when
 * the host reads back), so no IRQ emulation is needed for bring-up.
 *
 * HCI FORWARDING (-bt-hci <sock>): instead of the internal responder,
 * guest H2B packets go out as H4 ([type]+payload, u32-LE-length-framed)
 * over a unix socket to a host controller (Bumble virtual controller
 * or BlueZ adapter via a bridge), and socket input is queued into the
 * B2H ring. bramble listens; the bridge connects. Up to 8 outbound
 * packets are stashed pre-connect and flushed on accept.
 * ======================================================================== */
static char bt_hci_sock_path[256];
static int bt_hci_listen_fd = -1;
static int bt_hci_fd = -1;
static uint8_t bt_hci_pend[8][1088];
static uint16_t bt_hci_pend_len[8];
static int bt_hci_pend_count = 0;
static uint8_t bt_hci_rxbuf[2048];
static int bt_hci_rxlen = 0;

/* JS/WASM uplink (no sockets in the browser): H4 ring drained by the
 * embedder via cyw43_bt_hci_js_pop, fed via cyw43_bt_hci_js_push. */
static int bt_hci_js_mode = 0;
static uint8_t bt_hci_js_tx[8][1088];
static uint16_t bt_hci_js_tx_len[8];
static int bt_hci_js_tx_head = 0, bt_hci_js_tx_tail = 0;

void cyw43_bt_hci_js_enable(int on) {
    bt_hci_js_mode = on ? 1 : 0;
    if (!on)
        bt_hci_js_tx_head = bt_hci_js_tx_tail = 0;
}

/* Drain one outbound H4 packet ([type]+payload) into out[]. Returns its
 * length, 0 when empty, -1 when it doesn't fit (retry bigger). */
int cyw43_bt_hci_js_pop(uint8_t *out, int maxlen) {
    if (bt_hci_js_tx_head == bt_hci_js_tx_tail) return 0;
    int len = bt_hci_js_tx_len[bt_hci_js_tx_tail];
    if (!out || len > maxlen) return -1;
    memcpy(out, bt_hci_js_tx[bt_hci_js_tx_tail], (size_t)len);
    bt_hci_js_tx_tail = (bt_hci_js_tx_tail + 1) % 8;
    return len;
}

/* Inject one inbound H4 packet ([type]+payload) into the B2H ring. */
void cyw43_bt_hci_js_push(const uint8_t *h4, int h4len) {
    if (!h4 || h4len < 2 || h4len > 1084) return;
    cyw43_bt_queue_hci(h4[0], h4 + 1, h4len - 1);
}

void cyw43_bt_hci_attach(const char *path) {
    if (!path || !path[0]) return;
    strncpy(bt_hci_sock_path, path, sizeof(bt_hci_sock_path) - 1);
    bt_hci_sock_path[sizeof(bt_hci_sock_path) - 1] = '\0';
}

static void bt_hci_set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl != -1)
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Write one framed H4 packet to the forwarder (1 = sent/stashed). */
static int bt_hci_forward(const uint8_t *h4, int h4len) {
    if (bt_hci_sock_path[0] == '\0') return 0;
    if (h4len < 1 || h4len > 1084) return 0;
    if (bt_hci_fd < 0) {
        /* No bridge yet: stash for post-accept flush (drop-new if full). */
        if (bt_hci_pend_count < 8) {
            memcpy(bt_hci_pend[bt_hci_pend_count], h4, (size_t)h4len);
            bt_hci_pend_len[bt_hci_pend_count] = (uint16_t)h4len;
            bt_hci_pend_count++;
            return 1;
        }
        return 0;
    }
    uint8_t hdr[4];
    uint32_t L = (uint32_t)h4len;
    hdr[0] = L & 0xFF; hdr[1] = (L >> 8) & 0xFF;
    hdr[2] = (L >> 16) & 0xFF; hdr[3] = (L >> 24) & 0xFF;
    /* Non-blocking best-effort: short writes are dropped (guest stacks
     * retry commands; events/ACL from a stalled bridge are expendable). */
    if (write(bt_hci_fd, hdr, 4) != 4) return 0;
    size_t off = 0;
    while (off < (size_t)h4len) {
        ssize_t n = write(bt_hci_fd, h4 + off, (size_t)h4len - off);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
            close(bt_hci_fd);
            bt_hci_fd = -1;
            return 0;
        }
        off += (size_t)n;
    }
    return 1;
}

/* Periodic pump: listen/accept + drain inbound H4 into the B2H ring. */
void cyw43_bt_hci_bridge_poll(void) {
#ifdef __EMSCRIPTEN__
    /* No unix sockets in the browser: the JS H4 ring (bt_hci_js_mode,
     * --ble-hci in cli.js / connectBleHci in index.html) is the bridge.
     * Inbound H4 is already queued by cyw43_bt_hci_js_push; nothing to
     * listen/accept/drain here. */
    return;
#else
    if (bt_hci_sock_path[0] == '\0') return;
    if (bt_hci_listen_fd < 0 && bt_hci_fd < 0) {
        int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (lfd >= 0) {
            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, bt_hci_sock_path,
                    sizeof(addr.sun_path) - 1);
            if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
                listen(lfd, 1) == 0) {
                bt_hci_set_nonblock(lfd);
                bt_hci_listen_fd = lfd;
                if (CYW43_DBG)
                    fprintf(stderr, "[CYW43] BT HCI forward: listening on %s\n",
                            bt_hci_sock_path);
            } else {
                close(lfd);
            }
        }
        if (bt_hci_listen_fd < 0) return;
    }
    if (bt_hci_fd < 0 && bt_hci_listen_fd >= 0) {
        int cfd = accept(bt_hci_listen_fd, NULL, NULL);
        if (cfd >= 0) {
            bt_hci_set_nonblock(cfd);
            bt_hci_fd = cfd;
            if (CYW43_DBG)
                fprintf(stderr, "[CYW43] BT HCI forward: bridge connected\n");
            /* Flush pre-connect backlog in order. */
            for (int i = 0; i < bt_hci_pend_count; i++) {
                uint8_t hdr[4];
                uint32_t L = bt_hci_pend_len[i];
                hdr[0] = L & 0xFF; hdr[1] = (L >> 8) & 0xFF;
                hdr[2] = (L >> 16) & 0xFF; hdr[3] = (L >> 24) & 0xFF;
                if (write(bt_hci_fd, hdr, 4) != 4) break;
                size_t off = 0;
                int ok = 1;
                while (off < L) {
                    ssize_t n = write(bt_hci_fd, bt_hci_pend[i] + off, L - off);
                    if (n <= 0) { ok = 0; break; }
                    off += (size_t)n;
                }
                if (!ok) break;
            }
            bt_hci_pend_count = 0;
        }
    }
    if (bt_hci_fd < 0) return;
    bt_hci_drain();
#endif
}

/* Read available socket bytes, queue complete H4 packets into B2H.
 * Returns packets queued (-1 if the bridge went away). Loops until
 * EAGAIN so a burst of replies (e.g. 8 back-to-back CCs) is fully
 * drained in one poll instead of one packet per host-poll tick. */
static int bt_hci_drain(void) {
    if (bt_hci_fd < 0) return -1;
    int queued = 0;
    for (;;) {
        ssize_t n = read(bt_hci_fd, bt_hci_rxbuf + bt_hci_rxlen,
                         sizeof(bt_hci_rxbuf) - (size_t)bt_hci_rxlen);
        if (n == 0) {
            if (CYW43_DBG)
                fprintf(stderr, "[CYW43] BT HCI forward: bridge gone\n");
            close(bt_hci_fd);
            bt_hci_fd = -1;
            bt_hci_rxlen = 0;
            return -1;
        }
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                close(bt_hci_fd);
                bt_hci_fd = -1;
                bt_hci_rxlen = 0;
                return -1;
            }
            return queued;
        }
        bt_hci_rxlen += (int)n;
        /* Extract framed H4 packets: [u32 LE len][type+payload]. */
        while (bt_hci_rxlen >= 4) {
            uint32_t L = (uint32_t)bt_hci_rxbuf[0] |
                         ((uint32_t)bt_hci_rxbuf[1] << 8) |
                         ((uint32_t)bt_hci_rxbuf[2] << 16) |
                         ((uint32_t)bt_hci_rxbuf[3] << 24);
            if (L < 1 || L > 1084) {  /* desync: drop everything */
                bt_hci_rxlen = 0;
                return queued;
            }
            if (bt_hci_rxlen < 4 + (int)L) break;  /* incomplete: read more */
            cyw43_bt_queue_hci(bt_hci_rxbuf[4], bt_hci_rxbuf + 5, (int)L - 1);
            queued++;
            int total = 4 + (int)L;
            if (bt_hci_rxlen > total)
                memmove(bt_hci_rxbuf, bt_hci_rxbuf + total,
                        (size_t)(bt_hci_rxlen - total));
            bt_hci_rxlen -= total;
        }
    }
}

/* Forward one H4 packet and, for HCI commands, wait briefly (wall time)
 * for the controller's reply so it lands synchronously like the internal
 * responder's. Guest clocks can otherwise outrun real controllers and
 * time out before answers arrive. */
static void bt_hci_forward_sync(const uint8_t *h4, int h4len) {
    bt_hci_forward(h4, h4len);
    if (h4len < 1 || h4[0] != 0x01 || bt_hci_fd < 0) return;
    for (int i = 0; i < 40; i++) {
        struct pollfd pfd;
        pfd.fd = bt_hci_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int r = poll(&pfd, 1, 10);
        if (r < 0) return;
        if (r == 0) continue;
        if (bt_hci_drain() != 0) return;  /* got ≥1 (or bridge gone) */
    }
}

/* Forward one ring-side HCI packet (payload WITHOUT the H4 type byte)
 * with an explicit type, used when the internal command responder must
 * defer to the external controller (e.g. LE_Create_Connection). */
static void bt_hci_forward_sync_ext(const uint8_t *payload, int paylen,
                                    uint8_t type) {
    uint8_t h4[264 + 1];
    int n = paylen > 264 ? 264 : paylen;
    h4[0] = type;
    for (int i = 0; i < n; i++) h4[1 + i] = payload[i];
    bt_hci_forward(h4, 1 + n);
    if (type != 0x01 || bt_hci_fd < 0) return;
    for (int i = 0; i < 40; i++) {
        struct pollfd pfd;
        pfd.fd = bt_hci_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int r = poll(&pfd, 1, 10);
        if (r < 0) return;
        if (r == 0) continue;
        if (bt_hci_drain() != 0) return;
    }
}
static uint32_t bt_ram_rd32(uint32_t off) {
    return (uint32_t)cyw43.bt_ram[off] |
           ((uint32_t)cyw43.bt_ram[off + 1] << 8) |
           ((uint32_t)cyw43.bt_ram[off + 2] << 16) |
           ((uint32_t)cyw43.bt_ram[off + 3] << 24);
}

static void bt_ram_wr32(uint32_t off, uint32_t v) {
    cyw43.bt_ram[off] = v & 0xFF;
    cyw43.bt_ram[off + 1] = (v >> 8) & 0xFF;
    cyw43.bt_ram[off + 2] = (v >> 16) & 0xFF;
    cyw43.bt_ram[off + 3] = (v >> 24) & 0xFF;
}

/* Append a packet to the BT->host ring and flag FC_CHANGE. Payload
 * EXCLUDES the H4 type byte (the ring header carries it in hdr[3],
 * mirroring the host->BT direction); pass it separately. */
static void cyw43_bt_queue_hci(uint8_t pkt_type, const uint8_t *payload,
                               int paylen) {
    int pktlen = 4 + ((paylen + 3) & ~3);
    uint32_t pos = cyw43.bt_b2h_in & 0xFFF;
    uint8_t hdr[4] = { paylen & 0xFF, (paylen >> 8) & 0xFF, 0x00, pkt_type };
    for (int i = 0; i < 4; i++)
        cyw43.bt_ram[0x1000 + ((pos + i) & 0xFFF)] = hdr[i];
    for (int i = 0; i < paylen; i++)
        cyw43.bt_ram[0x1000 + ((pos + 4 + i) & 0xFFF)] = payload[i];
    for (int i = 4 + paylen; i < pktlen; i++)
        cyw43.bt_ram[0x1000 + ((pos + i) & 0xFFF)] = 0;
    cyw43.bt_b2h_in = (cyw43.bt_b2h_in + (uint32_t)pktlen) & 0xFFF;
    bt_ram_wr32(0x2008, cyw43.bt_b2h_in);
    cyw43.bt_int_status |= CYW43_BT_FC_CHANGE;
    if (CYW43_DBG)
        fprintf(stderr, "[CYW43] BT B2H queued type=%u paylen=%d in=%u out=%u\n",
                pkt_type, paylen, cyw43.bt_b2h_in,
                (unsigned)(bt_ram_rd32(0x200C) & 0xFFF));
    cyw43_update_irq();  /* shared wake line for BT too */
}

static void cyw43_bt_cmd_complete(uint16_t opcode, const uint8_t *params,
                                  int plen) {
    /* HCI Event packet WITHOUT the H4 type byte: [0E, len, ncmd,
     * opcode_lo, opcode_hi, status, params...]; len covers ncmd (1) +
     * opcode (2) + status/params (1+plen). */
    uint8_t ev[40];
    if (plen > 32) plen = 32;
    ev[0] = 0x0E; ev[1] = (uint8_t)(4 + plen); ev[2] = 0x01;
    ev[3] = opcode & 0xFF; ev[4] = (opcode >> 8) & 0xFF; ev[5] = 0x00;
    for (int i = 0; i < plen; i++) ev[6 + i] = params[i];
    cyw43_bt_queue_hci(0x04, ev, 6 + plen);
}

/* Broadcast our ADV payload to the vnet room (ethertype 0x88B5) so
 * scanning peers can synthesize LE Advertising Reports. */
static void cyw43_bt_adv_announce(void) {
    if (CYW43_DBG)
        fprintf(stderr, "[CYW43] BT ADV enabled (%dB)\n", cyw43.bt_adv_data_len);
    if (!vnet.enabled || cyw43.vnet_port < 0)
        return;
    uint8_t f[14 + 38];
    memset(f, 0xFF, 6);
    memcpy(f + 6, cyw43.mac_addr, 6);
    f[12] = 0x88; f[13] = 0xB5;
    memcpy(f + 14, cyw43.mac_addr, 6);
    f[20] = (uint8_t)cyw43.bt_adv_data_len;
    memcpy(f + 21, cyw43.bt_adv_data, (size_t)cyw43.bt_adv_data_len);
    vnet_tx_frame(cyw43.vnet_port, f, sizeof(f));
}

/* Periodic ADV beacon while advertising (real advertisers repeat theirs;
 * also heals races where a peer attaches after the enable announce). */
static uint64_t bt_adv_next_ms = 0;
static uint64_t cyw43_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
void cyw43_bt_beacon_poll(void) {
    if (!cyw43.bt_adv_enabled) return;
    if (!vnet.enabled || cyw43.vnet_port < 0) return;
    uint64_t now = cyw43_now_ms();
    if (now < bt_adv_next_ms) return;
    bt_adv_next_ms = now + 2000; /* every 2s wall */
    cyw43_bt_adv_announce();
}

/* ---- Minimal internal GATT/ATT link layer (no external controller).
 * MicroPython's btstack keeps the whole ATT DB host-side; the ONLY thing a
 * central needs from the controller is an LE-U ACL data path: HCI ACL
 * packets both ways + connection lifecycle events. We model one virtual
 * central:
 *  - vnet ethertype 0x88B6 carries LE link frames between instances
 *    (CONNECT_REQ / ATT request / ATT response / DISCONNECT),
 *  - guest side raises LE Connection Complete (0x3E/0x01) on accept and
 *    delivers inbound ATT as HCI ACL (type 0x02, L2CAP CID 4),
 *  - guest->controller ATT responses go out as HCI ACL, translated back
 *    onto the room bus.
 * ATT served from a tiny static DB (GAP 0x1800/device-name 0x2A00
 * "Bramble", GATT 0x1801/service-changed, plus RW scratch for handles
 * MP registers): MTU exchange, Read Req/Blob, Write Req/Cmd, Find Info,
 * Read-By-Type/Group. Enough for gatts_register_services + gatts_write +
 * gap_advertise E2E between two instances.
 * STATE LIVES HERE (file-static): cyw43_state_t is memset-zeroed on
 * reset, so per-link/DB state must NOT live there. */
#define CYW43_BT_ATT_MTU_DEFAULT 23
#define CYW43_BT_GATT_HANDLE_BASE 0x0010
/* Legacy single-link mirror (link 0 fast path). The multi-link table
 * below is authoritative; this stays in sync via bt_gatt_sync_link0(). */
static struct {
    int active;
    uint16_t handle;
    uint8_t peer[6];
} bt_gatt_link;
/* ATT attribute row. CCCD rows (uuid 0x2902) gate server-initiated
 * Handle-Value Notifications/Indications per bonded central. */
static struct {
    uint16_t handle;
    uint16_t uuid;
    uint8_t props;
    uint8_t value[32];
    uint8_t vlen;
} bt_gatt_attrs[16];
static int bt_gatt_nattrs;
static uint16_t bt_gatt_next_handle;
/* Multi-link: up to 4 concurrent virtual LE-U links. Link 0 is the
 * loopback/default (HCI handle 0x0042); peer links allocate 0x0043+.
 * The ATT DB is shared; per-link state (CCCD arm, MTU, indication
 * outstanding, long-write staging, notify slot) is per-link so two
 * centrals don't clobber each other. The legacy single-link statics
 * (bt_gatt_link etc.) mirror link 0 for the existing fast paths.
 * (struct + storage defined once here; the room-RX path above only
 * holds extern fwd declarations.) */
struct bt_gatt_link_state bt_gatt_links[BT_GATT_MAX_LINKS];
/* Link-table helpers: find by peer MAC / by HCI handle / free slot. */
static int bt_gatt_link_by_peer(const uint8_t *peer) {
    for (int i = 0; i < BT_GATT_MAX_LINKS; i++)
        if (bt_gatt_links[i].active && !memcmp(bt_gatt_links[i].peer, peer, 6))
            return i;
    return -1;
}
static int bt_gatt_link_by_handle(uint16_t h) {
    for (int i = 0; i < BT_GATT_MAX_LINKS; i++)
        if (bt_gatt_links[i].active && bt_gatt_links[i].handle == h)
            return i;
    return -1;
}
static int bt_gatt_link_alloc(const uint8_t *peer) {
    int old = bt_gatt_link_by_peer(peer);
    if (old >= 0) return old;
    for (int i = 0; i < BT_GATT_MAX_LINKS; i++)
        if (!bt_gatt_links[i].active) {
            memset(&bt_gatt_links[i], 0, sizeof(bt_gatt_links[i]));
            bt_gatt_links[i].active = 1;
            bt_gatt_links[i].handle = BT_GATT_BASE_HANDLE + i;
            memcpy(bt_gatt_links[i].peer, peer, 6);
            bt_gatt_links[i].peer_mtu = CYW43_BT_ATT_MTU_DEFAULT;
            return i;
        }
    return -1;
}
/* Keep the legacy link-0 mirror in sync with the table. */
static void bt_gatt_sync_link0(void) {
    bt_gatt_link.active = bt_gatt_links[0].active;
    bt_gatt_link.handle = bt_gatt_links[0].handle;
    memcpy(bt_gatt_link.peer, bt_gatt_links[0].peer, 6);
}
static void bt_gatt_link_raise_complete(int li) {
    uint8_t ev[19];
    ev[0] = 0x3E; ev[1] = 18; ev[2] = 0x01; ev[3] = 0x00;
    ev[4] = bt_gatt_links[li].handle & 0xFF;
    ev[5] = (bt_gatt_links[li].handle >> 8) & 0xFF;
    ev[6] = 0x00;                            /* role: central (we initiated) */
    memcpy(ev + 7, bt_gatt_links[li].peer, 6);
    ev[13] = 0; ev[14] = 0; ev[15] = 0; ev[16] = 0;
    ev[17] = 0; ev[18] = 0;
    cyw43_bt_queue_hci(0x04, ev, 19);
    if (CYW43_DBG)
        fprintf(stderr, "[CYW43] BT GATT link up peer=%02X:%02X:%02X:%02X:%02X:%02X h=%04x\n",
                bt_gatt_links[li].peer[0], bt_gatt_links[li].peer[1],
                bt_gatt_links[li].peer[2], bt_gatt_links[li].peer[3],
                bt_gatt_links[li].peer[4], bt_gatt_links[li].peer[5],
                bt_gatt_links[li].handle);
}
/* Notify/indicate outbox (one slot): queued by bt_gatt_notify(), drained
 * by the next cyw43_bt_hci_poll() as an ATT Handle-Value Notification
 * (0x1B) or Indication (0x1D) HCI ACL on the live link. Indications arm
 * bt_gatt_ind_pending until the peer's Handle-Value Confirmation (0x1E). */
static uint8_t bt_gatt_ntf_value[32];
static uint8_t bt_gatt_ntf_vlen;
static uint16_t bt_gatt_ntf_handle;
static int bt_gatt_ntf_indicate;
static int bt_gatt_ntf_queued;
static int bt_gatt_ind_pending;
/* Long-write staging (Prepare 0x16 / Execute 0x18): one outstanding
 * sequence per link (single-link responder). */
static uint8_t bt_gatt_prep_buf[32];
static uint16_t bt_gatt_prep_handle;
static int bt_gatt_prep_len;

static void bt_gatt_db_reset(void) {
    /* Attribute layout (handles 0x0010-0x0014):
     *   0x0010  GAP primary service 0x2800 = 0x1800
     *   0x0011  device-name char 0x2A00 "Bramble" (read 0x02)
     *   0x0012  scratch char 0x2A01 "??" (write 0x08 + read 0x02)
     *   0x0013  scratch CCCD 0x2902 = 00:00 (notify/indicate gate)
     *   0x0014  notify/indicate source char 0x2A01 (notify 0x10 +
     *           indicate 0x20 + read 0x02), value mirrors 0x0012 so
     *           Write -> notify/indicate -> Read-back round-trips.
     * MP-registered handles / loopback write+read-back tests use 0x0012. */
    bt_gatt_nattrs = 0;
    bt_gatt_next_handle = CYW43_BT_GATT_HANDLE_BASE;
    bt_gatt_attrs[0].handle = bt_gatt_next_handle++;
    bt_gatt_attrs[0].uuid = 0x2800;
    bt_gatt_attrs[0].props = 0;
    bt_gatt_attrs[0].value[0] = 0x00; bt_gatt_attrs[0].value[1] = 0x18;
    bt_gatt_attrs[0].vlen = 2;
    bt_gatt_attrs[1].handle = bt_gatt_next_handle++;
    bt_gatt_attrs[1].uuid = 0x2A00;
    bt_gatt_attrs[1].props = 0x02;
    memcpy(bt_gatt_attrs[1].value, "Bramble", 7);
    bt_gatt_attrs[1].vlen = 7;
    bt_gatt_attrs[2].handle = bt_gatt_next_handle++;
    bt_gatt_attrs[2].uuid = 0x2A01;
    bt_gatt_attrs[2].props = 0x08 | 0x02;
    memcpy(bt_gatt_attrs[2].value, "??", 2);
    bt_gatt_attrs[2].vlen = 2;
    bt_gatt_attrs[3].handle = bt_gatt_next_handle++;
    bt_gatt_attrs[3].uuid = 0x2902;
    bt_gatt_attrs[3].props = 0x08 | 0x02;  /* CCCD is itself writable */
    bt_gatt_attrs[3].value[0] = 0; bt_gatt_attrs[3].value[1] = 0;
    bt_gatt_attrs[3].vlen = 2;
    bt_gatt_attrs[4].handle = bt_gatt_next_handle++;
    bt_gatt_attrs[4].uuid = 0x2A01;
    bt_gatt_attrs[4].props = 0x10 | 0x20 | 0x02;
    memcpy(bt_gatt_attrs[4].value, "??", 2);
    bt_gatt_attrs[4].vlen = 2;
    bt_gatt_nattrs = 5;
    bt_gatt_ntf_queued = 0;
    bt_gatt_ind_pending = 0;
    bt_gatt_prep_handle = 0; bt_gatt_prep_len = 0;
}

/* vnet-RX callbacks (cyw43_vnet_rx is defined before this point). */
/* Multi-link helpers: fwd declarations were hoisted to the room-RX path
 * above; definitions live here with the table storage. */
void bt_gatt_link_up(const uint8_t *peer);
void bt_gatt_link_down(void);
int bt_gatt_link_is_up(void);

void bt_gatt_link_up(const uint8_t *peer) {
    int li = bt_gatt_link_alloc(peer);
    if (li < 0) return;  /* table full: refuse (no CC raised) */
    bt_gatt_db_reset();
    /* Reset per-link volatile state on (re)connect. */
    bt_gatt_links[li].cccd_cfg = 0;
    bt_gatt_links[li].ind_pending = 0;
    bt_gatt_links[li].ntf_queued = 0;
    bt_gatt_links[li].prep_handle = 0; bt_gatt_links[li].prep_len = 0;
    bt_gatt_links[li].peer_mtu = CYW43_BT_ATT_MTU_DEFAULT;
    bt_gatt_sync_link0();
    bt_gatt_link_raise_complete(li);
}

void bt_gatt_link_down(void) {
    if (!bt_gatt_link.active) return;
    bt_gatt_link.active = 0;
    bt_gatt_links[0].active = 0;
    bt_gatt_links[0].ntf_queued = 0;
    bt_gatt_links[0].ind_pending = 0;
    bt_gatt_links[0].prep_handle = 0; bt_gatt_links[0].prep_len = 0;
    bt_gatt_ntf_queued = 0;
    bt_gatt_ind_pending = 0;
    bt_gatt_prep_handle = 0; bt_gatt_prep_len = 0;
    uint8_t ev[4] = { 0x05, 3, 0x42, 0x00 };
    ev[3] = 0x13;  /* remote user terminated */
    cyw43_bt_queue_hci(0x04, ev, 4);
}

/* Drop one peer link by MAC (room DISCONNECT): per-link teardown with
 * that link's own HCI handle in the Disconnection Complete event. */
static void bt_gatt_link_down_peer(const uint8_t *peer) {
    int li = bt_gatt_link_by_peer(peer);
    if (li < 0) return;
    uint16_t h = bt_gatt_links[li].handle;
    memset(&bt_gatt_links[li], 0, sizeof(bt_gatt_links[li]));
    bt_gatt_sync_link0();
    uint8_t ev[4] = { 0x05, 3, h & 0xFF, (h >> 8) & 0xFF };
    ev[3] = 0x13;
    cyw43_bt_queue_hci(0x04, ev, 4);
}

/* Test hook (unit tests only): write the scratch CCCD 0x0013 directly.
 * Returns 1 if the CCCD row exists. */
int bt_gatt_test_cccd_write(uint16_t v) {
    return bt_gatt_test_cccd_write_on(0, v);
}
int bt_gatt_test_cccd_write_on(int li, uint16_t v) {
    if (li < 0) li = 0;
    if (li >= BT_GATT_MAX_LINKS || !bt_gatt_links[li].active) return 0;
    bt_gatt_links[li].cccd_cfg = v;
    /* Mirror ONLY for link 0 (loopback fast path shares the DB row). */
    if (li == 0) {
        for (int i = 0; i < bt_gatt_nattrs; i++)
            if (bt_gatt_attrs[i].uuid == 0x2902) {
                bt_gatt_attrs[i].value[0] = v & 0xFF;
                bt_gatt_attrs[i].value[1] = (v >> 8) & 0xFF;
                bt_gatt_attrs[i].vlen = 2;
                return 1;
            }
        return 0;
    }
    return 1;
}
/* Test hooks: per-link handle/MTU + raw ATT dispatch (error-path cover). */
int bt_gatt_test_link_handle(int li) {
    if (li < 0 || li >= BT_GATT_MAX_LINKS || !bt_gatt_links[li].active)
        return -1;
    return bt_gatt_links[li].handle;
}
void bt_gatt_test_set_mtu(int li, uint16_t mtu) {
    if (li < 0 || li >= BT_GATT_MAX_LINKS) return;
    bt_gatt_links[li].peer_mtu = mtu;
}
int bt_gatt_test_get_mtu(int li) {
    if (li < 0 || li >= BT_GATT_MAX_LINKS || !bt_gatt_links[li].active)
        return -1;
    return bt_gatt_links[li].peer_mtu;
}
int bt_gatt_test_att(const uint8_t *pdu, int len) {
    if (!bt_gatt_links[0].active) return 0;
    bt_gatt_handle_room_att_on(0, pdu, len, cyw43.mac_addr);
    return 1;
}

/* CCCD lookup: in our DB the scratch CCCD at 0x0013 *precedes* its
 * notify source at 0x0014 (standard layouts put it after; accept both
 * neighbors so either ordering arms the value handle). Per-link arm
 * bits live in the link table (bt_gatt_links[li].cccd_cfg); the shared
 * DB row mirrors link 0 for the loopback fast path. */
static int bt_gatt_cccd_on(int li, uint16_t h_val, int indicate) {
    uint16_t cfg;
    if (li < 0) li = 0;
    if (li >= BT_GATT_MAX_LINKS || !bt_gatt_links[li].active) return 0;
    cfg = bt_gatt_links[li].cccd_cfg;
    /* Fall back to the shared DB row (loopback writes land there). */
    if (!cfg) {
        for (int i = 0; i < bt_gatt_nattrs; i++) {
            if (bt_gatt_attrs[i].uuid == 0x2902 &&
                (bt_gatt_attrs[i].handle == (uint16_t)(h_val + 1) ||
                 bt_gatt_attrs[i].handle == (uint16_t)(h_val - 1)) &&
                bt_gatt_attrs[i].vlen >= 2) {
                cfg = bt_gatt_attrs[i].value[0] |
                      ((uint16_t)bt_gatt_attrs[i].value[1] << 8);
                break;
            }
        }
    }
    return indicate ? ((cfg & 0x0002) != 0) : ((cfg & 0x0001) != 0);
}
static int bt_gatt_cccd_enabled(uint16_t h_val, int indicate) {
    return bt_gatt_cccd_on(0, h_val, indicate);
}

/* CCCD probe (unit-test only): expose bt_gatt_cccd_enabled. */
int bt_gatt_cccd_probe(uint16_t h, int ind) {
    return bt_gatt_cccd_enabled(h, ind);
}

/* Debug dump (unit-test probe): print DB rows + notify slot to stderr. */
void bt_gatt_debug_dump(void) {
    fprintf(stderr, "[GATT-DB] nattrs=%d link=%d queued=%d indpend=%d\n",
            bt_gatt_nattrs, bt_gatt_link.active,
            bt_gatt_ntf_queued, bt_gatt_ind_pending);
    for (int i = 0; i < bt_gatt_nattrs; i++)
        fprintf(stderr, "[GATT-DB] [%d] h=%04x uuid=%04x props=%02x vlen=%d val=%02x%02x\n",
                i, bt_gatt_attrs[i].handle, bt_gatt_attrs[i].uuid,
                bt_gatt_attrs[i].props, bt_gatt_attrs[i].vlen,
                bt_gatt_attrs[i].value[0],
                bt_gatt_attrs[i].vlen > 1 ? bt_gatt_attrs[i].value[1] : 0);
}

int bt_gatt_link_is_up(void) {
    return bt_gatt_link.active;
}

static const void *bt_gatt_attr_by_handle(uint16_t h, int *idx_out) {
    for (int i = 0; i < bt_gatt_nattrs; i++)
        if (bt_gatt_attrs[i].handle == h) {
            if (idx_out) *idx_out = i;
            return &bt_gatt_attrs[i];
        }
    return NULL;
}

/* Queue one HCI ACL packet (type 0x02) toward the guest: L2CAP header
 * (len + CID 4) + ATT payload, connection handle in PB/BC flags. */
static void bt_gatt_send_acl(uint16_t conn, const uint8_t *att, int att_len) {
    uint8_t acl[4 + 4 + 64];
    int l2len = 4 + att_len;
    acl[0] = conn & 0xFF; acl[1] = ((conn >> 8) & 0x0F) | 0x20; /* PB=10 start */
    acl[2] = l2len & 0xFF; acl[3] = (l2len >> 8) & 0xFF;
    acl[4] = att_len & 0xFF; acl[5] = (att_len >> 8) & 0xFF;
    acl[6] = 0x04; acl[7] = 0x00;                              /* CID 4 = ATT */
    if (att_len > 0) memcpy(acl + 8, att, (size_t)att_len);
    cyw43_bt_queue_hci(0x02, acl, 8 + att_len);
}

/* ATT Error Response helper. */
static void bt_gatt_send_error(uint16_t conn, uint8_t req_op, uint16_t handle,
                               uint8_t err) {
    uint8_t r[5] = { 0x01, req_op, handle & 0xFF, (handle >> 8) & 0xFF, err };
    bt_gatt_send_acl(conn, r, 5);
}

/* Server-side emit: queue one Handle-Value Notification (0x1B) or
 * Indication (0x1D) for value handle h with payload v/vn. Returns 1 if
 * queued (link up, CCCD armed, no indication outstanding, no slot busy),
 * 0 otherwise. The poll loop drains the slot as an HCI ACL. Indications
 * arm ind_pending until the peer's Confirmation (0x1E). Link index li<0
 * means "link 0" (legacy callers). */
int bt_gatt_notify_on(int li, uint16_t h, const uint8_t *v, int vn, int indicate) {
    int idx = -1;
    if (li < 0) li = 0;
    if (li >= BT_GATT_MAX_LINKS || !bt_gatt_links[li].active) return 0;
    if (!bt_gatt_attr_by_handle(h, &idx)) return 0;
    if (indicate && bt_gatt_links[li].ind_pending) return 0;
    if (bt_gatt_links[li].ntf_queued) return 0;
    if (!bt_gatt_cccd_on(li, h, indicate)) return 0;
    if (vn < 0) vn = 0;
    if (vn > 32) vn = 32;
    if (vn > 0 && v) memcpy(bt_gatt_links[li].ntf_value, v, (size_t)vn);
    bt_gatt_links[li].ntf_vlen = (uint8_t)vn;
    bt_gatt_links[li].ntf_handle = h;
    bt_gatt_links[li].ntf_indicate = indicate ? 1 : 0;
    bt_gatt_links[li].ntf_queued = 1;
    if (indicate) bt_gatt_links[li].ind_pending = 1;
    /* Legacy mirror (link 0 fast path). */
    if (li == 0) {
        if (vn > 0 && v) memcpy(bt_gatt_ntf_value, v, (size_t)vn);
        bt_gatt_ntf_vlen = (uint8_t)vn;
        bt_gatt_ntf_handle = h;
        bt_gatt_ntf_indicate = indicate ? 1 : 0;
        bt_gatt_ntf_queued = 1;
        if (indicate) bt_gatt_ind_pending = 1;
    }
    return 1;
}
int bt_gatt_notify(uint16_t h, const uint8_t *v, int vn, int indicate) {
    return bt_gatt_notify_on(0, h, v, vn, indicate);
}

/* Drain the notify/indicate outbox as one HCI ACL (called from the poll
 * loop so server emits interleave safely with request/response traffic).
 * Also routes a copy onto the room bus so a peer central (two-instance
 * GATT) sees the server emit, not just the local guest. Drains ALL
 * active links (per-link slots), link 0 first. */
static void bt_gatt_poll_notify(void) {
    uint8_t pdu[1 + 2 + 32];
    for (int li = 0; li < BT_GATT_MAX_LINKS; li++) {
        if (!bt_gatt_links[li].ntf_queued || !bt_gatt_links[li].active)
            continue;
        pdu[0] = bt_gatt_links[li].ntf_indicate ? 0x1D : 0x1B;
        pdu[1] = bt_gatt_links[li].ntf_handle & 0xFF;
        pdu[2] = (bt_gatt_links[li].ntf_handle >> 8) & 0xFF;
        if (bt_gatt_links[li].ntf_vlen)
            memcpy(pdu + 3, bt_gatt_links[li].ntf_value,
                   bt_gatt_links[li].ntf_vlen);
        bt_gatt_links[li].ntf_queued = 0;
        if (li == 0) bt_gatt_ntf_queued = 0;
        bt_gatt_send_acl(bt_gatt_links[li].handle,
                         pdu, 3 + bt_gatt_links[li].ntf_vlen);
        bt_gatt_route_room_att_on(li, pdu, 3 + bt_gatt_links[li].ntf_vlen);
    }
    if (!bt_gatt_ntf_queued || !bt_gatt_link.active) return;
    pdu[0] = bt_gatt_ntf_indicate ? 0x1D : 0x1B;
    pdu[1] = bt_gatt_ntf_handle & 0xFF;
    pdu[2] = (bt_gatt_ntf_handle >> 8) & 0xFF;
    if (bt_gatt_ntf_vlen) memcpy(pdu + 3, bt_gatt_ntf_value, bt_gatt_ntf_vlen);
    bt_gatt_ntf_queued = 0;
    bt_gatt_send_acl(bt_gatt_link.handle ? bt_gatt_link.handle : 0x0042,
                     pdu, 3 + bt_gatt_ntf_vlen);
    bt_gatt_route_room_att(pdu, 3 + bt_gatt_ntf_vlen);
}

/* ATT opcode classes: server responses (incl. notifications) vs
 * client requests. Room frames carrying a response opcode are the
 * peer-server's answer routed back to the requesting central. */
static int bt_gatt_att_is_response(uint8_t op) {
    switch (op) {
    case 0x01: case 0x03: case 0x05: case 0x07: case 0x09: case 0x0B:
    case 0x0D: case 0x0F: case 0x11: case 0x13: case 0x17: case 0x19:
    case 0x1B: case 0x1D:
        return 1;
    default:
        return 0;
    }
}

/* Deliver a peer-server ATT response/notify as HCI ACL to OUR guest
 * (it is the GATT client that requested it). Link index selects the
 * HCI handle + per-link indication state. */
static void bt_gatt_deliver_room_resp(int li, const uint8_t *att, int att_len) {
    uint8_t resp[64];
    if (att_len < 1) return;
    if (li < 0) li = 0;
    if (li >= BT_GATT_MAX_LINKS || !bt_gatt_links[li].active) return;
    int n = att_len > 60 ? 60 : att_len;
    memcpy(resp, att, (size_t)n);
    if (att[0] == 0x1D) {
        /* Peer indication: our stack must confirm (spec: exactly one
         * outstanding indication per link). Confirmation goes back on
         * the room bus to the peer server. */
        uint8_t cfm[1] = { 0x1E };
        bt_gatt_route_room_att_on(li, cfm, 1);
    }
    bt_gatt_send_acl(bt_gatt_links[li].handle, resp, n);
}

/* Route one ATT PDU onto the room bus addressed to the link peer
 * (dst = peer MAC, src = our MAC, ethertype 0x88B6). Used for client
 * requests issued when the peer (not loopback) owns the server DB,
 * indication confirmations, and server emits. No-op without vnet.
 * _on(li) targets one link; unqualified = link 0 (legacy callers). */
static void bt_gatt_route_room_att_on(int li, const uint8_t *att, int att_len) {
    uint8_t f[14 + 6 + 1 + 64];
    if (!vnet.enabled || cyw43.vnet_port < 0) return;
    if (li < 0) li = 0;
    if (li >= BT_GATT_MAX_LINKS || !bt_gatt_links[li].active) return;
    if (att_len < 1 || att_len > 64) return;
    memcpy(f, bt_gatt_links[li].peer, 6);
    memcpy(f + 6, cyw43.mac_addr, 6);
    f[12] = 0x88; f[13] = 0xB6;
    memcpy(f + 14, bt_gatt_links[li].peer, 6);
    f[20] = (uint8_t)att_len;
    memcpy(f + 21, att, (size_t)att_len);
    vnet_tx_frame(cyw43.vnet_port, f, 21 + att_len);
}
static void bt_gatt_route_room_att(const uint8_t *att, int att_len) {
    bt_gatt_route_room_att_on(0, att, att_len);
}

/* Serve one ATT request PDU from the guest-as-central... no: from the
 * ROOM central (peer instance acting as GATT client). Responses go back
 * as HCI ACL so the guest btstack stack can complete its ATT client ops;
 * requests the guest itself emits (as server, type 0x02 H2B ACL) are
 * handled by bt_gatt_handle_local_acl() below. Shared by the room path
 * and the H2B loopback path (same DB, same wire format). Link index
 * li<0 = link 0 (loopback fast path): MTU/CCCD/prep state is per-link. */
static void bt_gatt_handle_room_att_on(int li, const uint8_t *att, int att_len,
                                       const uint8_t *src_mac) {
    uint16_t conn;
    if (li < 0) li = 0;
    if (li >= BT_GATT_MAX_LINKS || !bt_gatt_links[li].active) return;
    conn = bt_gatt_links[li].handle;
    if (att_len < 1) return;
    uint8_t op = att[0];
    uint8_t resp[64];
    int rlen = 0;
    switch (op) {
    case 0x02: { /* Exchange MTU Request: record per-link, answer 64. */
        uint16_t peer_mtu = att_len >= 3 ? (att[1] | (att[2] << 8)) : 23;
        if (peer_mtu < 23) peer_mtu = 23;
        if (peer_mtu > 64) peer_mtu = 64;
        bt_gatt_links[li].peer_mtu = peer_mtu;
        resp[0] = 0x03;
        resp[1] = 64; resp[2] = 0;  /* our MTU 64 */
        rlen = 3;
        break;
    }
    case 0x04: { /* Find Information Request */
        if (att_len < 5) break;
        uint16_t h1 = att[1] | (att[2] << 8), h2 = att[3] | (att[4] << 8);
        resp[0] = 0x05; resp[1] = 0x01;  /* format: 16-bit UUIDs */
        rlen = 2;
        for (int i = 0; i < bt_gatt_nattrs && rlen + 4 <= 60; i++) {
            uint16_t h = bt_gatt_attrs[i].handle;
            if (h < h1 || h > h2) continue;
            resp[rlen++] = h & 0xFF; resp[rlen++] = (h >> 8) & 0xFF;
            resp[rlen++] = bt_gatt_attrs[i].uuid & 0xFF;
            resp[rlen++] = (bt_gatt_attrs[i].uuid >> 8) & 0xFF;
        }
        if (rlen == 2) { bt_gatt_send_error(conn, op, h1, 0x0A); return; }
        break;
    }
    case 0x06: { /* Find By Type Value Request (service discovery by
                    * UUID+value): [h1, h2, uuid16, value...]. Our only
                    * group row is the GAP primary service at 0x0010. */
        if (att_len < 7) break;
        uint16_t h1 = att[1] | (att[2] << 8), h2 = att[3] | (att[4] << 8);
        uint16_t uuid = att[5] | (att[6] << 8);
        int vlen = att_len - 7;
        const uint8_t *val = att + 7;
        resp[0] = 0x07;
        rlen = 1;
        for (int i = 0; i < bt_gatt_nattrs; i++) {
            if (bt_gatt_attrs[i].uuid != uuid) continue;
            uint16_t h = bt_gatt_attrs[i].handle;
            if (h < h1 || h > h2) continue;
            if (bt_gatt_attrs[i].vlen != vlen ||
                (vlen && memcmp(bt_gatt_attrs[i].value, val, vlen))) continue;
            /* Found-group handle range: this handle..next group start-1
             * (or h2). Single service here: clamp to h2. */
            uint16_t hend = h2;
            for (int j = 0; j < bt_gatt_nattrs; j++) {
                if (bt_gatt_attrs[j].uuid == 0x2800 &&
                    bt_gatt_attrs[j].handle > h &&
                    bt_gatt_attrs[j].handle - 1 < hend)
                    hend = bt_gatt_attrs[j].handle - 1;
            }
            if (rlen + 4 > 60) break;
            resp[rlen++] = h & 0xFF; resp[rlen++] = (h >> 8) & 0xFF;
            resp[rlen++] = hend & 0xFF; resp[rlen++] = (hend >> 8) & 0xFF;
        }
        if (rlen == 1) { bt_gatt_send_error(conn, op, h1, 0x0A); return; }
        break;
    }
    case 0x08: { /* Read By Type Request */
        if (att_len < 7) break;
        uint16_t h1 = att[1] | (att[2] << 8), h2 = att[3] | (att[4] << 8);
        uint16_t uuid = att[5] | (att[6] << 8);
        resp[0] = 0x09; resp[1] = 0;  /* length filled below */
        rlen = 2;
        int pair_len = 0;
        for (int i = 0; i < bt_gatt_nattrs; i++) {
            uint16_t h = bt_gatt_attrs[i].handle;
            if (h < h1 || h > h2) continue;
            if (bt_gatt_attrs[i].uuid != uuid) continue;
            int need = 2 + bt_gatt_attrs[i].vlen;
            if (pair_len == 0) pair_len = need;
            if (need != pair_len || rlen + need > 60) break;
            resp[rlen++] = h & 0xFF; resp[rlen++] = (h >> 8) & 0xFF;
            memcpy(resp + rlen, bt_gatt_attrs[i].value, bt_gatt_attrs[i].vlen);
            rlen += bt_gatt_attrs[i].vlen;
        }
        if (rlen == 2) { bt_gatt_send_error(conn, op, h1, 0x0A); return; }
        resp[1] = (uint8_t)pair_len;
        break;
    }
    case 0x0A: { /* Read Request */
        if (att_len < 3) break;
        uint16_t h = att[1] | (att[2] << 8);
        int idx = -1;
        if (!bt_gatt_attr_by_handle(h, &idx)) {
            bt_gatt_send_error(conn, op, h, 0x0A); return;
        }
        resp[0] = 0x0B;
        int n = bt_gatt_attrs[idx].vlen;
        if (n > 60) n = 60;
        memcpy(resp + 1, bt_gatt_attrs[idx].value, (size_t)n);
        rlen = 1 + n;
        break;
    }
    case 0x0E: { /* Read Multiple Request: [h1, h2, ...] concatenated.
                    * Truncated to the MTU-60 response budget. */
        if (att_len < 3 || ((att_len - 1) & 1)) {
            bt_gatt_send_error(conn, op,
                               att_len >= 3 ? (att[1] | (att[2] << 8)) : 0,
                               0x0D); return;  /* invalid PDU length */
        }
        resp[0] = 0x0F;
        rlen = 1;
        for (int o = 1; o + 1 < att_len; o += 2) {
            uint16_t h = att[o] | (att[o + 1] << 8);
            int idx = -1;
            if (!bt_gatt_attr_by_handle(h, &idx)) {
                bt_gatt_send_error(conn, op, h, 0x0A); return;
            }
            int n = bt_gatt_attrs[idx].vlen;
            if (rlen + n > 60) break;  /* budget: truncate, still respond */
            memcpy(resp + rlen, bt_gatt_attrs[idx].value, (size_t)n);
            rlen += n;
        }
        break;
    }
    case 0x0C: { /* Read Blob Request */
        if (att_len < 5) break;
        uint16_t h = att[1] | (att[2] << 8);
        uint16_t off = att[3] | (att[4] << 8);
        int idx = -1;
        if (!bt_gatt_attr_by_handle(h, &idx)) {
            bt_gatt_send_error(conn, op, h, 0x0A); return;
        }
        if (off > bt_gatt_attrs[idx].vlen) {
            bt_gatt_send_error(conn, op, h, 0x07); return;  /* invalid offset */
        }
        resp[0] = 0x0D;
        int n = 0;
        if (off < bt_gatt_attrs[idx].vlen) {
            n = bt_gatt_attrs[idx].vlen - off;
            if (n > 60) n = 60;
            memcpy(resp + 1, bt_gatt_attrs[idx].value + off, (size_t)n);
        }
        rlen = 1 + n;
        break;
    }
    case 0x10: { /* Read By Group Type Request (service discovery) */
        if (att_len < 7) break;
        uint16_t h1 = att[1] | (att[2] << 8), h2 = att[3] | (att[4] << 8);
        uint16_t uuid = att[5] | (att[6] << 8);
        /* Only primary-service declaration 0x2800 rows are groups here. */
        if (uuid != 0x2800) {
            bt_gatt_send_error(conn, op, h1, 0x10); return;  /* unsupported */
        }
        resp[0] = 0x11; resp[1] = 0;  /* length filled below */
        rlen = 2;
        int grp_len = 0;
        for (int i = 0; i < bt_gatt_nattrs; i++) {
            if (bt_gatt_attrs[i].uuid != 0x2800) continue;
            uint16_t h = bt_gatt_attrs[i].handle;
            if (h < h1 || h > h2) continue;
            /* end-group-handle = last handle in DB (single service). */
            uint16_t hend = bt_gatt_attrs[bt_gatt_nattrs - 1].handle;
            if (hend > h2) hend = h2;
            int need = 2 + 2 + bt_gatt_attrs[i].vlen;
            if (grp_len == 0) grp_len = need;
            if (need != grp_len || rlen + need > 60) break;
            resp[rlen++] = h & 0xFF; resp[rlen++] = (h >> 8) & 0xFF;
            resp[rlen++] = hend & 0xFF; resp[rlen++] = (hend >> 8) & 0xFF;
            memcpy(resp + rlen, bt_gatt_attrs[i].value, bt_gatt_attrs[i].vlen);
            rlen += bt_gatt_attrs[i].vlen;
        }
        if (rlen == 2) { bt_gatt_send_error(conn, op, h1, 0x0A); return; }
        resp[1] = (uint8_t)grp_len;
        break;
    }
    case 0x12: { /* Write Request */
        if (att_len < 3) break;
        uint16_t h = att[1] | (att[2] << 8);
        int idx = -1;
        if (!bt_gatt_attr_by_handle(h, &idx)) {
            bt_gatt_send_error(conn, op, h, 0x0A); return;
        }
        if (!(bt_gatt_attrs[idx].props & 0x08)) {
            bt_gatt_send_error(conn, op, h, 0x03); return;  /* not writable */
        }
        int n = att_len - 3;
        if (n > 32) n = 32;
        if (n > 0) memcpy(bt_gatt_attrs[idx].value, att + 3, (size_t)n);
        bt_gatt_attrs[idx].vlen = (uint8_t)n;
        /* CCCD write of 0x0001/0x0002 arms notify/indicate on the
         * preceding value handle; mirror scratch writes (0x0012) into
         * the notify source (0x0014) so Write -> notify round-trips. */
        if (bt_gatt_attrs[idx].uuid == 0x2902 && n == 2) {
            uint16_t cfg = att[3] | ((uint16_t)att[4] << 8);
            if ((cfg & ~0x0003u) != 0) {
                bt_gatt_send_error(conn, op, h, 0x0D); return;  /* bad value */
            }
            bt_gatt_links[li].cccd_cfg = cfg;
        }
        if (h == 0x0012) {
            int j = -1;
            if (bt_gatt_attr_by_handle(0x0014, &j)) {
                if (n > 0) memcpy(bt_gatt_attrs[j].value, att + 3, (size_t)n);
                bt_gatt_attrs[j].vlen = (uint8_t)n;
            }
        }
        resp[0] = 0x13;
        rlen = 1;
        break;
    }
    case 0x1E: { /* Handle-Value Confirmation (indication ack) */
        bt_gatt_links[li].ind_pending = 0;
        bt_gatt_ind_pending = 0;
        return;
    }
    case 0x16: { /* Prepare Write Request: [h, off, value...]. Staged in
                    * the prep-write buffer; Execute Write commits. */
        if (att_len < 5) break;
        uint16_t h = att[1] | (att[2] << 8);
        uint16_t off = att[3] | (att[4] << 8);
        int idx = -1;
        if (!bt_gatt_attr_by_handle(h, &idx)) {
            bt_gatt_send_error(conn, op, h, 0x0A); return;
        }
        if (!(bt_gatt_attrs[idx].props & 0x08)) {
            bt_gatt_send_error(conn, op, h, 0x03); return;
        }
        int n = att_len - 5;
        if (off > 32 || off + n > 32) {
            bt_gatt_send_error(conn, op, h, 0x07); return;  /* bad offset */
        }
        if (bt_gatt_links[li].prep_handle != 0 && bt_gatt_links[li].prep_handle != h) {
            bt_gatt_send_error(conn, op, h, 0x0E); return;  /* unlikely err */
        }
        bt_gatt_links[li].prep_handle = h;
        if (n > 0) memcpy(bt_gatt_links[li].prep_buf + off, att + 5, (size_t)n);
        int end = off + n;
        if (end > bt_gatt_links[li].prep_len) bt_gatt_links[li].prep_len = end;
        resp[0] = 0x17;  /* Prepare Write Response echoes request */
        resp[1] = h & 0xFF; resp[2] = (h >> 8) & 0xFF;
        resp[3] = off & 0xFF; resp[4] = (off >> 8) & 0xFF;
        if (n > 0) memcpy(resp + 5, att + 5, (size_t)n);
        rlen = 5 + n;
        break;
    }
    case 0x18: { /* Execute Write Request: [flags]. 0x00 cancel, 0x01 commit
                    * the staged prepare-writes to the attribute. */
        if (att_len < 2) break;
        if (att[1] == 0x01 && bt_gatt_links[li].prep_handle != 0) {
            int idx = -1;
            if (!bt_gatt_attr_by_handle(bt_gatt_links[li].prep_handle, &idx)) {
                bt_gatt_send_error(conn, op, bt_gatt_links[li].prep_handle, 0x0A);
                bt_gatt_links[li].prep_handle = 0; bt_gatt_links[li].prep_len = 0;
                return;
            }
            int n = bt_gatt_links[li].prep_len;
            if (n > 32) n = 32;
            if (n > 0) memcpy(bt_gatt_attrs[idx].value, bt_gatt_links[li].prep_buf, (size_t)n);
            bt_gatt_attrs[idx].vlen = (uint8_t)n;
            if (bt_gatt_links[li].prep_handle == 0x0012) {
                int j = -1;
                if (bt_gatt_attr_by_handle(0x0014, &j)) {
                    if (n > 0) memcpy(bt_gatt_attrs[j].value, bt_gatt_links[li].prep_buf, (size_t)n);
                    bt_gatt_attrs[j].vlen = (uint8_t)n;
                }
            }
        }
        bt_gatt_links[li].prep_handle = 0; bt_gatt_links[li].prep_len = 0;
        resp[0] = 0x19;
        rlen = 1;
        break;
    }
    case 0x52: { /* Write Command: no response */
        if (att_len >= 3) {
            uint16_t h = att[1] | (att[2] << 8);
            int idx = -1;
            if (bt_gatt_attr_by_handle(h, &idx) &&
                (bt_gatt_attrs[idx].props & 0x08)) {
                int n = att_len - 3;
                if (n > 32) n = 32;
                if (n > 0) memcpy(bt_gatt_attrs[idx].value, att + 3, (size_t)n);
                bt_gatt_attrs[idx].vlen = (uint8_t)n;
                if (h == 0x0012) {
                    int j = -1;
                    if (bt_gatt_attr_by_handle(0x0014, &j)) {
                        if (n > 0)
                            memcpy(bt_gatt_attrs[j].value, att + 3, (size_t)n);
                        bt_gatt_attrs[j].vlen = (uint8_t)n;
                    }
                }
            }
        }
        return;
    }
    default:
        bt_gatt_send_error(conn, att_len >= 3 ? (att[1] | (att[2] << 8)) : 0,
                           op, 0x06);
        return;
    }
    if (rlen > 0) {
        bt_gatt_send_acl(conn, resp, rlen);
        /* Two-instance GATT: when the request came from a peer central
         * (src != our MAC), route the response back on the room bus so
         * the peer's guest stack completes its client op. Loopback
         * (src == our MAC) skips this: the B2H ACL above is the reply. */
        if (src_mac && memcmp(src_mac, cyw43.mac_addr, 6) != 0) {
            int rli = bt_gatt_link_by_peer(src_mac);
            if (rli < 0) rli = li;
            bt_gatt_route_room_att_on(rli, resp, rlen);
        }
    }
    (void)src_mac;
}

/* Legacy wrapper (loopback fast path): link 0. */
static void bt_gatt_handle_room_att(const uint8_t *att, int att_len,
                                    const uint8_t *src_mac) {
    bt_gatt_handle_room_att_on(0, att, att_len, src_mac);
}

/* Guest-as-server path: the guest stack emits HCI ACL (type 0x02 H2B)
 * when IT answers a peer central (its own ATT DB via btstack, e.g. MP
 * gatts_register_services). We cannot serve those from OUR static DB —
 * the ATT payload is a RESPONSE the peer central asked for — so route
 * it onto the room bus addressed to the link peer; the peer instance
 * delivers it as HCI ACL to its guest (see bt_gatt_deliver_room_resp).
 * Returns 1 if routed, 0 if not ours (no link / not ATT / not response). */
static int bt_gatt_handle_local_acl_on(int li, const uint8_t *h4payload, int h4len) {
    if (li < 0) li = 0;
    if (li >= BT_GATT_MAX_LINKS || !bt_gatt_links[li].active) return 0;
    if (h4len < 8) return 0;
    uint16_t cid = h4payload[6] | ((uint16_t)h4payload[7] << 8);
    if (cid != 4) return 0;
    int l2len = h4payload[4] | ((uint16_t)h4payload[5] << 8);
    int att_len = l2len - 2;
    if (att_len <= 0 || att_len > h4len - 8) return 0;
    if (!bt_gatt_att_is_response(h4payload[8])) return 0;
    bt_gatt_route_room_att_on(li, h4payload + 8, att_len);
    return 1;
}
static int bt_gatt_handle_local_acl(const uint8_t *h4payload, int h4len) {
    return bt_gatt_handle_local_acl_on(0, h4payload, h4len);
}

/* H2B path: the guest stack's own ATT PDUs (it is EITHER the GATT client
 * talking to OUR virtual link — e.g. MP gattc_discover/gattc_read after
 * WE initiated the link — OR the GATT server answering a peer central
 * from ITS OWN btstack DB). Layout of the H2B payload: [handle_lo,
 * handle_hi|flags, acl_len_lo, acl_len_hi, l2cap_len_lo, l2cap_len_hi,
 * cid_lo, cid_hi, ATT...]. CID != 4 is not ATT (leave for the forwarder).
 * Responses route to the peer (guest-as-server); requests serve from our
 * DB (guest-as-client loopback); confirmations (0x1E) do both (clear our
 * pending flag AND route to the peer server). Returns 1 if consumed. */
static int bt_gatt_handle_h2b_acl(const uint8_t *p, int len) {
    if (len < 8) return 0;
    if (CYW43_DBG)
        fprintf(stderr, "[CYW43] BT H2B ACL? len=%d: %02x %02x %02x %02x %02x %02x %02x %02x link=%d\n",
                len, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
                bt_gatt_link.active);
    uint16_t cid = p[6] | (p[7] << 8);
    if (cid != 4) return 0;
    uint16_t conn = p[0] | ((uint16_t)(p[1] & 0x0F) << 8);
    int li = bt_gatt_link_by_handle(conn);
    if (li < 0) li = 0;  /* legacy guests always use 0x0042 */
    if (!bt_gatt_links[li].active) return 0;
    int l2len = p[4] | (p[5] << 8);
    int att_len = l2len - 2;  /* L2CAP payload = CID(2) + ATT */
    if (att_len <= 0 || att_len > len - 8) return 0;
    uint8_t op = p[8];
    if (op == 0x1E) {
        /* Confirmation: ack our outstanding indication AND inform the
         * peer server (it may be waiting for exactly this). */
        bt_gatt_links[li].ind_pending = 0;
        bt_gatt_ind_pending = 0;
        bt_gatt_route_room_att_on(li, p + 8, att_len);
        return 1;
    }
    if (bt_gatt_att_is_response(op))
        return bt_gatt_handle_local_acl_on(li, p, len);
    /* Serve from our DB but direct the reply at the H2B loopback: reuse
     * the room handler with our own MAC as source (replies go to B2H). */
    bt_gatt_handle_room_att_on(li, p + 8, att_len, cyw43.mac_addr);
    return 1;
}

/* Central role: open a virtual LE-U link TO a peer peripheral whose ADV
 * we heard (0x88B5 room frame). Sends CONNECT_REQ on 0x88B6 and raises
 * LE Connection Complete locally so the guest stack can run its GATT
 * client (discover/read) over the link. Returns 1 if sent. Works
 * WITHOUT vnet too (loopback link to self): the room TX is best-effort
 * for peers, but the local accept always completes. */
static int bt_gatt_connect_to(const uint8_t *peer_mac) {
    if (bt_gatt_link_by_peer(peer_mac) >= 0) return 0;
    if (vnet.enabled && cyw43.vnet_port >= 0) {
        uint8_t f[14 + 6 + 1 + 8];
        memset(f, 0xFF, 6);
        memcpy(f + 6, cyw43.mac_addr, 6);
        f[12] = 0x88; f[13] = 0xB6;
        memcpy(f + 14, peer_mac, 6);
        f[20] = 8;
        f[21] = 0xF0; f[22] = 0x00;  /* CONNECT_REQ, peer_type public */
        memcpy(f + 23, cyw43.mac_addr, 6);
        vnet_tx_frame(cyw43.vnet_port, f, 14 + 6 + 1 + 8);
    }
    /* Loop the accept back locally: the peer's CONNECT_REQ echo comes
     * back through our own vnet_rx (room hub reflects to sender too),
     * which raises Connection Complete. If the hub doesn't reflect
     * (or no vnet at all), raise it directly here. */
    if (bt_gatt_link_by_peer(peer_mac) < 0) {
        int li = bt_gatt_link_alloc(peer_mac);
        if (li < 0) return 0;  /* table full */
        bt_gatt_db_reset();
        bt_gatt_sync_link0();
        bt_gatt_link_raise_complete(li);
    }
    return 1;
}

static void cyw43_bt_hci_cmd(const uint8_t *p, uint32_t len) {
    if (len < 3) return;
    uint16_t op = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    if (CYW43_DBG)
        fprintf(stderr, "[CYW43] BT HCI cmd %04x len %u\n", op,
                (unsigned)(len - 3));
    switch (op) {
    case 0x0C03: /* Reset */
    case 0x0C01: /* Set_Event_Mask */
    case 0x2001: /* LE_Set_Event_Mask */
    case 0x0C6D: /* Write_LE_Host_Support */
    case 0x0C33: /* Host_Buffer_Size */
        cyw43_bt_cmd_complete(op, NULL, 0);
        break;
    case 0x1001: { /* Read_Local_Version_Information */
        const uint8_t v[8] = {0x09, 0, 0, 0x09, 0x0F, 0, 0, 0};
        cyw43_bt_cmd_complete(op, v, 8);
        break;
    }
    case 0x1005: { /* Read_Buffer_Size */
        const uint8_t v[8] = {0xFD, 0x03, 0xFF, 4, 0, 4, 0, 0};
        cyw43_bt_cmd_complete(op, v, 8);
        break;
    }
    case 0x1009: /* Read_BD_ADDR: report the device MAC */
        cyw43_bt_cmd_complete(op, cyw43.mac_addr, 6);
        break;
    case 0x2002: { /* LE_Read_Buffer_Size */
        const uint8_t v[3] = {27, 0, 8};
        cyw43_bt_cmd_complete(op, v, 3);
        break;
    }
    case 0x2003: { /* LE_Read_Supported_States */
        const uint8_t v[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        cyw43_bt_cmd_complete(op, v, 8);
        break;
    }
    case 0x2006: /* LE_Set_Advertising_Parameters */
        cyw43_bt_cmd_complete(op, NULL, 0);
        break;
    case 0x2008: { /* LE_Set_Advertising_Data: [len, 31B data] */
        if (len >= 4) {
            int alen = p[3];
            if (alen > 31) alen = 31;
            if (4 + alen > (int)len) alen = (int)len - 4;
            if (alen < 0) alen = 0;
            cyw43.bt_adv_data_len = alen;
            for (int i = 0; i < alen; i++)
                cyw43.bt_adv_data[i] = p[4 + i];
        }
        cyw43_bt_cmd_complete(op, NULL, 0);
        break;
    }
    case 0x200A: { /* LE_Set_Advertise_Enable */
        int en = (len >= 4) ? (p[3] != 0) : 0;
        if (en && !cyw43.bt_adv_enabled)
            cyw43_bt_adv_announce();
        if (!en && cyw43.bt_adv_enabled && CYW43_DBG)
            fprintf(stderr, "[CYW43] BT ADV disabled\n");
        cyw43.bt_adv_enabled = en;
        if (en)
            bt_adv_next_ms = cyw43_now_ms() + 2000;
        cyw43_bt_cmd_complete(op, NULL, 0);
        break;
    }
    case 0x200B: /* LE_Set_Scan_Parameters */
        cyw43_bt_cmd_complete(op, NULL, 0);
        break;
    case 0x2005: { /* LE_Set_Random_Address: [addr 6]. Stored; ADV/scans
                    * report it when set (mirrors controllers that-
                    * advertise the random identity). */
        if (len >= 3 + 6) {
            memcpy(cyw43.bt_rand_addr, p + 3, 6);
            cyw43.bt_rand_addr_set = 1;
        }
        cyw43_bt_cmd_complete(op, NULL, 0);
        break;
    }
    case 0x2009: { /* LE_Set_Scan_Response_Data: [len, 31B]. Stored and
                    * announced alongside ADV (SCANNABLE legacy sets). */
        if (len >= 4) {
            int slen = p[3];
            if (slen > 31) slen = 31;
            if (4 + slen > (int)len) slen = (int)len - 4;
            if (slen < 0) slen = 0;
            cyw43.bt_scan_rsp_len = slen;
            for (int i = 0; i < slen; i++)
                cyw43.bt_scan_rsp_data[i] = p[4 + i];
        }
        cyw43_bt_cmd_complete(op, NULL, 0);
        break;
    }
    case 0x200C: { /* LE_Set_Scan_Enable */
        cyw43.bt_scan_enabled = (len >= 4) ? (p[3] != 0) : 0;
        if (CYW43_DBG)
            fprintf(stderr, "[CYW43] BT scan %s\n",
                    cyw43.bt_scan_enabled ? "enabled" : "disabled");
        cyw43_bt_cmd_complete(op, NULL, 0);
        break;
    }
    case 0x200D: { /* LE_Create_Connection: [scan_int, scan_win,
                    * init_filter, peer_type, peerMAC 6, own_type,
                    * conn_int_min/max, latency, timeout, ce_min/max].
                    * With an external controller attached (-bt-hci), the
                    * command goes to real silicon (it owns the LL
                    * handshake, Connection Complete, and the ACL path).
                    * With the internal responder, short-circuit: send
                    * CONNECT_REQ on the room bus and complete immediately. */
        uint8_t peer[6] = {0};
        if (len >= 13) memcpy(peer, p + 7, 6);
        if (bt_hci_sock_path[0] != '\0' || bt_hci_js_mode) {
            /* External controller owns the link: forward the command and
             * let ITS Connection Complete / ACL path drive the guest.
             * (Known RootCanal result: status "accepted (completion
             * follows)" but no Connection Complete ever arrives — the
             * bare-metal demo connects to its OWN mac with no peer
             * advertising, so there is no LL peer to complete against.
             * Internal loopback stays the offline default.) */
            bt_hci_forward_sync_ext(p, len, 0x01);
            return;
        }
        cyw43_bt_cmd_complete(op, NULL, 0);  /* status 0 first */
        if (!bt_gatt_link.active) {
            /* LE Connection Complete as the central role. */
            bt_gatt_connect_to(peer);
        }
        break;
    }
    case 0x0406: { /* Disconnect: [handle_lo, handle_hi, reason]. */
        cyw43_bt_cmd_complete(op, NULL, 0);
        if (bt_gatt_link.active && vnet.enabled && cyw43.vnet_port >= 0) {
            uint8_t f[14 + 6 + 1 + 1];
            memset(f, 0xFF, 6);
            memcpy(f + 6, cyw43.mac_addr, 6);
            f[12] = 0x88; f[13] = 0xB6;
            memcpy(f + 14, bt_gatt_link.peer, 6);
            f[20] = 1; f[21] = 0xF1;
            vnet_tx_frame(cyw43.vnet_port, f, 22);
        }
        if (bt_gatt_link.active) {
            bt_gatt_link.active = 0;
            uint8_t ev[4] = { 0x05, 3, 0x42, 0x00 };
            ev[3] = 0x16;  /* local host terminated */
            cyw43_bt_queue_hci(0x04, ev, 4);
        }
        break;
    }
    /* ---- Extended advertising set 0 (0x2036 params / 0x2037 data /
     * 0x2039 enable). Modeled as legacy ADV on the room bus: params
     * cached, data announced like 0x2008, enable beacons like 0x200A. */
    case 0x2036: { /* LE_Set_Extended_Advertising_Parameters */
        if (len >= 3 + 1 + 6) cyw43.bt_ext_adv_sid = p[4] & 0x0F;
        cyw43_bt_cmd_complete(op, NULL, 0);
        break;
    }
    case 0x2037: { /* LE_Set_Extended_Advertising_Data: [set, op, frag, len, data] */
        if (len >= 3 + 4) {
            int dlen = p[6];
            if (dlen > 251) dlen = 251;
            if (7 + dlen > (int)len) dlen = (int)len - 7;
            if (dlen < 0) dlen = 0;
            cyw43.bt_ext_adv_len = dlen;
            for (int i = 0; i < dlen; i++)
                cyw43.bt_ext_adv_data[i] = p[7 + i];
            /* Mirror first 31B into the legacy ADV slot so room peers
             * and scanners see the same payload. */
            int m = dlen > 31 ? 31 : dlen;
            cyw43.bt_adv_data_len = m;
            for (int i = 0; i < m; i++)
                cyw43.bt_adv_data[i] = cyw43.bt_ext_adv_data[i];
        }
        cyw43_bt_cmd_complete(op, NULL, 0);
        break;
    }
    case 0x2039: { /* LE_Set_Extended_Advertising_Enable: [en, nsets, set, dur, maxev] */
        int en = (len >= 4) ? (p[3] != 0) : 0;
        cyw43.bt_ext_adv_enabled = en;
        if (en && !cyw43.bt_adv_enabled)
            cyw43_bt_adv_announce();
        cyw43.bt_adv_enabled = en ? 1 : cyw43.bt_adv_enabled;
        if (en)
            bt_adv_next_ms = cyw43_now_ms() + 2000;
        cyw43_bt_cmd_complete(op, NULL, 0);
        break;
    }
    /* ---- Data length / PHY (0x2022 write-suggested, 0x2023 read-max,
     * 0x2032 set-default-PHY, 0x2030 read-PHY). Cached + acked; the
     * virtual link is not LL-throughput-modeled so values are advisory. */
    case 0x2022: { /* LE_Write_Suggested_Default_Data_Length */
        if (len >= 3 + 4) {
            cyw43.bt_max_tx_octets = p[3] | ((uint16_t)p[4] << 8);
            cyw43.bt_max_tx_time = p[5] | ((uint16_t)p[6] << 8);
        }
        cyw43_bt_cmd_complete(op, NULL, 0);
        break;
    }
    case 0x2023: { /* LE_Read_Maximum_Data_Length: octets+time x2 */
        const uint8_t v[8] = {0xFB, 0x00, 0x48, 0x08, 0xFB, 0x00, 0x48, 0x08};
        cyw43_bt_cmd_complete(op, v, 8);
        break;
    }
    case 0x2030: { /* LE_Read_PHY: [handle] -> CC(status, handle, tx, rx) */
        uint8_t v[4] = { (len >= 4) ? p[3] : 0x42,
                         cyw43.bt_tx_phy ? cyw43.bt_tx_phy : 1,
                         cyw43.bt_rx_phy ? cyw43.bt_rx_phy : 1 };
        v[3] = 0;
        cyw43_bt_cmd_complete(op, v, 3);
        break;
    }
    case 0x2032: { /* LE_Set_Default_PHY: [all, tx, rx] */
        if (len >= 3 + 3) {
            if (!(p[3] & 0x01)) cyw43.bt_tx_phy = p[4];
            if (!(p[3] & 0x02)) cyw43.bt_rx_phy = p[5];
        }
        cyw43_bt_cmd_complete(op, NULL, 0);
        break;
    }
    /* ---- SMP stub (0x0C56 Pin_Request reply-class + 0x0430 IO-cap
     * passkey class). Real pairing crypto needs a controller; we ack so
     * stacks proceed, and record pairing-start for the trace log. */
    case 0x0C56: /* Pin_Code_Request_Reply (SMP-adjacent legacy pairing) */
        cyw43.bt_smp_pairing = 1;
        cyw43_bt_cmd_complete(op, cyw43.mac_addr, 6);
        break;
    case 0x0C57: /* Pin_Code_Request_Negative_Reply */
        cyw43.bt_smp_pairing = 0;
        cyw43_bt_cmd_complete(op, cyw43.mac_addr, 6);
        break;
    default:
        /* Broadcom vendor + anything else: ack with status 0 so init
         * proceeds; the opcode is logged for follow-up work. */
        cyw43_bt_cmd_complete(op, NULL, 0);
        break;
    }
}

/* Consume newly arrived host->BT packets (called on H2B_IN write).
 * ALSO polled from the WFE fast-forward path (see cpu.c): the guest's
 * BT bring-up sits in cyw43_delay_ms -> best_effort_wfe_or_timeout ->
 * WFE loops whose timer IRQs we never model, so without host-side
 * pumping the H2B ring would never drain during those spins. Polling
 * here is side-effect-free when no new bytes arrived (avail==0). */
void cyw43_bt_hci_poll(void) {
    uint32_t in = bt_ram_rd32(0x2000) & 0xFFF;
    uint32_t out = cyw43.bt_h2b_out & 0xFFF;
    uint32_t avail = (in - out) & 0xFFF;
    if (avail > 0x800) {  /* stale shadow (re-init?): resync, drop */
        cyw43.bt_h2b_out = in;
        bt_ram_wr32(0x2004, in);
        return;
    }
    /* Server-side notify/indicate outbox drains first so request/response
     * traffic behind it never starves a queued Handle-Value PDU. */
    if (bt_gatt_ntf_queued && bt_gatt_link.active) bt_gatt_poll_notify();
    while (avail >= 4) {
        uint8_t hdr[4];
        for (int i = 0; i < 4; i++)
            hdr[i] = cyw43.bt_ram[(out + i) & 0xFFF];
        uint32_t hlen = hdr[0] | ((uint32_t)hdr[1] << 8);
        uint8_t type = hdr[3];
        uint32_t pktlen = 4 + ((hlen + 3) & ~3u);
        if (pktlen > avail || pktlen > 264) break;
        uint8_t payload[264];
        for (uint32_t i = 0; i < hlen && i < sizeof(payload); i++)
            payload[i] = cyw43.bt_ram[(out + 4 + i) & 0xFFF];
        out = (out + pktlen) & 0xFFF;
        avail -= pktlen;
        cyw43.bt_h2b_out = out;
        bt_ram_wr32(0x2004, out);
        /* Guest->controller HCI ACL (L2CAP/ATT, ring type 0x02): with the
         * internal responder, serve ATT for OUR virtual link directly
         * (loopback GATT); anything else still forwards below. */
        if (type == 0x02) {
            extern int bt_gatt_handle_h2b_acl(const uint8_t *p, int len);
            if (bt_gatt_handle_h2b_acl(payload, (int)hlen))
                continue;  /* consumed: response already queued */
            /* fall through to forward below (external controller/room) */
        }
        /* H4 uplink for every unhandled packet (commands AND unmatched
         * ACL): JS ring, unix socket, else internal command responder.
         * NOTE: with no -bt-hci path and no JS mode, only type 0x01
         * (commands) reach the internal responder; type 0x02 without a
         * virtual link has nowhere to go and is dropped. */
        {
            uint32_t cplen = hlen > 264 ? 264 : hlen;
            if (bt_hci_js_mode) {
                /* Browser/WASM uplink: ring for the embedder to drain. */
                uint32_t total = 1 + cplen;
                int n = (bt_hci_js_tx_head + 1) % 8;
                if (n != bt_hci_js_tx_tail && total <= 1088) {
                    bt_hci_js_tx[bt_hci_js_tx_head][0] = type;
                    for (uint32_t i = 0; i < cplen; i++)
                        bt_hci_js_tx[bt_hci_js_tx_head][1 + i] = payload[i];
                    bt_hci_js_tx_len[bt_hci_js_tx_head] = (uint16_t)total;
                    bt_hci_js_tx_head = n;
                }
            } else if (bt_hci_sock_path[0] != '\0') {
                uint8_t h4[264 + 1];
                h4[0] = type;
                for (uint32_t i = 0; i < cplen; i++)
                    h4[1 + i] = payload[i];
                bt_hci_forward_sync(h4, 1 + (int)cplen);
            } else if (type == 0x01) {
                cyw43_bt_hci_cmd(payload, hlen);
            }
        }
    }
}

/* ========================================================================
 * Backplane Access (Function 1)
 * ======================================================================== */

static uint32_t cyw43_backplane_read(uint32_t addr) {
    /* SPI backplane addresses carry SBSDIO_SB_ACCESS_2_4B_FLAG (0x8000);
     * strip it before every compare below (window regs, direct regs,
     * SDIO alias, and the windowed full_addr path all use raw offsets). */
    addr &= ~0x8000u;
    /* CHIPCLKCSR - ALP/HT clock status (SDIO func1 direct register) */
    if (addr == CYW43_BP_CHIPCLKCSR) {
        return cyw43.chipclkcsr;
    }

    /* SDIO_INT_STATUS (WLAN 0x18002000 window + 0x20, masked to 15 bits =
     * low alias 0x0020): the SAME word as BT INT_STATUS. The BT driver
     * (cyw43_ll_bt_has_work) and the WLAN driver (CLEAR_SDIO_INT paths)
     * both poll/clear it, so serve it before the generic windowed path
     * (whose 0x1800xxxx full_addr compare never matches this alias).
     * NOTE: only when the window is actually the SDIO/WLAN window —
     * BT-RAM-windowed reads of BT offsets must fall through to the
     * bt_ram window below. */
    if ((addr & 0x7FFF) == 0x0020 && cyw43.bp_window == 0x18002000u) {
        if (cyw43_bt_pending())
            cyw43.bt_int_status |= CYW43_BT_FC_CHANGE;
        return cyw43.bt_int_status;
    }

    /* Window address register reads */
    if (addr == 0x1000A) return (cyw43.bp_window >> 8) & 0xFF;
    if (addr == 0x1000B) return (cyw43.bp_window >> 16) & 0xFF;
    if (addr == 0x1000C) return (cyw43.bp_window >> 24) & 0xFF;

    /* SDIO func1 direct registers (full 17-bit address, no windowing) */
    if (addr == CYW43_BP_F2_WATERMARK) {
        /* F2 watermark scratch: BT-enabled guests write 0x10 then read
         * it back; a mismatch aborts bus_init with -EIO. */
        return cyw43.f2_watermark;
    }
    if (addr == 0x1001F) {
        /* SBSDIO_FUNC1_SLEEPCSR (KSO): KSO bit tracks guest writes,
         * DEVICE_ON always set. A stuck-1 KSO makes the driver's
         * 64-round kso_set(0) sleep-confirm spin on every idle poll. */
        return cyw43.sleepcsr;
    }

    /* Windowed backplane access: reconstruct full address from window + offset */
    uint32_t full_addr = cyw43.bp_window | (addr & 0x7FFF);

    /* Chip ID at CHIPCOMMON offset 0 */
    if (full_addr == 0x18000000u)
        return CYW43439_CHIP_ID;

    /* Core wrapper registers (use full address comparison) */
    if (full_addr == CYW43_WLAN_RESETCTRL)  return cyw43_wlan_resetctrl;
    if (full_addr == CYW43_WLAN_IOCTRL)     return cyw43_wlan_ioctrl;
    if (full_addr == CYW43_SOCRAM_RESETCTRL) return cyw43_socram_resetctrl;
    if (full_addr == CYW43_SOCRAM_IOCTRL)    return cyw43_socram_ioctrl;

    /* BT core: firmware is "already loaded" so FW_RDY/AWAKE always set;
     * HOST_CTRL returns stored bits; RAM base is discovered by driver. */
    if (full_addr == CYW43_BT_CTRL_REG)
        return CYW43_BT_FW_RDY | CYW43_BT_AWAKE;
    if (full_addr == CYW43_BT_HOST_CTRL_REG) return cyw43.bt_host_ctrl;
    if (full_addr == CYW43_BT_RAM_BASE_REG) return CYW43_BT_RAM_BASE;
    if (full_addr == CYW43_BT_INT_STATUS_REG) {
        /* Level semantics: FC_CHANGE reasserts while BT bytes wait, so a
         * WiFi-side write-1-to-clear (which masks 0xF0, covering bit 5)
         * cannot lose a pending BT wakeup. This is the SAME word as the
         * WLAN SDIO_INT_STATUS (0x18002020) the driver's bt_has_work
         * polls, so BT event delivery depends on it. */
        if (cyw43_bt_pending())
            cyw43.bt_int_status |= CYW43_BT_FC_CHANGE;
        return cyw43.bt_int_status;
    }
    if (full_addr >= CYW43_BT_RAM_BASE &&
        full_addr + 4 <= CYW43_BT_RAM_BASE + CYW43_BT_RAM_SIZE) {
        uint32_t o = full_addr - CYW43_BT_RAM_BASE;
        uint32_t v = (uint32_t)cyw43.bt_ram[o] |
               ((uint32_t)cyw43.bt_ram[o + 1] << 8) |
               ((uint32_t)cyw43.bt_ram[o + 2] << 16) |
               ((uint32_t)cyw43.bt_ram[o + 3] << 24);
        return v;
    }

    if (cpu.debug_enabled)
        fprintf(stderr, "[CYW43] Backplane read addr=0x%05X (full=0x%08X) -> 0\n",
                addr, full_addr);
    return 0;
}

static void cyw43_backplane_write(uint32_t addr, uint32_t val) {
    /* Strip SBSDIO_SB_ACCESS_2_4B_FLAG like the read path. */
    addr &= ~0x8000u;
    /* Window address registers: 3 byte writes build the window */
    if (addr == 0x1000A) { /* SDIO_BACKPLANE_ADDRESS_LOW: bits [15:8] */
        cyw43.bp_window = (cyw43.bp_window & 0xFFFF00FFu) | ((val & 0xFF) << 8);
        return;
    }
    if (addr == 0x1000B) { /* SDIO_BACKPLANE_ADDRESS_MID: bits [23:16] */
        cyw43.bp_window = (cyw43.bp_window & 0xFF00FFFFu) | ((val & 0xFF) << 16);
        return;
    }
    if (addr == 0x1000C) { /* SDIO_BACKPLANE_ADDRESS_HIGH: bits [31:24] */
        cyw43.bp_window = (cyw43.bp_window & 0x00FFFFFFu) | ((val & 0xFF) << 24);
        return;
    }

    /* CHIPCLKCSR write (SDIO func1 direct register) */
    if (addr == CYW43_BP_CHIPCLKCSR) {
        cyw43.chipclkcsr = val | CYW43_HT_AVAIL | CYW43_ALP_AVAIL;
        return;
    }

    /* SLEEPCSR write: KSO bit sticks (DEVICE_ON always reads set). */
    if (addr == 0x1001F) {
        cyw43.sleepcsr = (uint8_t)((val & 0x01) | 0x02);
        return;
    }

    /* F2 watermark scratch write (see read path). */
    if (addr == CYW43_BP_F2_WATERMARK) {
        cyw43.f2_watermark = (uint8_t)(val & 0xFF);
        return;
    }

    /* Windowed backplane access: reconstruct full address */
    uint32_t full_addr = cyw43.bp_window | (addr & 0x7FFF);

    /* SDIO_INT_STATUS write alias (same word as BT INT_STATUS above,
     * same window guard): write-1-to-clear so both drivers' clear
     * paths work. */
    if ((addr & 0x7FFF) == 0x0020 && cyw43.bp_window == 0x18002000u) {
        cyw43.bt_int_status &= ~val;
        return;
    }

    /* Core wrapper registers (use full address comparison) */
    if (full_addr == CYW43_WLAN_RESETCTRL)  { cyw43_wlan_resetctrl = val & 0xFF; return; }
    if (full_addr == CYW43_WLAN_IOCTRL)     { cyw43_wlan_ioctrl = val & 0xFF; return; }
    if (full_addr == CYW43_SOCRAM_RESETCTRL) { cyw43_socram_resetctrl = val & 0xFF; return; }
    if (full_addr == CYW43_SOCRAM_IOCTRL)    { cyw43_socram_ioctrl = val & 0xFF; return; }

    /* BT core: HOST_CTRL stored RMW; BT RAM window byte-stored;
     * INT_STATUS is write-1-to-clear (BT FC_CHANGE for HCI events). */
    if (full_addr == CYW43_BT_HOST_CTRL_REG) { cyw43.bt_host_ctrl = val; return; }
    if (full_addr == CYW43_BT_INT_STATUS_REG) {
        /* Write-1-to-clear. Stale clears are harmless: the read side
         * reasserts FC_CHANGE while BT bytes are still pending. */
        cyw43.bt_int_status &= ~val;
        return;
    }
    if (full_addr >= CYW43_BT_RAM_BASE &&
        full_addr + 4 <= CYW43_BT_RAM_BASE + CYW43_BT_RAM_SIZE) {
        uint32_t o = full_addr - CYW43_BT_RAM_BASE;
        cyw43.bt_ram[o] = val & 0xFF;
        cyw43.bt_ram[o + 1] = (val >> 8) & 0xFF;
        cyw43.bt_ram[o + 2] = (val >> 16) & 0xFF;
        cyw43.bt_ram[o + 3] = (val >> 24) & 0xFF;
        /* Host appended HCI bytes: parse + answer synchronously.
         * Bulk writes land consecutive words, so any word covering
         * H2B_IN (offset 0x2000) means new bytes arrived. */
        if (o <= 0x2000 && 0x2000 < o + 4)
            cyw43_bt_hci_poll();
        /* Host consumed BT bytes: re-evaluate the shared wake line. */
        if (o <= 0x200C && 0x200C < o + 4)
            cyw43_update_irq();
        return;
    }

    if (cpu.debug_enabled)
        fprintf(stderr, "[CYW43] Backplane write addr=0x%05X (full=0x%08X) = 0x%08X\n",
                addr, full_addr, val);
}

/* ========================================================================
 * GPIO-Level gSPI (legacy, unused by Pico SDK PIO driver)
 * ======================================================================== */

static uint8_t dio_out_value = 0;
static int dio_out_bit = 0;

static void cyw43_clock_bit(int dio_in) {
    cyw43_spi_state_t *s = &cyw43.spi;

    if (!s->in_data_phase) {
        s->cmd_word = (s->cmd_word << 1) | (dio_in & 1);
        s->cmd_bits++;
        if (s->cmd_bits >= 32) {
            /* Would need to process command here */
            s->in_data_phase = 1;
        }
    } else if (s->is_write) {
        /* Write data phase */
    } else {
        /* Read data phase */
        if (dio_out_bit == 0) {
            if (s->resp_offset < s->resp_len)
                dio_out_value = s->resp_buf[s->resp_offset++];
            else
                dio_out_value = 0;
        }
        dio_out_bit++;
        if (dio_out_bit >= 8) dio_out_bit = 0;
    }
}

int cyw43_gpio_intercept(uint32_t gpio, uint32_t value) {
    if (!cyw43.enabled) return 0;

    switch (gpio) {
    case WL_CS:
        if (value == 0) {
            cyw43.spi.cs_active = 1;
            cyw43.spi.cmd_word = 0;
            cyw43.spi.cmd_bits = 0;
            cyw43.spi.in_data_phase = 0;
            cyw43.spi.data_offset = 0;
            dio_out_bit = 0;
        } else {
            cyw43.spi.cs_active = 0;
        }
        return 1;
    case WL_CLK:
        if (cyw43.spi.cs_active && value == 1) {
            int dio_in = 0;
            cyw43_clock_bit(dio_in);
        }
        return 1;
    case WL_DIO:
        return 1;
    default:
        return 0;
    }
}

uint32_t cyw43_gpio_read_dio(void) {
    if (!cyw43.enabled) return 0;
    cyw43_spi_state_t *s = &cyw43.spi;
    if (!s->cs_active || s->is_write || !s->in_data_phase) return 0;
    return (dio_out_value >> (7 - dio_out_bit)) & 1;
}

void cyw43_add_scan_result(const char *ssid, int rssi, int channel, int auth) {
    if (cyw43.scan_count >= CYW43_MAX_SCAN_RESULTS) return;

    cyw43_scan_result_t *r = &cyw43.scan_results[cyw43.scan_count];
    snprintf(r->ssid, CYW43_MAX_SSID_LEN + 1, "%s", ssid);
    r->ssid[CYW43_MAX_SSID_LEN] = '\0';
    r->rssi = (int16_t)rssi;
    r->channel = (uint8_t)channel;
    r->auth_mode = (uint8_t)auth;

    uint32_t hash = 0x12345678;
    for (const char *p = ssid; *p; p++)
        hash = hash * 31 + *p;
    r->bssid[0] = 0x02;
    r->bssid[1] = (hash >> 24) & 0xFF;
    r->bssid[2] = (hash >> 16) & 0xFF;
    r->bssid[3] = (hash >>  8) & 0xFF;
    r->bssid[4] = (hash >>  0) & 0xFF;
    r->bssid[5] = cyw43.scan_count;

    cyw43.scan_count++;
}

/* Remove a scan result by SSID (e.g. soft-AP going down). */
static void cyw43_del_scan_result(const char *ssid) {
    for (int i = 0; i < cyw43.scan_count; i++) {
        if (strncmp(cyw43.scan_results[i].ssid, ssid, CYW43_MAX_SSID_LEN + 1) == 0) {
            memmove(&cyw43.scan_results[i], &cyw43.scan_results[i + 1],
                    (size_t)(cyw43.scan_count - i - 1) * sizeof(cyw43_scan_result_t));
            cyw43.scan_count--;
            return;
        }
    }
}

/* Bring the soft-AP up/down: track state, publish/withdraw the beacon
 * (scan list), and queue AP-interface events for the driver. */
static void cyw43_ap_set_up(int up) {
    if (up && !cyw43.ap_up) {
        cyw43.ap_up = 1;
        if (cyw43.ap_ssid[0])
            cyw43_add_scan_result(cyw43.ap_ssid, -50,
                                  cyw43.ap_channel ? cyw43.ap_channel : 6, 0);
        cyw43_queue_event_if(CYW43_EV_SET_SSID, CYW43_STATUS_SUCCESS, 0, 0, 1);
        cyw43_queue_event_if(CYW43_EV_LINK, CYW43_STATUS_SUCCESS, 0, 1, 1);
        if (CYW43_DBG)
            fprintf(stderr, "[CYW43] AP up: '%s' ch=%d\n",
                    cyw43.ap_ssid, cyw43.ap_channel);
    } else if (!up && cyw43.ap_up) {
        cyw43.ap_up = 0;
        if (cyw43.ap_ssid[0])
            cyw43_del_scan_result(cyw43.ap_ssid);
        cyw43_queue_event_if(CYW43_EV_DISASSOC, CYW43_STATUS_SUCCESS, 0, 0, 1);
    }
}

/* ========================================================================
 * PIO-Level gSPI Protocol Handling
 *
 * The Pico SDK CYW43 driver uses PIO0 SM0 for gSPI communication.
 * TX FIFO writes = gSPI command/data words
 * RX FIFO reads  = gSPI response words
 * ======================================================================== */

static enum {
    PIO_CYW43_IDLE,
    PIO_CYW43_WRITE,
    PIO_CYW43_READ
} pio_cyw43_phase = PIO_CYW43_IDLE;

static uint32_t pio_cmd_function;
static uint32_t pio_cmd_address;
static uint32_t pio_cmd_size;
static uint32_t pio_words_remaining;

static uint32_t pio_resp_buf[512];
static int pio_resp_count;
static int pio_resp_idx;

/* Count of pre-DMA pio_sm_put writes to skip (bit count words, not gSPI data) */
static int pio_pre_dma_skip = 0;

/* Number of remaining commands that use SWAP32 encoding (first 2 at boot) */
static int pio_init_swap_remaining = 0;

/* Whether the current command uses SWAP32 encoding */
static int pio_cmd_is_swap = 0;

/* Reverse bytes within each 16-bit halfword (ARM REV16 / SWAP32) */
static inline uint32_t cyw43_rev16(uint32_t x) {
    return ((x & 0xFF00FF00u) >> 8) | ((x & 0x00FF00FFu) << 8);
}

/* Reverse all 4 bytes of a 32-bit word */
static inline uint32_t cyw43_bswap32(uint32_t x) {
    return ((x >> 24) & 0xFF) | ((x >> 8) & 0xFF00) |
           ((x << 8) & 0xFF0000) | ((x << 24) & 0xFF000000);
}

/* Decode a TXF word to the original make_cmd() value */
static inline uint32_t cyw43_decode_txf(uint32_t val, int is_swap) {
    /* Regular: TXF = bswap32(cmd)  → cmd = bswap32(TXF)
     * Swap:    TXF = bswap32(rev16(cmd)) → cmd = rev16(bswap32(TXF)) */
    uint32_t b = cyw43_bswap32(val);
    return is_swap ? cyw43_rev16(b) : b;
}

/* Encode a response value for pio_resp_buf so firmware sees 'expected' after DMA bswap */
static inline uint32_t cyw43_encode_resp(uint32_t expected, int is_swap) {
    /* Regular: firmware reads bswap32(X) as uint32_t → X = bswap32(expected)
     * Swap:    firmware reads SWAP32(bswap32(X))    → X = bswap32(rev16(expected)) */
    return is_swap ? cyw43_bswap32(cyw43_rev16(expected)) : cyw43_bswap32(expected);
}

/* Called when CYW43 PIO SM is restarted (before each transfer setup) */
void cyw43_pio_sm_restart(void) {
    pio_pre_dma_skip = 2;  /* SDK always does 2 pio_sm_put (X, Y) before DMA */
    pio_cyw43_phase = PIO_CYW43_IDLE;
}

void cyw43_pio_tx_write(uint32_t val) {
    if (!cyw43.enabled) return;

    /* Skip the pre-DMA bit-count words (from pio_sm_put for X/Y registers) */
    if (pio_pre_dma_skip > 0) {
        pio_pre_dma_skip--;
        return;
    }

    switch (pio_cyw43_phase) {
    case PIO_CYW43_IDLE: {
        /* Determine encoding: first 2 commands at boot use SWAP32 (rev16+bswap),
         * all subsequent commands use plain bswap32. */
        pio_cmd_is_swap = (pio_init_swap_remaining > 0);
        if (pio_cmd_is_swap)
            pio_init_swap_remaining--;

        uint32_t cmd = cyw43_decode_txf(val, pio_cmd_is_swap);

        int is_write     = (cmd >> 31) & 1;
        pio_cmd_function = (cmd >> 28) & 0x3;   /* 2-bit function field */
        pio_cmd_address  = (cmd >> 11) & 0x1FFFF;
        pio_cmd_size     = cmd & 0x7FF;          /* 11-bit size field */

        if (CYW43_DBG)
            fprintf(stderr, "[CYW43] PIO gSPI: %s func=%d addr=0x%05X size=%d%s (raw=0x%08X)\n",
                    is_write ? "WR" : "RD", pio_cmd_function,
                    pio_cmd_address, pio_cmd_size,
                    pio_cmd_is_swap ? " (swap)" : "", val);

        if (is_write) {
            pio_cyw43_phase = PIO_CYW43_WRITE;
            pio_words_remaining = (pio_cmd_size + 3) / 4;
            if (pio_words_remaining == 0) pio_words_remaining = 1;

            /* Reset WLAN TX buffer for accumulation */
            if (pio_cmd_function == CYW43_FUNC_WLAN)
                cyw43.wlan_tx_offset = 0;
        } else {
            /* Prepare read response */
            pio_resp_count = 0;
            pio_resp_idx = 0;

            int total_words = (pio_cmd_size + 3) / 4;
            if (total_words <= 0) total_words = 1;
            if (total_words > 512) total_words = 512;

            memset(pio_resp_buf, 0, total_words * 4);

            switch (pio_cmd_function) {
            case CYW43_FUNC_BUS: {
                uint32_t resp = cyw43_bus_read(pio_cmd_address);
                pio_resp_buf[0] = cyw43_encode_resp(resp, pio_cmd_is_swap);
                pio_resp_count = total_words;
                break;
            }
            case CYW43_FUNC_BACKPLANE: {
                /* Backplane reads have CYW43_BACKPLANE_READ_PAD_LEN_BYTES=16 of padding
                 * (for CYW43_USE_SPI). The firmware DMA reads (4 + pad + data) bytes,
                 * returning (rx_length/4 - tx_length/4) = (24/4 - 1) = 5 words from RXF.
                 * The actual response goes at index pad_words = 4. */
                int pad_words = 4; /* CYW43_BACKPLANE_READ_PAD_LEN_BYTES / 4 = 16/4 = 4 */
                int total_words_bp = total_words + pad_words;
                if (total_words_bp > 512) total_words_bp = 512;
                memset(pio_resp_buf, 0, total_words_bp * 4);
                /* Serve consecutive words for bulk reads (BT HCI buffers,
                 * mem_read padding); single-word reads behave as before. */
                for (int j = 0; j < total_words && pad_words + j < 512; j++) {
                    uint32_t resp = cyw43_backplane_read(pio_cmd_address + (uint32_t)(j * 4));
                    pio_resp_buf[pad_words + j] = cyw43_encode_resp(resp, pio_cmd_is_swap);
                }
                pio_resp_count = total_words_bp;
                break;
            }
            case CYW43_FUNC_WLAN: {
                /* Return queued RX frame.
                 * Each word in pio_resp_buf must be bswap32 of the LE frame word,
                 * because DMA bswap will reverse it back for the firmware. */
                cyw43_rx_frame_t *f = rx_queue_peek();
                if (f && f->len > 0) {
                    int copy_len = f->len;
                    if (copy_len > (int)pio_cmd_size) copy_len = (int)pio_cmd_size;
                    if (copy_len > (int)sizeof(pio_resp_buf)) copy_len = (int)sizeof(pio_resp_buf);
                    int words = (copy_len + 3) / 4;
                    memset(pio_resp_buf, 0, words * 4);
                    /* Copy bytes then bswap32 each word so firmware gets correct byte order */
                    memcpy(pio_resp_buf, f->data, copy_len);
                    for (int j = 0; j < words; j++)
                        pio_resp_buf[j] = cyw43_bswap32(pio_resp_buf[j]);
                    pio_resp_count = total_words;
                    /* Popping the SET_SSID response: ll_wifi_join assigns
                     * ACTIVE right after this, so queue join events now —
                     * any later poll consumes them strictly after ACTIVE
                     * (see connect_pending_id). */
                    if (cyw43.connect_pending_id >= 0 &&
                        (f->data[5] & 0x0F) == SDPCM_CONTROL_CHANNEL &&
                        f->len >= 24 &&
                        (int)(f->data[22] | ((uint16_t)f->data[23] << 8)) ==
                            cyw43.connect_pending_id) {
                        cyw43.connect_pending_id = -1;
                        cyw43_queue_connect_events();
                    }
                    rx_queue_pop();

                    if (CYW43_DBG)
                        fprintf(stderr, "[CYW43] WLAN RX: delivering %d byte frame (ch=%d seq=%d)\n",
                                copy_len, f->data[4] & 0x0F, f->data[3]);
                } else {
                    /* No data - return empty SDPCM (size=0) */
                    pio_resp_count = total_words;
                }
                break;
            }
            }

            pio_cyw43_phase = PIO_CYW43_READ;
        }
        break;
    }

    case PIO_CYW43_WRITE: {
        /* Decode data word: same transform as the command word */
        uint32_t decoded = cyw43_decode_txf(val, pio_cmd_is_swap);
        switch (pio_cmd_function) {
        case CYW43_FUNC_BUS:
            cyw43_bus_write(pio_cmd_address, decoded);
            break;
        case CYW43_FUNC_BACKPLANE:
            cyw43_backplane_write(pio_cmd_address, decoded);
            break;
        case CYW43_FUNC_WLAN:
            /* decoded = bswap32(TXF) = LE word with correct frame bytes */
            cyw43_wlan_tx_word(decoded);
            break;
        }
        pio_cmd_address += 4;

        if (--pio_words_remaining <= 0) {
            if (pio_cmd_function == CYW43_FUNC_WLAN)
                cyw43_wlan_tx_complete();
            pio_cyw43_phase = PIO_CYW43_IDLE;
        }
        break;
    }

    case PIO_CYW43_READ:
        pio_cyw43_phase = PIO_CYW43_IDLE;
        cyw43_pio_tx_write(val);
        break;
    }
}

uint32_t cyw43_pio_rx_read(void) {
    if (!cyw43.enabled) return 0;

    if (pio_cyw43_phase == PIO_CYW43_READ && pio_resp_idx < pio_resp_count) {
        uint32_t val = pio_resp_buf[pio_resp_idx++];
        if (pio_resp_idx >= pio_resp_count) {
            pio_cyw43_phase = PIO_CYW43_IDLE;
        }
        return val;
    }

    return 0;
}

int cyw43_pio_rx_ready(void) {
    return (pio_cyw43_phase == PIO_CYW43_READ && pio_resp_idx < pio_resp_count);
}

int cyw43_pio_phase_is_idle(void) {
    return (pio_cyw43_phase == PIO_CYW43_IDLE);
}
