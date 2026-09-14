#!/usr/bin/env node
// picoemu — run RP2040/RP2350 (M0+/M33/RV32) UF2 firmware in Node.
// Usage: picoemu <firmware.uf2> [--arch auto|m0|m33|rv32] [--clock 125]
//        [--steps 2000000] [--timeout 30] [--cores 2] [--wifi]
//        [--gateway ws://localhost:5090/api/network-gateway] [--room myroom]
//        [--ble-hci ws://localhost:5090/api/ble-gateway]
//        [--board pico-eth|pico-eth2] [--board-spi 0|1] [--board-live]
//        [--net-w5500 ws://localhost:8765/w5500]  (live W5500 proxy pump)
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const mod = await (await import(path.join(HERE, 'bramble.wasm.js'))).default({
  print: () => {}, printErr: () => {},
});

const args = process.argv.slice(2);
const opt = (name, def) => {
  const i = args.indexOf(name);
  return i >= 0 && i + 1 < args.length ? args[i + 1] : def;
};
const file = args.find((a) => !a.startsWith('--'));
if (!file) {
  console.error('Usage: picoemu <firmware.uf2> [--arch auto|m0|m33|rv32] [--clock 125] [--steps 2000000] [--timeout 30] [--cores 2] [--wifi] [--gateway URL] [--room ID] [--ble-hci URL] [--board pico-eth|pico-eth2] [--board-spi 0|1] [--board-live] [--net-w5500 URL]');
  process.exit(2);
}
const u8 = new Uint8Array(fs.readFileSync(file));
// UF2 family ID lives at offset 28 of block 0
const FAM = { 0xe48bff56: 0, 0xe48bff59: 2, 0xe48bff5a: 1 };
let arch = { auto: -1, m0: 0, rv32: 1, m33: 2 }[opt('--arch', 'auto')] ?? -1;
if (arch < 0) {
  const fam = u8.length >= 32
    ? (u8[28] | (u8[29] << 8) | (u8[30] << 16) | (u8[31] << 24)) >>> 0
    : 0;
  arch = FAM[fam] ?? 0;
}
const clock = parseInt(opt('--clock', '125'), 10);
const budget = parseInt(opt('--steps', '2000000'), 10);
const timeoutS = parseFloat(opt('--timeout', '30'));
const cores = parseInt(opt('--cores', '2'), 10);

mod._bramble_init(arch);
mod._bramble_set_clock(clock);
try { mod._bramble_set_cores(cores); } catch { /* older builds */ }
const ptr = mod._malloc(u8.length);
mod.HEAPU8.set(u8, ptr);
const ok = mod._bramble_load_uf2(ptr, u8.length);
mod._free(ptr);
if (!ok) { console.error('picoemu: UF2 load failed'); process.exit(1); }
mod._bramble_reset();

// Optional CYW43 WiFi (--wifi enables the model; with --gateway the
// fake DHCP/DNS server is disabled like native -nodhcp).
{
  const wantWifi = args.includes('--wifi') || opt('--gateway', '') !== '' || opt('--ble-hci', '') !== '';
  if (wantWifi) {
    const nodhcp = opt('--gateway', '') !== '' ? 1 : 0;
    try { mod._bramble_wifi_enable(nodhcp); } catch {}
  }
  try { if (opt('--ble-hci', '') !== '') mod._bramble_bt_hci_enable(1); } catch {}
}

// pico-eth/pico-eth2 board (WIZnet W5500-EVB-Pico/Pico2): separate SPI
// board, off by default. --board-live mirrors SEND to the WS proxy like
// -net-live. Pico2 wiring is identical; only the SoC differs.
{
  const board = opt('--board', '');
  if (board) {
    if (board !== 'pico-eth' && board !== 'pico-eth2') { console.error(`picoemu: unknown board '${board}' (use pico-eth|pico-eth2)`); process.exit(2); }
    let spi = parseInt(opt('--board-spi', '0'), 10);
    if (!(spi === 0 || spi === 1)) { console.error('picoemu: --board-spi must be 0 or 1'); process.exit(2); }
    const live = args.includes('--board-live') ? 1 : 0;
    try { mod._bramble_board_eth(1, live, spi); } catch {}
    // Live needs the proxy pump outlet too (same as --net-w5500): without
    // an explicit URL, board-live defaults to the local proxy /w5500 path
    // so CONNECT/LISTEN/CLOSE/SEND actually reach net_proxy.py.
    if (live && !opt('--net-w5500', '') && !(opt('--net-url', '') || opt('--net', ''))) {
      try { opt._autoW5500 = 'ws://localhost:8765/w5500'; } catch {}
    }
  }
}

if (process.stdin.isTTY) process.stdin.setRawMode(true);
process.stdin.resume();
process.stdin.on('data', (d) => {
  for (const b of d) {
    if (b === 3) process.exit(0); // Ctrl-C
    mod._bramble_write_uart(b);
  }
});

const tick = () => new Promise((r) => setImmediate(r));
const t0 = Date.now();
let done = 0;
const CHUNK = 100000;

