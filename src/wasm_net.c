#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <emscripten.h>

/* Expose putchar capture from bramble_wasm.c for mirroring */
extern int putchar(int c);

/* ============ Cooperative corepool (no pthreads in browser) ============ */
#include <pthread.h>
static int wasm_num_cores = 2;
static int wasm_quantum = 64;

void corepool_init(void) {
    EM_ASM({ console.log("[WASM-CORE] cooperative init (no pthreads)"); });
    /* Detect host CPUs via JS when available */
    int hc = EM_ASM_INT({
        if (typeof navigator !== 'undefined' && navigator.hardwareConcurrency)
            return navigator.hardwareConcurrency;
        return 4;
    });
    (void)hc;
}
void corepool_set_step_quantum(int q) {
    if (q < 1) q = 1;
    if (q > 4096) q = 4096;
    wasm_quantum = q;
}
int corepool_get_step_quantum(void) { return wasm_quantum; }
int corepool_query_cores(void) {
    int hc = EM_ASM_INT({
        if (typeof navigator !== 'undefined' && navigator.hardwareConcurrency)
            return navigator.hardwareConcurrency;
        return 4;
    });
    if (hc < 1) hc = 1;
    /* Leave one CPU for browser UI, clamp to 1..2 (RP2040 max) */
    int want = hc > 1 ? 2 : 1;
    if (wasm_num_cores == 1 || wasm_num_cores == 2) want = wasm_num_cores;
    return want;
}
void corepool_register(int cores) {
    if (cores == 1 || cores == 2) wasm_num_cores = cores;
}
void corepool_unregister(void) {}
void corepool_start_threads(void) {}
void corepool_stop_threads(void) {}
void corepool_start_core_thread(int id) { (void)id; }
void corepool_wake_cores(void) {}
void corepool_lock(void) {}
void corepool_unlock(void) {}
void corepool_cleanup(void) {}
int corepool_detect_host_cpus(void) {
    int hc = EM_ASM_INT({
        if (typeof navigator !== 'undefined' && navigator.hardwareConcurrency)
            return navigator.hardwareConcurrency;
        return 4;
    });
    return hc > 0 ? hc : 4;
}
/* WASM helper for bramble_step to query active cores */
int wasm_corepool_num_cores(void) { return wasm_num_cores; }
void wasm_corepool_set_num_cores(int n) {
    if (n == 1 || n == 2) wasm_num_cores = n;
}

/* ============ Net bridge via WebSocket + serial mirror ============ */
static int net_connected = 0;
static int net_mirror = 1; /* mirror TX to serial console too */
#define NET_RX_SIZE 4096
static uint8_t net_rx_buf[NET_RX_SIZE];
static int net_rx_head = 0, net_rx_tail = 0;

void bramble_net_set_connected(int v) { net_connected = v ? 1 : 0; }
void bramble_net_push_rx(const uint8_t *data, int len) {
    for (int i = 0; i < len; i++) {
        int nxt = (net_rx_head + 1) % NET_RX_SIZE;
        if (nxt == net_rx_tail) break;
        net_rx_buf[net_rx_head] = data[i];
        net_rx_head = nxt;
    }
}
int bramble_net_pop_rx(uint8_t *out, int maxlen) {
    int n = 0;
    while (n < maxlen && net_rx_tail != net_rx_head) {
        out[n++] = net_rx_buf[net_rx_tail];
        net_rx_tail = (net_rx_tail + 1) % NET_RX_SIZE;
    }
    return n;
}

