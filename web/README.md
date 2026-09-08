# picoemu

RP2040 / RP2350 (Cortex-M0+, Cortex-M33, RISC-V Hazard3) emulator that runs
in Node or the browser via WebAssembly. Boots real UF2 firmware —
littleOS, MicroPython, and 40+ bare-metal peripheral demos included.

Credit: emulation core is a WASM port of
[Night-Traders-Dev/Bramble](https://github.com/Night-Traders-Dev/Bramble)
(MIT); WASM build, browser UI and packaging by
[danish9661/picoemu](https://github.com/danish9661/picoemu).

## Install

```sh
npm i picoemu
```

## CLI — `npx picoemu firmware.uf2`

```sh
npx picoemu hello_world.uf2                    # arch auto-detected from UF2
npx picoemu ./web/uart_echo_rv32.uf2           # type a line, Enter submits
npx picoemu fw.uf2 --arch rv32 --clock 125 --steps 2000000 --timeout 30 --cores 2
npx picoemu wifi_fw.uf2 --gateway ws://localhost:5099/api/network-gateway --room lab
```

Type into the terminal to send UART bytes (Ctrl-C quits); firmware output
streams to stdout. `--arch` accepts `auto` (default, UF2 family ID),
`m0`, `m33`, `rv32`.

## JS API

```js
import createEmu from 'picoemu';
import fs from 'fs';

const mod = await createEmu({ print: () => {}, printErr: () => {} });
mod._bramble_init(1);            // 0=M0+ 1=RV32 2=M33
mod._bramble_set_clock(125);     // MHz
const uf2 = new Uint8Array(fs.readFileSync('hello_rv32.uf2'));
const ptr = mod._malloc(uf2.length);
mod.HEAPU8.set(uf2, ptr);
mod._bramble_load_uf2(ptr, uf2.length);
mod._free(ptr);
mod._bramble_reset();
mod._bramble_step(200000);       // run N instructions

let ch, out = '';
while ((ch = mod._bramble_read_uart(0)) !== -1) out += String.fromCharCode(ch);
console.log(out);                // Hello from Bramble RV32!
mod._bramble_write_uart(65);     // send 'A' to firmware
```

More entry points: `_bramble_load_elf`, `_bramble_get_gpio` /
`_bramble_set_gpio`, `_bramble_mem_read32` / `_bramble_mem_write32`,
`_bramble_set_cores`, `_bramble_is_halted`. Full reference with every
export: [docs/PICOEMU.md](https://github.com/danish9661/picoemu/blob/main/docs/PICOEMU.md).

## Browser

`index.html` is a ready-made UI (serial monitor, GPIO viewer, three demo
dropdowns: RP2040 / M33 / RV32). Serve the package dir and open it, or copy
`bramble.wasm.*` + `index.html` into your app.

## Firmware in this package

Top level: one UF2 per demo (`*_test.uf2` RP2040, `*_pico2.uf2` M33,
`*_rv32.uf2` RV32) plus `littleos*.uf2` and `micropython*.uf2`.
`examples/` mirrors the same set for the UI dropdowns.

## License

MIT — see LICENSE (upstream © 2025 Night-Traders-Dev).
