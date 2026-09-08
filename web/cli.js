#!/usr/bin/env node
// picoemu — run RP2040/RP2350 (M0+/M33/RV32) UF2 firmware in Node.
// Usage: picoemu <firmware.uf2> [--arch auto|m0|m33|rv32] [--clock 125]
//        [--steps 2000000] [--timeout 30] [--cores 2]
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
  console.error('Usage: picoemu <firmware.uf2> [--arch auto|m0|m33|rv32] [--clock 125] [--steps 2000000] [--timeout 30] [--cores 2]');
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
process.stdout.write('');
while (done < budget && (Date.now() - t0) / 1000 < timeoutS) {
  mod._bramble_step(Math.min(CHUNK, budget - done));
  done += CHUNK;
  await tick(); // let stdin/stdio events fire
  let s = '', ch, n = 0;
  while ((ch = mod._bramble_read_uart(0)) !== -1 && n++ < 65536) s += String.fromCharCode(ch);
  if (s) process.stdout.write(s);
  try { if (mod._bramble_is_halted()) break; } catch { /* ignore */ }
}
process.exit(0);
