# picoemu — Session Summary (for model switch)

> Historical handoff note (started 2026-09-06, last updated 2026-09-08).
> Current user/API reference: `docs/PICOEMU.md`. Current audit state:
> `docs/audit_report.md` re-triage (all actionable items closed).
> Test suite is at 388/388; counts below are historical snapshots.

**Date:** 2026-09-06
**Repo:** `danish9661/picoemu` (fork of `Night-Traders-Dev/Bramble` MIT; npm `picoemu`)
**Current version:** `v0.50.0` + 5 commits (M33 dual-core live), `378/378` tests passing
**This session model:** `opencode/muse-spark-1.3-contributor-free`
**Goal of this file:** Let a new session resume without the huge turn history that causes `invalid_request_error`. Paste this file as context in the new session.

---

## 1. What This Repo Is

From-scratch RP2040 / RP2350 emulator in C99. Tri-arch:
- `RP2040 Cortex-M0+` (`-arch m0+`)
- `RP2350 Cortex-M33` (`-arch m33`, Thumb-2) + 520KB SRAM
- `RP2350 Hazard3 RV32IMAC+Zba/Zbb/Zbs/Zcb/Zcmp` (`-arch rv32`)

Peripherals: GPIO, UART PL011, SPI PL022, I2C, Timer, PWM, ADC, DMA 12ch, PIO 2+1, NVIC, Clocks, USB CDC, RTC, ROM, SIO, VREG, etc. + RP2350 TICKS/POWMAN/QMI/OTP/BOOTRAM/TIMER1 + SD/eMMC SPI, W5500, BME280, CYW43 TAP, VNet, SDD.

**WASM port:** `Emscripten 6.0.9` (`emsdk/`), `build_wasm.sh` → `web/bramble.wasm.{js,wasm}` (236K), `web/index.html` browser UI, `web/examples/` presets, `pages.yml` deploys `web/` to `https://danish9661.github.io/picoemu/`.

---

## 2. What We Were Doing

Port the native emulator to the browser at ~22-25 MIPS (vs native 85.9/147.6 MIPS, vs c1570/rp2040js ~70M cycles/s) and close all `native ↔ WASM` gaps so the same firmware and tests pass in both.

Explicit user requests in this session:
1. Read whole codebase and explain it
2. List what is left to port
3. `complete all of this` (audit C1-C9, H1-H20, WASM shims)
4. `do all of these` (wire pio/usb/net/vnet/w5500/flush/fault/script/watchdog/gdb, GDB proxy, ETH mesh, devtools 18, threads, tests)
5. `commit and test all` → `05cc2d9`
6. `what next` → `do all` (Pico SDK USB + W5500 live + threads + version bump + benchmarks)
7. Second `do all` (re-verify)
8. Fix `SagePico hello, real W5500, real GDB` → in progress, root-caused to NVIC 26 vs 32 + hello_usb proof
9. `what next` → finish trio → `84a7c39` / `v0.49.0`

---

## 3. What Has Been Done (this session, most recent first)

**Post-v0.50.0 (unreleased):**
- **M33 dual-core boots** (`src/corepool.c`, `src/cpu.c`): threaded WFI never woke (peripherals only step in core quanta; no fast-forward/forced-wake like cooperative) → deterministic 262K-step stall in USB boot. Mirrored both into worker WFI path. `-cores 2` boots to shell, supervisor on Core 1 (Core 0: 2.6B steps, Core 1: 171M).
- **SIO bootrom re-announce** (`src/cpu.c`, `src/membus.c`): SDK reset drains our single sentinel push; supervisor's later VLD check saw empty → single-core fallback. Time-gated (>1ms) re-push while core1 waits. Full 6-word launch handshake unhalts core1 (vtor/SP/PC). `test_sio_bootrom_replenish_launch`.
- **WASM M33 bring-up** (see v0.50.0 follow-ups below): overlay/SRAM/periph init + RP2350 RAM window in `cpu_step_core`.
- **RV trap trace gated** (`src/rp2350_rv/rv_cpu.c`): ebreak loop wrote 6GB in 2 min.
- 378/378 tests. WASM bench (hello_usb, Node 22): 25.4 MIPS JIT-off, 22.1 JIT-on (leave off).

