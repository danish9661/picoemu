# picoemu — usage, implementation & API reference

`picoemu` (npm) is the prebuilt WebAssembly distribution of the Bramble
RP2040/RP2350 emulator: three CPU cores (Cortex-M0+, Cortex-M33, RISC-V
Hazard3 RV32IMAC), full peripherals, a browser UI, a Node CLI, and 40+
demo UF2 images. Emulation core: MIT port of
[Night-Traders-Dev/Bramble](https://github.com/Night-Traders-Dev/Bramble).

## 1. Install & quick start

```sh
npm i picoemu
npx picoemu hello_world.uf2                 # arch auto-detected from UF2
npx picoemu uart_echo_rv32.uf2              # type a line, Enter submits
```

CLI options: `--arch auto|m0|m33|rv32` (default `auto` via UF2 family ID
`0xE48BFF56/59/5A`), `--clock 125`, `--steps 2000000`, `--timeout 30`,
`--cores 2`. Terminal input → firmware UART; firmware output → stdout;
Ctrl-C quits.

## 2. Browser UI

Serve the package directory over HTTP and open `index.html` (file:// will
not load the `.wasm`). Three demo dropdowns — RP2040 / M33 / RV32 — load
presets via `loadPreset(name)`; `Run` steps the emulator in
`requestAnimationFrame` chunks; serial monitor drains UART0; GPIO viewer
polls `bramble_get_gpio*`; `Send` writes the input box + CR(13).

## 3. Embedding in JS

```js
import createEmu from 'picoemu';            // resolves bramble.wasm.js
import fs from 'fs';

const mod = await createEmu({ print: () => {}, printErr: () => {} });
mod._bramble_init(1);                       // 0=M0+ 1=RV32 2=M33
mod._bramble_set_clock(125);
const uf2 = new Uint8Array(fs.readFileSync('hello_rv32.uf2'));
const ptr = mod._malloc(uf2.length);
mod.HEAPU8.set(uf2, ptr);
mod._bramble_load_uf2(ptr, uf2.length);     // returns nonzero on success
mod._free(ptr);
mod._bramble_reset();

mod._bramble_step(200000);                  // run N instructions
let ch, out = '';
while ((ch = mod._bramble_read_uart(0)) !== -1) out += String.fromCharCode(ch);
mod._bramble_write_uart(65);                // one byte into the firmware
```

Step in a loop (e.g. 2.5M instructions/frame at 60 fps ≈ 150 MHz
real-time). Drain UART once per frame, not per step. `HEAPU8`/`HEAPU32`
give zero-copy memory access.

## 4. API reference (all `bramble_*` exports)

### Lifecycle / image loading

| Export | Signature | Notes |
|---|---|---|
| `bramble_init` | `int (int arch)` | `0`=M0+ `1`=RV32 `2`=M33. Resets everything. |
| `bramble_reset` | `void ()` | Re-reset current firmware. |
| `bramble_load_uf2` | `int (ptr, len)` | Parses UF2 blocks into flash. Returns nonzero OK. |
| `bramble_load_elf` | `int (ptr, len)` | Same for ELF images. |
| `bramble_set_clock` | `void (int mhz)` | e.g. `125`. Drives timer/CLINT rates. |

### Run control / inspection

| Export | Signature | Notes |
|---|---|---|
| `bramble_step` | `int (int n)` | Execute up to N instructions; returns count run. Stops early on halt/GDB hit. |
| `bramble_is_halted` | `int ()` | Nonzero when hart0/core0 halted. |
| `bramble_get_core_state` | `void (core, *pc, *sp)` | Pass pointers to two u32 slots; reads PC/SP of core/hart 0/1. |
| `bramble_get_flash_ptr` | `uint8_t * ()` | Direct flash bytes (read via `HEAPU8`). |
| `bramble_get_sram_ptr` | `uint8_t * ()` | Direct SRAM bytes (RV32 bus SRAM). |
| `bramble_set_cores` | `void (int n)` | `1`/`2`. Gates hart1/core1 stepping + launch. Default 2. |
| `bramble_get_cores` | `int ()` | Active core count. |
| `bramble_set_quantum` | `void (int q)` | Cooperative step quantum. |
| `bramble_set_jit` | `void (int on)` | Hot-block JIT (default off in WASM). |
| `bramble_set_debug` | `void (int on, int core)` | Per-core instruction tracing. |
| `bramble_set_semihosting` | `void (int on)` | ARM semihosting SVC handling. |

### UART

| Export | Signature | Notes |
|---|---|---|
| `bramble_read_uart` | `int ()` | Next RX byte, or `-1` if empty. Drain in a loop. |
| `bramble_read_uart_bulk` | `int (*dest, max)` | Bulk drain into a buffer. |
| `bramble_write_uart` | `void (int ch)` | One byte into firmware RX. CR(13) submits shell lines; USB CDC firmware (MicroPython) also takes raw CR. |

### GPIO

| Export | Signature | Notes |
|---|---|---|
| `bramble_get_gpio` | `int (pin)` | Effective level (output if driven, else input). |
| `bramble_get_gpio_raw` | `int (pin)` | Raw input level. |
| `bramble_get_gpio_out` | `uint32_t ()` | Output latch bitmask. |
| `bramble_get_gpio_oe` | `uint32_t ()` | Output-enable bitmask. |
| `bramble_set_gpio` | `void (pin, val)` | Drive an input pin (buttons/sensors). |

### Memory

| Export | Signature | Notes |
|---|---|---|
| `bramble_mem_read32` | `uint32_t (addr)` | Bus-accurate read (peripherals included). |
| `bramble_mem_write32` | `void (addr, val)` | Bus-accurate write. |

Handy bases: UART0 `0x40070000` (RP2350) / `0x40034000` (RP2040),
SIO `0xD0000000`, CLINT `0xD0000100` (RV32: MTIME `+0x00`,
MTIMECMP0 `+0x08`), TIMER0 `0x400B0000`, SRAM `0x20000000` (520 KB
RP2350), flash `0x10000000`.

### Storage / SD / flash

| Export | Notes |
|---|---|
| `bramble_flash_save/load` | Persist flash to browser storage. |
| `bramble_flash_write(data, len, offset)` | Patch flash bytes. |
| `bramble_sdcard_load(data, len, spi)` / `bramble_emmc_load` | Attach file-backed block devices. |

### Network / GDB / devtools

`bramble_net_enable`, `bramble_sdd_add`, `bramble_eth_push_rx`,
`bramble_w5500_push_rx/status`, `bramble_ws_send_w5500` (WebSocket
bridges, pumped each frame by the UI); `bramble_gdb_enable/is_hit/
hit_core/break` (non-blocking RSP for the UI GDB panel);
`bramble_coverage_*`, `bramble_trace_*`, `bramble_hotspots_*`,
`bramble_profile_*`, `bramble_callgraph_*`, `bramble_gpiotrace_*`
(VCD), `bramble_irqlat_*`, `bramble_stackcheck_*`,
`bramble_symbols_load`, `bramble_watch_add`, `bramble_fault_add`,
`bramble_script_load`, `bramble_expect_*`, `bramble_heatmap_*`,
`bramble_set_buslog`. These mirror the native devtools; the UI's
Devtools panel drives them.

## 5. Implementation notes

- `src/bramble_wasm.c` replaces the CLI (`main.c`) with the exports
  above; peripherals/CPUs are shared with native (`src/`, `src/rp2350_rv/`,
  `src/rp2350_arm/`). WASM built by `build_wasm.sh` (Emscripten,
  MODULARIZE, 64–256 MB linear memory).
- RV32 demos (`test-firmware/*_rv32.S`) are generated by
  `test-firmware/gen_rv32.py` (position-independent rv32ima, auipc string
  refs) and linked by `test-firmware/rvlink.py` (handles
  R_JAL/BRANCH/HI20/LO12/PCREL) + `uf2conv.py` (family `0xE48BFF5A`).
  Rebuild: `python3 test-firmware/gen_rv32.py`, assemble with
  `clang --target=riscv32-unknown-elf -march=rv32ima -mabi=ilp32`,
  link, convert. Native sweep: `./test-firmware/sweep_all.sh build`
  (43 firmware, all archs).
- Dual-core: ARM uses host-threaded stepping with WFI fast-forward;
  RV32 harts step cooperatively; hart 1 launches via the SIO mailbox
  (`0xD00001C0` entry / `0x1C4` SP / `0x1C8` arg / `0x1CC` launch).
  See `dualcore_rv32` demo.
- littleOS Sage eval verified on M33 and RV32 (`sage print(6*7)` →
  `42.0000`); RV32 `health` temperature reads wrong on the vintage
  `littleos_pico2_riscv.uf2` (stale firmware, not an emulation bug).
- Performance: native ~86 MIPS (ICache) / ~148 MIPS (JIT); WASM
  ~22–25 MIPS. Budget ~2.5M steps/frame for 60 fps UI.

## 6. Building from source

Native: `cmake -S . -B build && cmake --build build -j && ctest
--test-dir build` (389 tests). WASM: `./build_wasm.sh` (needs emsdk;
output to `web/bramble.wasm.*`). Publish flow: manual
`.github/workflows/publish.yml` (branch + version + description →
npmjs `picoemu` + GPR `@danish9661/picoemu`).

## 7. Porting matrix (native → WASM)

Every emulation source is in the WASM build. Host-OS-bound modules are
replaced by shims with the same API:

| Native | WASM | Notes |
|---|---|---|
| all CPUs + peripherals (`cpu`, `thumb32`, `membus`, `gpio`, `timer`, `uart`, `spi`, `i2c`, `pwm`, `adc`, `dma`, `pio`, `nvic`, `clocks`, `usb`, `rtc`, `rom`, `gdb`, `storage`, `sdcard`, `emmc`, `fatfs`, `w5500`, `bme280`, `cyw43`, `devtools`, `vnet`, `sdd*`, `rv_*`, `m33_cpu`) | compiled as-is | bit-identical emulation |
| `corepool.c` (pthreads) | `wasm_net.c` cooperative pool | `bramble_set_cores(1\|2)`; `bramble.wasm.threads.*` (`-pthread`) for true workers via `serve_coop.py` |
| `netbridge.c` TCP | WebSocket bridge | `connectNet()` + `web/net_proxy.py` |
| `wire.c` unix sockets | `BroadcastChannel` | multi-tab mesh, same protocol |
| `tapif.c` TAP device | WebSocket proxy | fake fd + loopback, `net_proxy.py --ws` |
| `fuse_mount.c` FUSE | `fuse_mount_wasm.c` | OPFS/IDBFS persistent flash |
| `main.c` CLI | `bramble_wasm.c` exports + `web/cli.js` (node) | full API in §4 |

WiFi (CYW43) is compiled in and hooked to PIO + polled in every loop,
but has no guest driver in-box: end-to-end WiFi needs Pico-SDK-based
firmware (provides the CYW43 stack) plus a live backend
(`-net-live` native, proxy in browser).