// Optional network gateway (raw ETH frames, OpenHW-gateway protocol).
let gw = null;
{
  let url = opt('--gateway', '');
  const room = opt('--room', '');
  if (url) {
    if (room) url += (url.includes('?') ? '&' : '?') + 'sessionId=' + encodeURIComponent(room);
    gw = new WebSocket(url);
    gw.binaryType = 'arraybuffer';
    gw.onopen = () => { try { mod._bramble_eth_set_uplink(1); } catch {} };
    gw.onclose = () => { try { mod._bramble_eth_set_uplink(0); } catch {} };
    gw.onmessage = (e) => {
      const arr = e.data instanceof ArrayBuffer ? new Uint8Array(e.data) : new Uint8Array(0);
      if (arr.length < 14 || arr.length > 1522) return;
      const p = mod._malloc(arr.length);
      mod.HEAPU8.set(arr, p);
      try { mod._bramble_eth_push_rx(p, arr.length); } catch {}
      mod._free(p);
    };
    gw.onerror = (e) => console.error('picoemu: gateway error ' + url + (e && e.message ? ' (' + e.message + ')' : ''));
  }
}
// Optional live-W5500 proxy socket (shared net_proxy /w5500 path; the proxy
// dials real TCP/UDP. NOT the OpenHW gateway: W5500 frames are socket-level
// CONNECT/LISTEN/CLOSE/SEND, not raw ETH).
// Auto-uses the Net socket when it targets /w5500, else --net-w5500 URL.
let w5500ws = null;
{
  let url = opt('--net-w5500', '') || (opt._autoW5500 || '');
  if (!url) {
    const netUrl = opt('--net-url', '') || opt('--net', '');
    if (netUrl && /\/w5500/.test(netUrl)) url = netUrl;
  }
  if (url) {
    w5500ws = new WebSocket(url);
    w5500ws.binaryType = 'arraybuffer';
    w5500ws.onmessage = (e) => {
      const arr = e.data instanceof ArrayBuffer ? new Uint8Array(e.data)
        : typeof e.data === 'string' ? new TextEncoder().encode(e.data) : new Uint8Array(0);
      if (!arr.length) return;
      if (arr[0] === 0x53 && arr.length >= 4) {
        try { mod._bramble_w5500_push_status(arr[1], arr[3]); } catch {}
      } else if (arr[0] < 8 && arr.length >= 3) {
        const ln = arr[1] | (arr[2] << 8);
        const payload = arr.slice(3, 3 + ln);
        const p = mod._malloc(payload.length);
        mod.HEAPU8.set(payload, p);
        try { mod._bramble_w5500_push_rx(arr[0], p, payload.length); } catch {}
        mod._free(p);
      } else if (arr[0] === 0x57 && arr.length >= 4) {
        const sock = arr[1], ln = arr[2] | (arr[3] << 8);
        const payload = arr.slice(4, 4 + ln);
        const p = mod._malloc(payload.length);
        mod.HEAPU8.set(payload, p);
        try { mod._bramble_w5500_push_rx(sock, p, payload.length); } catch {}
        mod._free(p);
      }
    };
    w5500ws.onerror = (e) => console.error('picoemu: net-w5500 error ' + url + (e && e.message ? ' (' + e.message + ')' : ''));
  }
}
// Optional BLE HCI uplink (raw H4 packets, Bumble/gateway ble-gateway protocol).
let blehci = null;
{
  const url = opt('--ble-hci', '');
  if (url) {
    blehci = new WebSocket(url);
    blehci.binaryType = 'arraybuffer';
    blehci.onmessage = (e) => {
      const arr = e.data instanceof ArrayBuffer ? new Uint8Array(e.data) : new Uint8Array(0);
      if (arr.length < 2 || arr.length > 1088) return;
      const p = mod._malloc(arr.length);
      mod.HEAPU8.set(arr, p);
      try { mod._bramble_bt_hci_push_rx(p, arr.length); } catch {}
      mod._free(p);
    };
    blehci.onerror = (e) => console.error('picoemu: ble-hci error ' + url + (e && e.message ? ' (' + e.message + ')' : ''));
  }
}
process.stdout.write('');
while (done < budget && (Date.now() - t0) / 1000 < timeoutS) {
  mod._bramble_step(Math.min(CHUNK, budget - done));
  done += CHUNK;
  await tick(); // let stdin/stdio/events fire
  if (gw && gw.readyState === 1) {
    for (let i = 0; i < 16; i++) {
      const p = mod._malloc(2048);
      let got = -1;
      try { got = mod._bramble_eth_pop_tx(p, 2048); } catch { mod._free(p); break; }
      if (got <= 0) { mod._free(p); break; }
      try { gw.send(mod.HEAPU8.slice(p, p + got)); } catch {}
      mod._free(p);
    }
  }
  if (blehci && blehci.readyState === 1) {
    for (let i = 0; i < 16; i++) {
      const p = mod._malloc(2048);
      let got = -1;
      try { got = mod._bramble_bt_hci_pop_tx(p, 2048); } catch { mod._free(p); break; }
      if (got <= 0) { mod._free(p); break; }
      try { blehci.send(mod.HEAPU8.slice(p, p + got)); } catch {}
      mod._free(p);
    }
  }
  // W5500 proxy pump (Node): forward queued CONNECT/LISTEN/CLOSE/SEND to
  // the --net-w5500 proxy socket. Same shared net_proxy as the browser Net
  // panel; NOT the OpenHW gateway (W5500 = socket-level TCP/UDP, not ETH).
  if (w5500ws && w5500ws.readyState === 1) {
    let budget = 0;
    try { budget = mod._bramble_w5500_tx_len(); } catch { budget = 0; }
    if (budget > 0) {
      const n = Math.min(budget, 8192);
      const p = mod._malloc(n);
      let got = 0;
      try { got = mod._bramble_w5500_pop_tx(p, n); } catch { got = 0; }
      if (got > 0) { try { w5500ws.send(mod.HEAPU8.slice(p, p + got)); } catch {} }
      mod._free(p);
    }
  }
  let s = '', ch, n = 0;
  while ((ch = mod._bramble_read_uart(0)) !== -1 && n++ < 65536) s += String.fromCharCode(ch);
  if (s) process.stdout.write(s);
  try { if (mod._bramble_is_halted()) break; } catch { /* ignore */ }
}
process.exit(0);