**v0.50.0 follow-ups (`c76e58b`, `5338df9`):**
- **WASM M33 bring-up** (`src/bramble_wasm.c`, `src/cpu.c`): `bramble_init` skipped the M33 overlay (520KB SRAM, RP2350 periph, ROM patch) and `cpu_step_core` hardcoded 264KB RAM — littleOS M33 died instantly with zero output in browser. Fixed; `test-wasm.js` now asserts `root@littleos` shell.
- **Headed Playwright sweep** (`/tmp/sweep_demos.py`, not committed): 28/28 demos PASS, zero console errors (all RP2040 + M33/RV32 littleOS shells + both MicroPython REPLs).
- **New demos** (`test-firmware/{clocks,psm,fp}_test.S`, `build.sh`, `web/` + `web/examples/` + `index.html` buttons): clocks/PSM register readouts, `0.1+0.2` softfloat bit-exact (`0x3FD3333333333334` == host Python).
- **RV shadow bypasses** (`src/rp2350_rv/rv_membus.c`): IO_QSPI/PADS_QSPI/I2C1/PWM/WATCHDOG translated targets alias RP2350 natives claimed first by shared bus; direct RP2040-semantics routing + `test_rv_shadow_bypass`.
- **`-cores` preserved** (`src/cpu.c`): `dual_core_init` reset it to 1; threads start now. M33 `-cores 2` still stalls in early USB boot (threaded WFI-wakeup gap, single-core unaffected).

**v0.50.0 (`a64105d`, tag pushed): littleOS shells + Sage eval**
- **M33 shell**: IT-mask parity, IT flag suppression, SBC borrow, RP2350 ADC `0x400A0000`.
- **RV32 shell**: PSM direct route (RP2040 PSM collides with RP2350 CLOCKS).
- **`-stdin` decoupling**, **VFP single-precision**, **DCP deferred-compute**, **RRX** (`__aeabi_f2d` stale-carry; health `26.9C`/`0.0%`).
- **STMIA.W L-bit is upper[4]** (was bit7: stores ran as loads, Sage lexer froze; `print(6*7)` → `42`).
- **USAT/SSAT** (pico_double `double2fix64_z`; F3AF-hint guard), **SMMULR/SMMLAR/SMMLSR/SMMUL/SMMLA/SMUAD/SMUSD** (double div was 0; `100/10` → `10`, `1/2` → `0.500000`).
- 344 → 377 tests, bench 85.86/147.62 MIPS, CHANGELOG/ROADMAP/README/CI updated.

**v0.49.0 `84a7c39` (superseded):**
- **NVIC user IRQs 26-31** (`include/nvic.h` 26→32, `src/nvic.c` `1u` + `0xFFFFFFFF`): Pico SDK claims soft IRQs top-down for `tud_task`. Was dropping USB worker → hello_usb stall fixed. Test `test_nvic_user_irq_pend_deliver`.
- **Level USB IRQ** (`src/usb.c`): re-assert `USBCTRL_IRQ` while sources pending.
- **WFI fast-forward** (`include/timer.h`, `src/timer.c:timer_next_wakeup_us`, `src/cpu.c`): jump to next alarm (cap 10ms) + SysTick. 1s sleep no longer minutes.
- **hello_usb fixture** (`web/hello_usb.{uf2,elf}`): Pico SDK 2.1.1 built (ARM 13.2), preset + `test-wasm.js` assert `Hello, world!`.
- **311 snapshots lost (agent error)** — restored above.

**v0.48.0 `e903671` (`8f12630` fixup):**
- **Root-caused MicroPython `hard_assert`** (`ep %d %s was already available` at `rp2040_usb.c:115`): Multi-packet IN completion stamped total LEN into `buf_ctrl`; TinyUSB counts per-packet → `remaining_len` underflow → re-arm panic. Fixed by not rewriting LEN (parsers use `in_accum`). Add `test_usb_multipacket_in_keeps_per_packet_len` (325→326).
- Removed stall-guard force-DONE, fixed stdin CR/LF per-target (USB raw CR, UART LF), WASM CDC via `putchar`.
- Verified `print(6*7)` → `42` native + WASM + Chromium.

**v0.47.0 `fe5e136`/`375c9cb`:**
- **WASM polls** in `bramble_step` (pio/usb/net/vnet/w5500/cyw43/flush/fault/script/watchdog/gdb) + **GDB RSP WebSocket** (`src/gdb.c` queues, `web/net_proxy.py` WS↔TCP), **W5500 dest-aware dial** (`CONNECT/LISTEN/CLOSE` + proxy), **ETH BroadcastChannel mesh**, **devtools 18**, **threads** (`bramble_worker.js`, `serve_coop.py`, `-pthread` variant), **tests** (`test-wasm.js` + `.github/workflows/test.yml`), **USB strings + SET_INTERFACE**, **EEPROM SDD** (24LC256, +5 tests → 324), **benchmarks** (see §4).