int net_bridge_init(void) {
    EM_ASM({ console.log("[WASM-NET] net_bridge via WebSocket (+serial mirror)"); });
    return 0;
}
void net_bridge_cleanup(void) {}
void net_bridge_poll(void) {
    /* Drain WebSocket-queued bytes (pushed by JS via bramble_net_push_rx)
     * into UART0. The actual uart_rx_push is done by bramble_wasm feed path
     * which calls this helper; we expose pop for bramble_wasm.c. */
}
int net_bridge_uart_active(int uart_num) { (void)uart_num; return net_connected; }
void net_bridge_uart_tx(int uart_num, uint8_t byte) {
    (void)uart_num;
    EM_ASM({
        const ch = $0;
        if (typeof window !== 'undefined' && window.brambleNetSocket &&
            window.brambleNetSocket.readyState === 1) {
            try { window.brambleNetSocket.send(new Uint8Array([ch])); } catch(e) {}
        }
    }, byte);
    /* Mirror to serial console so local monitor still shows data */
    if (net_mirror) putchar((int)byte);
}

/* ============ Wire via BroadcastChannel + serial mirror ============ */
static int wire_connected = 0;
#define WIRE_RX_SIZE 4096
static uint8_t wire_rx_buf[WIRE_RX_SIZE];
static int wire_rx_head = 0, wire_rx_tail = 0;

void bramble_wire_set_connected(int v) { wire_connected = v ? 1 : 0; }
void bramble_wire_push_rx(const uint8_t *data, int len) {
    for (int i = 0; i < len; i++) {
        int nxt = (wire_rx_head + 1) % WIRE_RX_SIZE;
        if (nxt == wire_rx_tail) break;
        wire_rx_buf[wire_rx_head] = data[i];
        wire_rx_head = nxt;
    }
}

int wire_init(void) {
    EM_ASM({
        if (typeof BroadcastChannel !== 'undefined') {
            if (!window.brambleWire) {
                window.brambleWire = new BroadcastChannel('bramble-wire');
                console.log("[WASM-NET] wire BroadcastChannel ready");
                window.brambleWire.onmessage = function(ev) {
                    const m = ev.data;
                    if (!m) return;
                    if (m.type === 'uart' && typeof m.ch === 'number') {
                        if (typeof window.brambleWireRx === 'function') {
                            try { window.brambleWireRx(m.uart|0, m.ch); } catch(e) {}
                        }
                    } else if (m.type === 'gpio' && typeof m.pin === 'number') {
                        if (typeof window.brambleGpioRx === 'function') {
                            try { window.brambleGpioRx(m.pin, m.value); } catch(e) {}
                        }
                    } else if (m.type === 'eth' && m.data) {
                        if (typeof window.brambleEthRx === 'function') {
                            try { window.brambleEthRx(m.data); } catch(e) {}
                        }
                    }
                };
            }
        } else {
            console.log("[WASM-NET] BroadcastChannel unavailable");
        }
    });
    return 0;
}
void wire_cleanup(void) {}
void wire_poll(void) {}
int wire_add_link(const char *path, uint8_t type, uint8_t channel) {
    (void)channel;
    (void)type;
    EM_ASM({ console.log("[WASM-NET] wire link", UTF8ToString($0), "(BroadcastChannel)"); }, path);
    wire_connected = 1;
    return 0;
}
int wire_uart_active(int uart_num) { (void)uart_num; return wire_connected; }
void wire_send_uart(int uart_num, uint8_t byte) {
    EM_ASM({
        if (window.brambleWire) {
            try { window.brambleWire.postMessage({type:'uart', uart:$0, ch:$1}); } catch(e) {}
        }
    }, uart_num, byte);
    if (net_mirror) putchar((int)byte);
}
void wire_send_gpio(uint8_t pin, uint8_t value) {
    EM_ASM({
        if (window.brambleWire) {
            try { window.brambleWire.postMessage({type:'gpio', pin:$0, value:$1}); } catch(e) {}
        }
    }, pin, value);
}
int wire_eth_active(void) { return wire_connected; }
void wire_send_eth_frame(const uint8_t *frame, int len) {
    if (!frame || len <= 0 || len > 1522) return;
    /* ETH mesh between tabs via BroadcastChannel (binary structured clone).
     * JS onmessage with type 'eth' forwards to bramble_eth_push_rx() which
     * injects into vnet. Also mirror to WebSocket proxy for cross-machine mesh. */
    EM_ASM({
        try {
            var len = $1; var ptr = $0;
            var bytes = Module.HEAPU8.slice(ptr, ptr + len);
            if (window.brambleWire) {
                try { window.brambleWire.postMessage({type:'eth', data: bytes}); } catch(e) {}
            }
            if (typeof window !== 'undefined' && window.brambleNetSocket &&
                window.brambleNetSocket.readyState === 1 && window.brambleEthOverWs) {
                try {
                    var hdr = new Uint8Array([0x45, 0x54, 0x48, 0x00, len & 0xFF, (len >> 8) & 0xFF]);
                    var out = new Uint8Array(hdr.length + bytes.length);
                    out.set(hdr, 0); out.set(bytes, hdr.length);
                    window.brambleNetSocket.send(out);
                } catch(e) {}
            }
        } catch(e) {}
    }, frame, len);
}

