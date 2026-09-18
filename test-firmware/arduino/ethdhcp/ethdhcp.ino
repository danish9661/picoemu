#include <W5500lwIP.h>
// pico-eth DHCP prove-out (WIZnet W5500-EVB-Pico/Pico2, SPI0 CS17/RST20/INT21).
// FQBN pico (M0+): rp2040:rp2040:wiznet_5500_evb_pico
// FQBN pico2 (M33): rp2040:rp2040:wiznet_5500_evb_pico2
// Prints ETH-BEGIN-OK, then ETH-IP=<addr> once DHCP binds (or (IP unset)).
// Emulator run (needs a live peer — Arduino DHCP has no dead-peer marker):
//   ./build/bramble /tmp/ethdhcp/out/ethdhcp.ino.uf2 -board pico-eth \
//       -net-peer /tmp/ethdhcp.sock
//   python3 test-firmware/dhcp_peer_test.py /tmp/ethdhcp.sock
// Expect: ALL DHCP CHECKS PASSED + ETH-IP=192.168.4.2 on UART.
Wiznet5500lwIP eth(17, SPI, 21);

static unsigned long spin_until(unsigned long ms) {
  // Busy-pump instead of WFI sleep: keeps guest time at wall pace so the
  // async-context lwIP pump + DHCP timers track the wall-ms peer replies.
  unsigned long t0 = millis();
  while (millis() - t0 < ms) { /* spin */ }
  return millis();
}

void setup() {
  Serial.begin(115200);
  spin_until(2000);
  Serial.println("ETHDHCP-START");
  bool ok = eth.begin();  // DHCP (default)
  Serial.println(ok ? "ETH-BEGIN-OK" : "ETH-BEGIN-FAIL");
}

void loop() {
  static unsigned long last = 0;
  static int phase = 0;
  unsigned long now = millis();
  if (now - last > 2000) {
    last = now;
    phase++;
    Serial.print("ETH-TICK conn=");
    Serial.print(eth.connected() ? 1 : 0);
    Serial.print(" ip=");
    Serial.println(eth.localIP());
    spin_until(1500);
    if (phase >= 10) Serial.println("ETHDHCP-DONE");
  }
}