**Earlier (05cc2d9):** Audit C1-C9/H1-H20 (GPIO INTR order/6 banks/proc1/hi pins, interp sign-ext, subword RMW, IPR `0xC0`, dual RAM invalidate, SysTick wake, shifts, ROM `ldexp`, flush batching, DPRAM guard, GDB checksum, etc.), OPFS fuse, WebSocket net/wire.

**Toolchain installed:** `arm-gnu-toolchain-13.2` in `/home/danish1075/.cache/arm-gnu/`, `arm-none-eabi-gcc`/`gdb`/`addr2line`, `PICO_SDK_PATH=/home/danish1075/.cache/mpbuild/micropython/lib/pico-sdk` (+ 2.x), `CMAKE_POLICY_VERSION_MINIMUM=3.5`.

---

## 4. How to Build and Test

### Native (Linux, Fedora 43, CMake 4.4, gcc)
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure   # 377/377
./build/bramble_bench                        # 85.86 MIPS ICache, 147.62 MIPS JIT
./bramble web/hello_world.uf2                # Hello from Bramble
./bramble web/hello_usb.uf2 -clock 125       # Hello, world! (USB CDC fix)
printf 'print(6*7)\r' | ./bramble web/micropython_rp2040.uf2 -clock 125 -stdin  # 42
```

### WASM (browser)
```bash
./build_wasm.sh          # emcc -O3 -msimd128 → web/bramble.wasm.{js:92K,wasm:236K}
./build_wasm_threads.sh  # -pthread variant → web/bramble.wasm.threads.{js,wasm} (needs COOP/COEP)
python3 -m http.server 8080 --directory web          # http://localhost:8080
python3 web/serve_coop.py 8080                        # for -pthread (SAB)
node test-wasm.js                                    # WASM boots + hello_usb + REPL
python3 -m http.server 18081 --directory web &        # Playwright
# GDB: ./bramble web/hello_usb.uf2 -gdb 3333 &  gdb -ex "target remote :3333" -ex "break main"
# Proxy: python3 web/net_proxy.py --ws 8765 --uart-tcp 9999 --gdb-tcp 3333
```

### CI
`.github/workflows/test.yml`: native ctest+bench, `node test-wasm.js`, Playwright Chromium (hello UART, 0 console errors), `gdb-e2e` (system gdb breaks main).

---

## 5. Known Gaps (honest)

- RV32 `health` shows `Temperature: -410.2C` (M33: `26.9C`). RV Sage floats are bit-exact (literals/div/full formula → `27.1162`), so compute is exonerated; RV UF2 is Mar-21 vintage (M33: Sep-3) and its `adc temp` prints `["` (matches nothing in current source) then ebreak-loops. Needs a fresh RV UF2 (no RISC-V toolchain here).
- RV32 littleOS runs single-core by firmware design (Hart 1 never launched); interactive shell verified anyway.
- `web/micropython_rp2350.uf2` prints an RP2040 banner (possibly mislabeled upstream build); eval works.
- `web/micropython_rp2040.uf2` shipped is v1.22.1 (old); fresh local build is v1.22.1-ish but needs `micropython-lib` submodule dance.

---

## 6. How to Resume (to avoid invalid_request_error)

1. Start **new session** (clears huge turn history).
2. Paste this file as first message + add: `Use one tool call per turn, read before edit, exact oldString.`
3. Pin model if needed via `opencode.json`:
```json
{ "models": { "default": "opencode/muse-spark-1.3-contributor-free" } }
```
4. Next suggested tasks (in order):
    a. Refresh RV UF2 from current littleOS source (needs RISC-V toolchain) and re-verify temp/`adc temp`; close the stale-firmware gap.
    b. Promote `web/micropython_rp2040.uf2` to fresh local build (now proven 270k→ REPL), add `hello_usb.elf` symbols to CI.

---

## 7. What We Are Going to Do Next (Roadmap — agreed with user)

**Done since (v0.49.0 follow-up was written):**
1. **littleOS RP2350 shells** — M33 (IT/SBC/ADC fixes) and RV32 (PSM + shadow bypasses) both reach `root@littleos:/#`, native + browser.
2. **Release `v0.50.0`** — shipped with updated `ROADMAP.md`, `CHANGELOG.md`, `README.md`, CI test count, tag pushed.

**After that (only if you ask):**
- More SDD models (accelerometer LIS3DH, display SSD1306, EEPROM already done)
- vNet DHCP/mDNS, packet capture/replay
- RP2350 HSTX/DVI output via `web/display.ts` canvas
- RV32 temp + M33 dual-core follow-ups from §6a/§6b
```

