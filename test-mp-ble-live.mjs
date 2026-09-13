import BrambleModule from './web/bramble.wasm.js';
import fs from 'fs';

// Live-MP BLE test on STOCK Pico-W build (has bluetooth module).
// Usage: node test-mp-ble-live.mjs [uf2path]
// Boots MP, waits for REPL, runs BLE.active(True) + gap_advertise,
// reports HCI opcodes seen on the H4 trace via CYW43 trace env.
const uf2path = process.argv[2] || (process.env.HOME + '/mpbuild-stock/firmware.uf2');
const mod = await BrambleModule({ print: () => {}, printErr: () => {} });
mod._bramble_init(0);
mod._bramble_set_clock(125);
const uf2 = new Uint8Array(fs.readFileSync(uf2path));
const ptr = mod._malloc(uf2.length);
mod.HEAPU8.set(uf2, ptr);
const blocks = mod._bramble_load_uf2(ptr, uf2.length);
mod._free(ptr);
mod._bramble_reset();
try { mod._bramble_wifi_enable(0); } catch {}
console.log(`[mp-ble] load ok=${blocks} from ${uf2path}`);
console.log(`[mp-ble] expecting stock Pico-W MP boot (~30-60s wall); stepping...`);

function drain(cap = 30000) {
  let out = '', ch, n = 0;
  while ((ch = mod._bramble_read_uart(0)) !== -1 && n++ < cap) out += String.fromCharCode(ch);
  return out;
}
function sendLine(s) {
  for (const c of s) mod._bramble_write_uart(c.charCodeAt(0));
  mod._bramble_write_uart(13); // CR submits REPL line
}
async function waitRepl(timeoutSteps = 400) {
  let acc = '';
  for (let i = 0; i < timeoutSteps; i++) {
    mod._bramble_step(2000000);
    acc += drain();
    if (acc.includes('>>>')) return acc;
    if (i % 50 === 49) console.log(`[mp-ble] still booting... ${(i + 1) * 2}M steps`);
  }
  return acc;
}

let boot = await waitRepl(1500);
console.log('[mp-ble] boot has MP banner:', boot.includes('MicroPython'), 'has >>>:', boot.includes('>>>'));
if (!boot.includes('>>>')) {
  console.log('[mp-ble] BOOT TAIL:', JSON.stringify(boot.slice(-400)));
  console.log('[mp-ble] FAIL: no REPL');
  process.exit(1);
}
// 1) bluetooth import
sendLine('import bluetooth; print("BT-IMPORT-OK")');
let s1 = '';
for (let i = 0; i < 60; i++) { mod._bramble_step(1000000); s1 += drain(); if (s1.includes('BT-IMPORT-OK')) break; }
console.log('[mp-ble] import bluetooth:', s1.includes('BT-IMPORT-OK') ? 'PASS' : 'FAIL');
if (!s1.includes('BT-IMPORT-OK')) { console.log('[mp-ble] OUT:', JSON.stringify(s1.slice(-500))); process.exit(1); }
// 2) active(True)
sendLine('ble = bluetooth.BLE(); ble.active(True); print("ACTIVE-OK")');
let s2 = '';
for (let i = 0; i < 120; i++) { mod._bramble_step(1000000); s2 += drain(); if (s2.includes('ACTIVE-OK')) break; }
console.log('[mp-ble] BLE.active(True):', s2.includes('ACTIVE-OK') ? 'PASS' : 'FAIL');
if (!s2.includes('ACTIVE-OK')) { console.log('[mp-ble] OUT:', JSON.stringify(s2.slice(-800))); process.exit(1); }
// 3) gap_advertise
sendLine('ble.gap_advertise(100000, b"\\x02\\x01\\x06\\x05\\x09MPBT1"); print("ADV-CALLED")');
let s3 = '';
for (let i = 0; i < 120; i++) { mod._bramble_step(1000000); s3 += drain(); if (s3.includes('ADV-CALLED')) break; }
console.log('[mp-ble] gap_advertise returns:', s3.includes('ADV-CALLED') ? 'PASS (rc=0)' : 'FAIL');
console.log('[mp-ble] OUT:', JSON.stringify(s3.slice(-500)));
if (!s3.includes('ADV-CALLED')) process.exit(1);
console.log('[mp-ble] LIVE-MP HCI PATH DONE (check native trace for 0x2006/0x2008/0x200a opcodes)');
