#!/usr/bin/env bash
# sweep_all.sh — native firmware sweep: every demo UF2 must print its marker.
# Usage: ./test-firmware/sweep_all.sh [build-dir]  (defaults to ./build)
# Interactive demos (uart_echo/name_prompt) get piped stdin.
set -u
BUILD="${1:-build}"
BIN="$BUILD/bramble"
WEB="$(dirname "$0")/../web"
pass=0; fail=0; failed=""
run() { # file marker steps [stdin-text]
  local f="$1" marker="$2" steps="$3" input="${4:-}"
  local out
  if [ -n "$input" ]; then
    out=$(printf '%b' "$input" | timeout 120 "$BIN" "$WEB/$f" -clock 125 -stdin -timeout 110 -max-steps "$steps" 2>&1 | tr -d '\0')
  else
    out=$(timeout 120 "$BIN" "$WEB/$f" -clock 125 -timeout 110 -max-steps "$steps" 2>&1 | tr -d '\0')
  fi
  if echo "$out" | grep -qF "$marker"; then pass=$((pass+1)); # echo "PASS $f"
  else fail=$((fail+1)); failed="$failed $f"; echo "FAIL $f (want: $marker)"; fi
}
run_wifi() { # file marker steps — CYW43 model enabled (M33/RV32 WiFi tests)
  local f="$1" marker="$2" steps="$3"
  local out
  out=$(timeout 120 "$BIN" "$WEB/$f" -clock 125 -wifi -timeout 110 -max-steps "$steps" 2>&1 | tr -d '\0')
  if echo "$out" | grep -qF "$marker"; then pass=$((pass+1)); # echo "PASS $f"
  else fail=$((fail+1)); failed="$failed $f"; echo "FAIL $f (want: $marker)"; fi
}
# RP2040 (M0+)
run hello_world.uf2 "Hello" 2000000
run gpio_test.uf2 "LED ON" 2000000
run timer_test.uf2 "Timer Test Complete!" 5000000
run interrupt_test.uf2 "Interrupt Test Complete!" 5000000
run spi_test.uf2 "SPI Test Complete!" 2000000
run i2c_test.uf2 "I2C Test Comple" 2000000
run pwm_test.uf2 "PWM Test Complete" 2000000
run adc_test.uf2 "ADC Test Comple" 2000000
run dma_test.uf2 "DMA Test Compl" 2000000
run pio_test.uf2 "PIO Test Complete!" 2000000
run usb_test.uf2 "USB Test Complete!" 2000000
run clocks_test.uf2 "Clocks Test Complete!" 2000000
run psm_test.uf2 "PSM Test Complete!" 2000000
run fp_test.uf2 "FP Test Complete!" 5000000
run ws2812_test.uf2 "WS2812 Test Complete!" 5000000
run rtc_test.uf2 "RTC Test Complete!" 2000000
run uart_echo.uf2 "UART Echo Test Complete!" 5000000 'hello\n'
run name_prompt.uf2 "Hello, Ada!" 5000000 'Ada\n'
# RP2350 M33
run hello_world_pico2.uf2 "Hello" 2000000
run gpio_test_pico2.uf2 "LED ON" 2000000
run timer_test_pico2.uf2 "Timer Test Complete!" 5000000
run spi_test_pico2.uf2 "SPI Test Complete!" 5000000
run clocks_test_pico2.uf2 "Clocks Test Complete!" 5000000
run psm_test_pico2.uf2 "PSM Test Complete!" 5000000
run fp_test_pico2.uf2 "FP Test Complete!" 10000000
run ws2812_test_pico2.uf2 "WS2812 Test Complete!" 10000000
run rtc_test_pico2.uf2 "RTC Test Complete!" 5000000
run uart_echo_pico2.uf2 "UART Echo Test Complete!" 5000000 'hello\n'
run interrupt_test_pico2.uf2 "Timer Interrupt Test Complete!" 5000000
run name_prompt_pico2.uf2 "Hello, Ada!" 5000000 'Ada\n'
# RP2350 RV32
run hello_rv32.uf2 "Hello from Bramble RV32" 2000000
run gpio_rv32.uf2 "GPIO Test Complete!" 2000000
run timer_rv32.uf2 "Timer Test Complete!" 5000000
run interrupt_rv32.uf2 "Timer Interrupt Test Complete!" 60000000
run dualcore_rv32.uf2 "Dual-Core Test Complete!" 30000000
run spi_rv32.uf2 "SPI Test Complete!" 2000000
run i2c_rv32.uf2 "I2C Test Comple" 2000000
run pwm_rv32.uf2 "PWM Test Complete" 2000000
run adc_rv32.uf2 "ADC Test Comple" 2000000
run dma_rv32.uf2 "DMA Test Compl" 2000000
run pio_rv32.uf2 "PIO Test Complete!" 2000000
run usb_rv32.uf2 "USB Test Complete!" 2000000
run clocks_rv32.uf2 "Clocks Test Complete!" 2000000
run psm_rv32.uf2 "PSM Test Complete!" 2000000
run ws2812_rv32.uf2 "WS2812 Test Complete!" 10000000
run rtc_rv32.uf2 "RTC Test Complete!" 5000000
run uart_echo_rv32.uf2 "UART Echo Test Complete!" 10000000 'hello\n'
run name_prompt_rv32.uf2 "Hello, Ada!" 10000000 'Ada\n'
run_wifi wifi_rv32.uf2 "CYW43 TEST PATTERN OK" 5000000
run_wifi wifi_webserver_rv32.uf2 "RV32 WEBSERVER LISTEN" 8000000
echo "== sweep: $pass passed, $fail failed =="
[ -n "$failed" ] && echo "failed:$failed"
exit $fail
