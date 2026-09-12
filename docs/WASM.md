# picoemu – WebAssembly Port

Compiled from C (picoemu past v0.50.0) to WASM via Emscripten (`emsdk`) for browser execution. Published to npm as [`picoemu`](https://www.npmjs.com/package/picoemu) with Node CLI (`cli.js`) — see `docs/PICOEMU.md` for the user/API reference.

## Build

```bash
source emsdk/emsdk_env.sh
./build_wasm.sh
# COMMON_FLAGS="-O3 -msimd128 -Wall ..."
# INCLUDES="-Iinclude/ -Iinclude/rp2350_rv -Iinclude/rp2350_arm"
# SOURCES: src/bramble_wasm.c + cpu, thumb32, membus, uf2, elf, gpio, timer, uart, spi, i2c, pwm, adc, dma, pio, nvic, clocks, usb, rtc, rom, gdb, storage, sdcard, emmc, fatfs, w5500, bme280, cyw43, devtools, vnet, sdd, rv_cpu, rv_clint, rv_membus, rv_bootrom, rp2350_periph, picobin, m33_cpu
# Excluded: main.c (CLI), fuse_mount.c, corepool.c, tapif.c, netbridge.c, wire.c (stubs for uart/net/wire)
# Flags: -s WASM=1 ALLOW_MEMORY_GROWTH=1 MODULARIZE=1 EXPORT_NAME=BrambleModule EXPORT_ES6=1 ENVIRONMENT='web,node' INITIAL_MEMORY=64M
```

Outputs `web/bramble.wasm.js` 92K + `web/bramble.wasm.wasm` 236K (`build_wasm.sh`).

## Exports (`src/bramble_wasm.c` + `src/gdb.c` WASM)

`bramble_init(arch)` 0:M0+ 1:RV32 2:M33, `bramble_load_uf2(ptr,len)` via `fopen("/tmp/fw.uf2")` + `load_uf2`, `bramble_load_elf`, `bramble_step(n)` cooperative (respects `bramble_set_cores(1|2)`, drives `pio_step/usb_step/net_bridge_poll/wire_poll/cyw43_tap_poll/vnet_poll/cyw43_bt_beacon_poll/cyw43_ndp_ra_poll/w5500_poll/sdcard_flush/fault_check/script_poll/watchdog/gdb_should_stop`), `bramble_reset` with `picobin_scan` for RV32 entry, `bramble_read_uart`/`_bulk`/`_write_uart` via `putchar` intercept `uart_tx_buf[4096]` (+ net RX drain + USB-CDC route when `usb_cdc_stdio_active`), `bramble_get_gpio`/`_raw`/`_out`/`_oe` (hi pins 32-47 via `gpio_out_hi`), `bramble_mem_read32/write32`, `bramble_is_halted`, `bramble_get_core_state`, `get_flash/sram_ptr`.
Completed controls: `bramble_set_cores/get_cores`, `bramble_set_quantum`, `bramble_set_jit` (`jit_enable`), `bramble_set_debug`, `bramble_set_semihosting`, `bramble_flash_save/load/write` (MEMFS `/flash.bin` + IDBFS `/persist`), `bramble_sdcard_load(ptr,len,spi)` + `bramble_emmc_load` (MEMFS + `spi_attach_device`), `bramble_net_enable(live)` (`vnet_init`+`w5500_init`), `bramble_sdd_add(arg)`, `bramble_eth_push_rx` (->`vnet_tx_frame`), `bramble_w5500_push_rx` (->RX+`RECV`), full devtools (18): `coverage_start/dump`, `trace_start/stop`, `hotspots_start/report`, `profile_start/dump`, `callgraph_start/dump`, `gpiotrace_start/stop`, `irqlat_start/report`, `stackcheck_start/report`, `symbols_load`, `watch_add`, `fault_add`, `script_load`, `expect_start/check`, `heatmap_start/dump`, `set_buslog`, GDB `gdb_enable/is_hit/hit_core/break/poll/start/stop/notify/push_rx/pop_tx/tx_len`, `bramble_net_set_connected/push_rx/pop_rx`, `bramble_wire_set_connected/push_rx`, `bramble_tap_push_rx`, `bramble_ws_send_w5500`, `fuse_mount_start/stop/active` (OPFS/IDBFS), `flash_persist_*`.
Shims (`src/wasm_net.c`): cooperative `corepool_*` (no pthreads, `navigator.hardwareConcurrency`), `net_bridge_*` via WebSocket + serial mirror (`putchar`), `wire_*` via `BroadcastChannel('bramble-wire')` + mirror (UART/GPIO/ETH `eth` binary), `tapif_*` via WebSocket proxy (fake fd 42 + loopback), `bramble_ws_send_w5500` `[0x57,sock,lenLE,payload]` -> proxy.
GDB (`src/gdb.c` `__EMSCRIPTEN__`): TX/RX queues, `gdb_send_raw`->TX queue, `gdb_recv_packet` non-blocking (0=no packet, NAK on bad checksum), `bramble_gdb_poll` single-packet (0=resume,1=step,2=stopped,-1=detach). Proxy `web/net_proxy.py` bridges WS `/gdb` <-> TCP `:3333` (`target remote :3333`).
USB (`src/usb.c`): WASM CDC OUT via `putchar` (serial monitor), IN via `usb_cdc_rx_push` when `usb_cdc_stdio_active`, control-stall auto-DONE after 20k polls (SagePico retry guard), config `255B`.

## Web UI (`web/index.html`)

`import BrambleModule from './bramble.wasm.js'` with `EXPORT_ES6=1` `print/printErr:console.log` avoids red. Features: drag-drop UF2/ELF, three demo dropdowns (RP2040 / M33 / RV32: `hello_world` `gpio_test` `timer_test` `interrupt_test` `name_prompt` `littleos`, `littleos_pico2`, `littleos_pico2_riscv`, all `*_test`/`*_pico2`/`*_rv32` demos) in `web/` and `web/examples/`, serial monitor UART0 (+USB-CDC via `putchar`), GPIO viewer (`get_gpio_raw||get_gpio`), core PC/SP/halted/MIPS + gdb-stop `perf-info`, clock select, cores/JIT/debug wired to `bramble_set_cores/jit/debug`, flash/SD upload to `bramble_flash_write/sdcard_load` + MEMFS/IDBFS, Net/GDB/W5500/ETH WebSocket via `bramble_net_push_rx/eth_push_rx/w5500_push_rx/gdb_push_rx` + `brambleNetSocket`/`brambleGDB` + pump in `frame()`, Wire via `BroadcastChannel` + `brambleWireRx/GpioRx/EthRx`, Devtools panel (Cov/Trace/Hot/Prof/Call/VCD/IRQ/Stack/Heat + Dump+Download via `FS.readFile` Blobs), Threads panel (SAB detect, `serve_coop.py` COOP/COEP, `bramble_worker.js` off-thread stepping, `build_wasm_threads.sh -pthread` variant), Tests panel (`node test-wasm.js`; `PICOEMU_TEST_GATEWAY=1` adds the WASM gateway E2E: in-process WS gateway DHCP+ARP, Pico SDK join sample gets .2), proxy hint `python3 web/net_proxy.py --ws 8765`, `web/.nojekyll` `/.github/workflows/pages.yml` deploy `web/` to `https://danish9661.github.io/picoemu/`.

## Chips Verified (Arduino CLI `rp2040:rp2040@6.0.0`)

- RP2040 M0+ `hello_world` `Hello from Bramble RP2040 Emulator!` `gpio_test` `LED ON/OFF` `timer_test` `Timer Test Complete` `littleos` shell, `hello_usb` TinyUSB CDC, MicroPython REPL banner+eval, `388/388` native tests, 43-firmware native sweep (`test-firmware/sweep_all.sh`) all archs.
- RP2350 RV32 18 demos incl. CLINT timer trap (`interrupt_rv32`) and dual-hart launch (`dualcore_rv32`), `littleos_pico2_riscv` shell + Sage eval `print(6*7)` → `42.0000`.
- RP2350 M33 `littleos_pico2` shell + Sage eval, 13 `*_pico2` demos.
- MicroPython `micropython_rp2040.uf2`/`micropython_rp2350.uf2`: REPL banner + `6*7==42` eval (USB CDC).
- GDB: `$?#7f` NAK + `$T05thread:1` verified via Node (`bramble_gdb_push_rx/poll/pop_tx`); full `target remote :3333` via `web/net_proxy.py` WS `/gdb` <-> TCP.
- Threads: `bramble_worker.js` off-main-thread stepping verified; SAB `build_wasm_threads.sh -pthread` needs `serve_coop.py` COOP/COEP (`crossOriginIsolated` panel).

## Peripherals

All `src/*` compiled: GPIO, UART PL011, SPI PL022, I2C DW_apb_i2c, Timer 64-bit, PWM 8 slices, ADC, DMA 12ch, PIO 2 blocks, NVIC, Clocks, USB, RTC, ROM, SIO, VREG, etc., SD/eMMC SPI, W5500, BME280, CYW43 TAP, VNet, SDD, all verified via `388` tests and firmware UART.

## Credit

Original emulator: [Night-Traders-Dev/Bramble](https://github.com/Night-Traders-Dev/Bramble) MIT. This repo (`danish9661/picoemu`) is a WASM port with browser UI, `build_wasm.sh`, `src/bramble_wasm.c`, `web/` for GitHub Pages + npm (`picoemu`).

## Deploy

`git push main` triggers `pages.yml` `upload-pages-artifact path: ./web`. Local `python3 -m http.server 8080 --directory web`.
