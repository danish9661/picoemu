#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

source "$SCRIPT_DIR/../emsdk/emsdk_env.sh" 2>/dev/null || {
  export EMSDK="$SCRIPT_DIR/../emsdk"
  export PATH="$EMSDK:$EMSDK/upstream/emscripten:$PATH"
}

echo "=== Bramble WASM Build ==="
echo "Emscripten: $(emcc --version | head -1)"

mkdir -p web

COMMON_FLAGS="-O3 -msimd128 -Wall -Wno-macro-redefined -Wno-logical-not-parentheses -Wno-format"
INCLUDES="-Iinclude/ -Iinclude/rp2350_rv -Iinclude/rp2350_arm"

SOURCES=(
  src/bramble_wasm.c src/fuse_mount_wasm.c src/wasm_net.c
  src/cpu.c src/instructions.c src/thumb32.c src/membus.c
  src/uf2.c src/elf.c src/gpio.c src/timer.c src/uart.c
  src/spi.c src/i2c.c src/pwm.c src/adc.c src/dma.c
  src/pio.c src/nvic.c src/clocks.c src/usb.c src/rtc.c
  src/rom.c src/gdb.c src/storage.c src/sdcard.c src/emmc.c
  src/fatfs.c src/w5500.c src/bme280.c src/cyw43.c
  src/devtools.c src/vnet.c src/sdd.c src/sdd_thermo.c src/sdd_eeprom.c
  src/rp2350_rv/rv_cpu.c src/rp2350_rv/rv_clint.c
  src/rp2350_rv/rv_membus.c src/rp2350_rv/rv_bootrom.c
  src/rp2350_rv/rp2350_periph.c src/rp2350_rv/picobin.c
  src/rp2350_arm/m33_cpu.c
)

EXPORTS='[
  "_bramble_init","_bramble_reset",
  "_bramble_load_uf2","_bramble_load_elf",
  "_bramble_step","_bramble_set_clock",
  "_bramble_read_uart","_bramble_read_uart_bulk","_bramble_write_uart",
  "_bramble_get_gpio","_bramble_get_gpio_raw","_bramble_get_gpio_out","_bramble_get_gpio_oe","_bramble_set_gpio",
  "_bramble_mem_read32","_bramble_mem_write32",
  "_bramble_is_halted","_bramble_get_core_state",
  "_bramble_usb_state32",
  "_bramble_get_flash_ptr","_bramble_get_sram_ptr",
  "_bramble_set_cores","_bramble_get_cores","_bramble_set_quantum",
  "_bramble_set_jit","_bramble_set_debug","_bramble_set_semihosting",
  "_bramble_flash_save","_bramble_flash_load","_bramble_flash_write",
  "_bramble_sdcard_load","_bramble_emmc_load",
  "_bramble_net_enable","_bramble_sdd_add",  "_bramble_eth_push_rx",
  "_bramble_eth_pop_tx","_bramble_eth_set_uplink","_bramble_wifi_enable",
  "_bramble_bt_hci_enable","_bramble_bt_hci_pop_tx","_bramble_bt_hci_push_rx",
  "_bramble_w5500_push_rx","_bramble_w5500_push_status","_bramble_ws_send_w5500",
  "_bramble_coverage_start","_bramble_coverage_dump",
  "_bramble_trace_start","_bramble_trace_stop",
  "_bramble_hotspots_start","_bramble_hotspots_report",
  "_bramble_profile_start","_bramble_profile_dump",
  "_bramble_callgraph_start","_bramble_callgraph_dump",
  "_bramble_gpiotrace_start","_bramble_gpiotrace_stop",
  "_bramble_irqlat_start","_bramble_irqlat_report",
  "_bramble_stackcheck_start","_bramble_stackcheck_report",
  "_bramble_symbols_load","_bramble_watch_add","_bramble_fault_add",
  "_bramble_script_load","_bramble_expect_start","_bramble_expect_check",
  "_bramble_heatmap_start","_bramble_heatmap_dump","_bramble_set_buslog",
  "_bramble_gdb_enable","_bramble_gdb_is_hit","_bramble_gdb_hit_core",
  "_bramble_gdb_break","_bramble_gdb_poll",
  "_bramble_gdb_push_rx","_bramble_gdb_pop_tx","_bramble_gdb_tx_len",
  "_bramble_gdb_start","_bramble_gdb_stop","_bramble_gdb_notify_stop",
  "_bramble_net_set_connected","_bramble_net_push_rx","_bramble_net_pop_rx",
  "_bramble_wire_set_connected","_bramble_wire_push_rx",
  "_bramble_tap_push_rx",
  "_bramble_get_gpio_out","_bramble_get_gpio_oe",
  "_bramble_w5500_dev_push_rx",
  "_fuse_mount_start","_fuse_mount_stop","_fuse_mount_active",
  "_flash_persist_set_path","_flash_persist_open","_flash_persist_save_all",
  "_free","_malloc"
]'

echo "Compiling ${#SOURCES[@]} source files..."

emcc \
  "${SOURCES[@]}" \
  $INCLUDES $COMMON_FLAGS \
  -s WASM=1 \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s MODULARIZE=1 \
  -s EXPORT_NAME="BrambleModule" \
  -s EXPORTED_FUNCTIONS="$EXPORTS" \
  -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","getValue","setValue","HEAPU8","HEAPU32","HEAP8"]' \
  -s INITIAL_MEMORY=67108864 \
  -s MAXIMUM_MEMORY=268435456 \
  -s STACK_SIZE=1048576 \
  -s NO_EXIT_RUNTIME=1 \
  -s ENVIRONMENT='web,node' \
  -s EXPORT_ES6=1 \
  -o web/bramble.wasm.js

echo ""
echo "Build complete!"
echo "  WASM: $(du -h web/bramble.wasm.wasm | cut -f1)"
echo "  JS:   $(du -h web/bramble.wasm.js | cut -f1)"
echo ""
echo "To test: python3 -m http.server 8080 --directory web"
echo "  Then open http://localhost:8080"