/* W5500 live via WebSocket proxy pump (Node-safe).
 *
 * Queue model: w5500.c queues CONNECT/LISTEN/CLOSE/SEND bytes via
 * w5500_ws_tx_push(); JS drains them with bramble_w5500_pop_tx() and
 * forwards to the proxy socket (proxy does real TCP/UDP). Kept as a
 * shim for ABI compat: forwards into the same queue the socket-command
 * path uses. Works in browsers AND Node (no window access here). */
void bramble_ws_send_w5500(int sock_idx, const uint8_t *data, int len) {
    if (!data || len <= 0) return;
    if (sock_idx < 0 || sock_idx > 7) return;
    if (len > 2048) len = 2048;
    extern void w5500_ws_tx_push(const uint8_t *data, int len);
    uint8_t hdr[4];
    hdr[0] = 0x57; hdr[1] = (uint8_t)(sock_idx & 0xFF);
    hdr[2] = (uint8_t)(len & 0xFF); hdr[3] = (uint8_t)((len >> 8) & 0xFF);
    w5500_ws_tx_push(hdr, 4);
    w5500_ws_tx_push(data, len);
}

/* ============ TAP via WebSocket proxy + loopback ============ */
#define TAP_RX_SIZE 8192
static uint8_t tap_rx_buf[TAP_RX_SIZE];
static int tap_rx_head = 0, tap_rx_tail = 0;
static int tap_open = 0;

void bramble_tap_push_rx(const uint8_t *data, int len) {
    for (int i = 0; i < len; i++) {
        int nxt = (tap_rx_head + 1) % TAP_RX_SIZE;
        if (nxt == tap_rx_tail) break;
        tap_rx_buf[tap_rx_head] = data[i];
        tap_rx_head = nxt;
    }
}

int tapif_open(const char *name) {
    (void)name;
    EM_ASM({ console.log("[WASM-NET] tapif via WebSocket proxy (fake fd 42)"); });
    tap_open = 1;
    return 42;
}
void tapif_close(int fd) { (void)fd; tap_open = 0; }
int tapif_read(int fd, uint8_t *buf, int maxlen) {
    (void)fd;
    int n = 0;
    while (n < maxlen && tap_rx_tail != tap_rx_head) {
        buf[n++] = tap_rx_buf[tap_rx_tail];
        tap_rx_tail = (tap_rx_tail + 1) % TAP_RX_SIZE;
    }
    return n;
}
int tapif_write(int fd, const uint8_t *buf, int len) {
    (void)fd;
    if (!tap_open) return -1;
    /* Forward to WebSocket proxy if present (binary ETH frame) */
    EM_ASM({
        if (typeof window !== 'undefined' && window.brambleNetSocket &&
            window.brambleNetSocket.readyState === 1) {
            try {
                const len = $1;
                const ptr = $0;
                const bytes = Module.HEAPU8.slice(ptr, ptr + len);
                window.brambleNetSocket.send(bytes);
            } catch(e) {}
        }
    }, buf, len);
    return len;
}
