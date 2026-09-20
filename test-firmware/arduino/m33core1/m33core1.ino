// M33 core1-launch probe: stock SDK multicore_launch_core1 + FIFO.
// If core1 never runs or FIFO never lands, the async-context threadsafe
// background (which parks work on core1 via alarm-pool) can't pump —
// narrowing the ethdhcp_m33 stall to 2nd-core bring-up.
#include <pico/multicore.h>
#include <hardware/gpio.h>

static void core1_main() {
  Serial1.println("CORE1-ALIVE");
  while (true) {
    uint32_t v = multicore_fifo_pop_blocking();
    Serial1.print("CORE1-GOT ");
    Serial1.println(v);
    multicore_fifo_push_blocking(v + 1);
  }
}

void setup() {
  Serial1.begin(115200);
  delay(2000);
  Serial1.println("CORE1PROBE-START");
  multicore_launch_core1(core1_main);
  delay(1000);
  Serial1.println("CORE1-LAUNCHED");
  multicore_fifo_push_blocking(41);
}

void loop() {
  static int n = 0;
  static unsigned long last = 0;
  unsigned long now = millis();
  if (multicore_fifo_rvalid()) {
    uint32_t v = multicore_fifo_pop_blocking();
    Serial1.print("CORE0-GOT ");
    Serial1.println(v);
  }
  if (now - last > 2000) {
    last = now;
    n++;
    Serial1.print("CORE1PROBE-TICK n=");
    Serial1.println(n);
    if (n >= 4) Serial1.println("CORE1PROBE-DONE");
  }
}
