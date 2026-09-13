---
disable: true
---
Build mode: active - this file is for build, not plan.

# AGENTS.md — Bramble WASM RP2350 Browser Emulator

## Project Goal

Compile Bramble (C, native RP2040/RP2350 emulator) to WebAssembly using Emscripten,
then build a TypeScript web UI layer to run the emulator in a browser at ~8-10x
the speed of pure-JavaScript emulators (rp2040js, GhostRoboticsLab/rp2350js_emulator).

## Why Bramble

- v0.50.0, 396 tests passing
- Complete RP2040 + RP2350 (ARM + RISC-V Hazard3)
- ALL peripherals: UART, SPI, I2C, PWM, ADC, DMA, PIO, GPIO, USB, WiFi (CYW43),
  SD card, eMMC, networking (TAP/W5500/virtual Ethernet), sensors (BME280)
- Dual-core support (host pthreads)
- Built-in ICache (64K decoded entries) + optional JIT (-jit flag)
- 18 dev tools (semihosting, coverage, trace, VCD, GDB, etc.)
- Cycle-accurate timing model (configurable clock)
- MIT license
- Written in C99 → compiles to WASM via Emscripten

## Directory Structure

```
Bramble/
├── src/
│   ├── main.c              # CLI entry point (REPLACE with WASM exports)
│   ├── cpu.c               # ARM Cortex-M0+ core (O(1) dispatch)
│   ├── instructions.c      # Thumb instruction implementations
│   ├── thumb32.c           # Thumb-2 (Cortex-M33)
│   ├── membus.c            # Memory bus routing
│   ├── uf2.c / elf.c       # Firmware loaders
│   ├── gpio.c              # GPIO peripheral
│   ├── timer.c             # 64-bit timer + alarms
│   ├── uart.c              # Dual PL011 UART
│   ├── spi.c               # Dual PL022 SPI
│   ├── i2c.c               # Dual DW_apb_i2c
│   ├── pwm.c               # 8-slice PWM
│   ├── adc.c               # ADC + temp sensor
│   ├── dma.c               # 12-channel DMA
│   ├── pio.c               # Dual PIO block (full instruction exec)
│   ├── nvic.c              # NVIC interrupt controller
│   ├── clocks.c            # Clocks, XOSC, PLLs, Watchdog
│   ├── usb.c               # USB host + CDC
│   ├── rtc.c               # RTC
│   ├── rom.c               # ROM function table
│   ├── gdb.c               # GDB remote serial protocol
│   ├── storage.c           # Flash write-through
│   ├── sdcard.c            # SD card SPI emulation
│   ├── emmc.c              # eMMC SPI emulation
│   ├── netbridge.c         # UART-to-TCP bridge
│   ├── vnet.c              # Virtual network bus
│   ├── wire.c              # Multi-instance wiring
│   ├── tapif.c             # TAP bridge
│   ├── w5500.c             # W5500 Ethernet
│   ├── cyw43.c             # CYW43 WiFi
│   ├── bme280.c            # BME280 sensor model
│   ├── sdd.c / sdd_thermo.c # Software-defined devices
│   ├── fatfs.c             # FAT16 helpers
│   ├── fuse_mount.c        # FUSE mount (DISABLE for WASM)
│   ├── corepool.c          # Host-threaded execution (DISABLE for WASM)
│   ├── devtools.c          # Developer tools
│   ├── rp2350_rv/
│   │   ├── rv_cpu.c        # Hazard3 RV32IMAC CPU (93+ instructions)
│   │   ├── rv_clint.c      # CLINT interrupt controller
│   │   ├── rv_membus.c     # RP2350 memory bus (520KB SRAM)
│   │   ├── rv_bootrom.c    # Minimal RISC-V bootrom
│   │   ├── rp2350_periph.c # RP2350 peripheral layer
│   │   └── picobin.c       # picobin IMAGE_DEF parser
│   └── rp2350_arm/
│       └── m33_cpu.c       # Cortex-M33 core
├── include/                 # Header files
├── tests/                   # Test suite (396 tests)
├── test-firmware/           # Test firmware binaries
├── docs/                    # Documentation
├── CMakeLists.txt           # Native build (CMake)
└── build.sh                 # Native build script
```

## Build Overview

### Step 1: Emscripten Build System

1. Install Emscripten SDK (emsdk)
2. Create `CMakeLists.wasm` or `build_wasm.sh` for Emscripten cross-compilation
3. Disable features that don't work in browser:
   - `fuse_mount.c` (no FUSE in browser)
   - `corepool.c` (no pthreads — use cooperative scheduling or SharedArrayBuffer)
   - `tapif.c` (no TAP device — use WebSockets/WebRTC instead)
   - `netbridge.c` TCP bridge (no raw sockets — use WebSocket proxy)
   - `wire.c` Unix sockets (no Unix domain sockets in browser)
4. Keep all peripheral emulation (GPIO, UART, SPI, I2C, PWM, ADC, DMA, PIO, USB, etc.)
5. Keep the RV32 RISC-V core (`rp2350_rv/`)

### Phase 2: WASM Exports

