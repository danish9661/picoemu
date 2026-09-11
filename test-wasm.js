#!/usr/bin/env node
// WASM test suite: native ctest + WASM boots + MicroPython USB-CDC REPL.
import BrambleModule from './web/bramble.wasm.js';
import fs from 'fs';
import { execSync } from 'child_process';

const mod = await BrambleModule({ print: () => {}, printErr: () => {} });

// 1) Native reference (must be 377/377)
try {
  const out = execSync('ctest --test-dir build --output-on-failure 2>&1 | tail -n 5', { encoding: 'utf8' });
  console.log('[native]', out.trim().split('\n').pop());
} catch (e) {
  console.log('[native] ctest failed');
  process.exit(1);
}

// 2) WASM smoke across archs (mirrors Playwright log-not-error + UART checks)
async function wasmBoot(name, arch, steps, expectSub, drainCap = 4000) {
  mod._bramble_init(arch);
  mod._bramble_set_clock(125);
  const uf2 = new Uint8Array(fs.readFileSync('./web/' + name));
  const ptr = mod._malloc(uf2.length);
  mod.HEAPU8.set(uf2, ptr);
  const b = mod._bramble_load_uf2(ptr, uf2.length);
  mod._free(ptr);
  mod._bramble_reset();
  const s = mod._bramble_step(steps);
  let out = '', ch, n = 0;
  while ((ch = mod._bramble_read_uart(0)) !== -1 && n++ < drainCap) out += String.fromCharCode(ch);
  const ok = expectSub ? out.includes(expectSub) : true;
  console.log(`[wasm] ${name} blocks=${b} steps=${s} halted=${mod._bramble_is_halted()} uart=${JSON.stringify(out.slice(0, 80))} ${ok ? 'PASS' : 'FAIL'}`);
  if (!ok) process.exitCode = 1;
}

await wasmBoot('hello_world.uf2', 0, 50000, 'Hello from Bramble');
await wasmBoot('gpio_test.uf2', 0, 200000, 'LED ON');
await wasmBoot('timer_test.uf2', 0, 200000, 'Timer Test Complete');
// TinyUSB CDC via Pico SDK (user-IRQ pump + multi-packet IN needs full enum)
await wasmBoot('hello_usb.uf2', 0, 3000000, 'Hello, world!');
await wasmBoot('littleos_pico2.uf2', 2, 60000000, 'root@littleos', 150000);
await wasmBoot('littleos_pico2_riscv.uf2', 1, 200000, '');

// 3) MicroPython USB-CDC REPL (bundled v1.22.1 UF2): banner + eval 6*7==42.
// Boots via USB enumeration; input via bramble_write_uart (routed to USB CDC
// when enumerated, raw CR submits the line); output via CDC -> serial monitor.
for (const [f, arch] of [['micropython_rp2040.uf2', 0]]) {
  try {
    mod._bramble_init(arch);
    mod._bramble_set_clock(125);
    const uf2 = new Uint8Array(fs.readFileSync('./web/' + f));
    const ptr = mod._malloc(uf2.length);
    mod.HEAPU8.set(uf2, ptr);
    mod._bramble_load_uf2(ptr, uf2.length);
    mod._free(ptr);
    mod._bramble_reset();
    mod._bramble_step(3000000);
    let out = '', ch, n = 0;
    while ((ch = mod._bramble_read_uart(0)) !== -1 && n++ < 8000) out += String.fromCharCode(ch);
    const banner = out.includes('MicroPython') && out.includes('>>>');
    const cmd = 'print(6*7)\r';
    for (const c of cmd) mod._bramble_write_uart(c.charCodeAt(0));
    mod._bramble_step(8000000);
    out = ''; n = 0;
    while ((ch = mod._bramble_read_uart(0)) !== -1 && n++ < 8000) out += String.fromCharCode(ch);
    const eval42 = out.includes('42');
    console.log(`[wasm] ${f} banner=${banner ? 'PASS' : 'FAIL'} eval42=${eval42 ? 'PASS' : 'FAIL'} ${JSON.stringify(out.slice(0, 60))}`);
    if (!banner || !eval42) process.exitCode = 1;
  } catch (e) {
    console.log(`[wasm] ${f} ERROR ${e.message}`);
    process.exitCode = 1;
  }
}

console.log('WASM tests done (see docs/WASM.md for proxy/GDB/threads)');

// Opt-in headed WiFi E2E (minutes): RV32 join + DHCP through an
// in-process WS gateway. PICOEMU_TEST_GATEWAY=1 node test-wasm.js
if (process.env.PICOEMU_TEST_GATEWAY === '1') {
  console.log('[wasm] gateway E2E (opt-in)...');
  await import('./test-wasm-gateway.js');
}