Replace `src/main.c` CLI logic with exported WASM functions. Create `src/bramble_wasm.c`:

```c
// Exported to JS via Emscripten
EMSCRIPTEN_KEEPALIVE
int bramble_init(int arch);           // 0=M0+, 1=M33, 2=RV32

EMSCRIPTEN_KEEPALIVE
void bramble_reset(void);

EMSCRIPTEN_KEEPALIVE
int bramble_load_uf2(const uint8_t *data, int len);

EMSCRIPTEN_KEEPALIVE
int bramble_load_elf(const uint8_t *data, int len);

EMSCRIPTEN_KEEPALIVE
int bramble_step(int n_instructions);  // Run N steps, return cycles used

EMSCRIPTEN_KEEPALIVE
void bramble_set_clock(int freq_mhz);

EMSCRIPTEN_KEEPALIVE
int bramble_read_uart(int port);       // Returns char or -1

EMSCRIPTEN_KEEPALIVE
void bramble_write_uart(int port, int ch);

EMSCRIPTEN_KEEPALIVE
int bramble_get_gpio(int pin);

EMSCRIPTEN_KEEPALIVE
void bramble_set_gpio(int pin, int val);

EMSCRIPTEN_KEEPALIVE
uint32_t bramble_mem_read32(uint32_t addr);

EMSCRIPTEN_KEEPALIVE
void bramble_mem_write32(uint32_t addr, uint32_t val);

// Direct memory access (fast path — no function call overhead)
// JS accesses: Module.HEAPU8, Module.HEAPU32
```

### Phase 3: WASM Compilation Command

```bash
emcc \
  src/bramble_wasm.c \
  src/cpu.c src/instructions.c src/thumb32.c src/membus.c \
  src/uf2.c src/elf.c src/gpio.c src/timer.c src/uart.c \
  src/spi.c src/i2c.c src/pwm.c src/adc.c src/dma.c \
  src/pio.c src/nvic.c src/clocks.c src/usb.c src/rtc.c \
  src/rom.c src/gdb.c src/storage.c src/sdcard.c src/emmc.c \
  src/fatfs.c src/w5500.c src/bme280.c src/cyw43.c \
  src/devtools.c src/vnet.c src/sdd.c src/sdd_thermo.c \
  src/rp2350_rv/rv_cpu.c src/rp2350_rv/rv_clint.c \
  src/rp2350_rv/rv_membus.c src/rp2350_rv/rv_bootrom.c \
  src/rp2350_rv/rp2350_periph.c src/rp2350_rv/picobin.c \
  src/rp2350_arm/m33_cpu.c \
  -Iinclude/ -Iinclude/rp2040 -Iinclude/rp2350_rv -Iinclude/rp2350_arm \
  -O3 -msimd128 \
  -s WASM=1 \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s MODULARIZE=1 \
  -s EXPORT_NAME="BrambleModule" \
  -s EXPORTED_FUNCTIONS='["_bramble_init","_bramble_reset","_bramble_load_uf2","_bramble_load_elf","_bramble_step","_bramble_set_clock","_bramble_read_uart","_bramble_write_uart","_bramble_get_gpio","_bramble_set_gpio","_bramble_mem_read32","_bramble_mem_write32","_free","_malloc"]' \
  -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","getValue","setValue"]' \
  -s INITIAL_MEMORY=134217728 \
  -s MAXIMUM_MEMORY=268435456 \
  -s STACK_SIZE=1048576 \
  -s NO_EXIT_RUNTIME=1 \
  -s ENVIRONMENT='web' \
  -o web/bramble.wasm.js
```

### Phase 4: TypeScript Bridge + Web UI

Create `web/` directory with:

```
web/
├── index.html              # Main page
├── bramble.ts              # TypeScript bridge over WASM
├── bramble.d.ts            # WASM type declarations
├── ui/
│   ├── serial-monitor.ts   # UART output display
│   ├── pin-viewer.ts       # GPIO state visualization
│   ├── display.ts          # Canvas for any display output
│   ├── code-editor.ts      # Firmware upload / code editor
│   └── toolbar.ts          # Run/pause/reset/clock controls
├── package.json            # npm dependencies
└── tsconfig.json
```

#### bramble.ts (Bridge Layer)

```typescript
// Minimal JS layer — minimizes WASM↔JS boundary crossings

let bramble: any;
let uartBuffer: string = '';

export async function init() {
  const factory = (await import('./bramble.wasm.js')).default;
  bramble = await factory();
  bramble.bramble_init(2); // RP2350 RV32
}

export function loadUF2(data: Uint8Array) {
  const ptr = bramble._malloc(data.length);
  bramble.HEAPU8.set(data, ptr);
  bramble.bramble_load_uf2(ptr, data.length);
  bramble._free(ptr);
}

// Run emulator in requestAnimationFrame loop
// 150MHz / 60fps = 2.5M instructions per frame
export function runFrame() {
  const CYCLES_PER_FRAME = 2_500_000;
  bramble.bramble_step(CYCLES_PER_FRAME);

  // Collect UART output (once per frame, not per instruction)
  while (true) {
    const ch = bramble.bramble_read_uart(0);
    if (ch === -1) break;
    uartBuffer += String.fromCharCode(ch);
  }

  // Update GPIO display
  const gpio25 = bramble.bramble_get_gpio(25);
  updateLED(gpio25);

  requestAnimationFrame(runFrame);
}

// Fast path: direct memory access (no function call overhead)
export function readMemory32(addr: number): number {
  return bramble.HEAPU32[addr >> 2];
}
```

### Phase 5: Performance Optimization

Key strategies to maximize WASM speed:

1. **Batch operations**: Run 2.5M instructions inside WASM per frame, cross boundary
   to JS only ONCE per frame (~16ms). Do NOT cross per instruction.

2. **Direct memory access**: JS reads/writes WASM linear memory directly via
   `Module.HEAPU8` / `Module.HEAPU32` — zero function call overhead.

3. **WASM SIMD**: Use `-msimd128` for SHA-256, bulk memory copies, PIO stepping.

4. **No allocation in hot path**: Pre-allocate all buffers before emulation starts.

5. **Minimize peripheral callbacks**: Only notify JS when state changes (UART TX,
   GPIO change, display update). Don't poll every cycle.

6. **SharedArrayBuffer** (optional, advanced): If COEP headers are set, use
   SharedArrayBuffer for zero-copy WASM↔JS memory sharing.

### Phase 6: Test & Validate

1. Run existing 396 tests via Emscripten (compile test_suite.c to WASM)
2. Boot `hello_world.uf2` in browser, verify UART output
3. Boot `littleos.uf2` in browser, verify OS boots
4. Boot `micropython.uf2`, verify REPL works
5. Boot `littleos_pico2_riscv.uf2`, verify RP2350 RISC-V mode
6. Benchmark: compare instructions/second vs GhostRoboticsLab/rp2350js_emulator
7. Target: ≥5x speed improvement over pure-JS emulators

## What to Disable for WASM

| File | Reason | Action |
|------|--------|--------|
| `src/main.c` | CLI entry point | Replace with `bramble_wasm.c` exports |
| `src/fuse_mount.c` | No FUSE in browser | `#ifdef` guard or exclude from build |
| `src/corepool.c` | No pthreads in WASM (without SharedArrayBuffer) | Disable, use cooperative stepping |
| `src/tapif.c` | No TAP device in browser | Disable, use WebSocket proxy instead |
| `src/netbridge.c` TCP | No raw sockets in browser | Disable, use WebSocket proxy instead |
| `src/wire.c` Unix | No Unix domain sockets in browser | Disable |

## What to Keep for WASM

All of these must compile to WASM:

- CPU cores: `cpu.c`, `thumb32.c`, `rp2350_rv/rv_cpu.c`, `rp2350_arm/m33_cpu.c`
- Memory bus: `membus.c`, `rp2350_rv/rv_membus.c`
- All peripherals: `gpio.c`, `timer.c`, `uart.c`, `spi.c`, `i2c.c`, `pwm.c`,
  `adc.c`, `dma.c`, `pio.c`, `nvic.c`, `clocks.c`, `usb.c`, `rtc.c`, `rom.c`
- Firmware loaders: `uf2.c`, `elf.c`, `rp2350_rv/picobin.c`
- RISC-V support: `rv_clint.c`, `rv_bootrom.c`, `rp2350_periph.c`
- Storage: `storage.c`, `sdcard.c`, `emmc.c`, `fatfs.c`
- Networking (via WebSocket): `vnet.c`, `w5500.c`, `cyw43.c`
- Dev tools: `devtools.c`, `gdb.c`
- Sensor models: `bme280.c`, `sdd.c`, `sdd_thermo.c`

## Key Performance Numbers (Expected)

| Metric | rp2040js (pure JS) | Bramble WASM | Improvement |
|--------|---------------------|--------------|-------------|
| Instructions/sec | ~5-15M | ~50-100M | **5-10x** |
| UART throughput | ~10K chars/sec | ~100K chars/sec | **10x** |
| Boot time (hello_world) | ~2s | ~0.2s | **10x** |
| MicroPython REPL | Laggy, slow | Smooth | **5-8x** |
| Frame budget (60fps) | ~2.5M cycles | ~15M cycles | **6x** |

## Risk: WASM↔JS Boundary Crossing

The biggest performance risk is calling WASM functions from JS too frequently.
Each call costs ~50-200ns. If you cross 1000 times per frame at 60fps, that's:
1000 * 100ns * 60 = 6ms/frame overhead (36% of 16ms budget).

**Solution**: Run ALL emulation inside WASM. Only cross back to JS once per frame
to read UART output and update the display. Use direct memory access
(`HEAPU32[addr >> 2]`) for fast state inspection without function calls.

## Overview

1. Install Emscripten SDK
2. Create `build_wasm.sh` script
3. Create `src/bramble_wasm.c` with WASM exports
4. Compile Bramble to WASM, fix any build errors
5. Create minimal `web/index.html` + `web/bramble.ts`
6. Boot `hello_world.uf2` in browser
7. Add serial monitor UI
8. Add GPIO pin viewer
9. Add firmware upload (drag-and-drop UF2)
10. Benchmark and optimize
