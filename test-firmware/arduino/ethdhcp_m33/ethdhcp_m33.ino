#include <W5500lwIP.h>
// pico-eth2 DHCP prove-out, M33 flavor (WIZnet W5500-EVB-Pico2, RP2350).
// Same hardware as ethdhcp.ino (SPI0 CS17/RST20/INT21) but prints over
// UART (Serial1 = UART0 -> emulator stdout): USB-CDC Serial is unmodeled
// on M33, so the Serial variant sits in USB-wait and shows no output.
// FQBN: rp2040:rp2040:wiznet_5500_evb_pico2
// Prints ETHDHCP-START, then ETH-BEGIN-OK, then ETH-TICK lines with ip=.
// Emulator run (needs a live peer — Arduino DHCP has no dead-peer marker):
//   ./build/bramble /tmp/ethdhcp_m33/out/ethdhcp_m33.ino.uf2 -board pico-eth2 \
//       -net-peer /tmp/ethdhcp.sock
//   python3 test-firmware/dhcp_peer_test.py /tmp/ethdhcp.sock
// Expect: ALL DHCP CHECKS PASSED + ip=192.168.4.2 on UART.
Wiznet5500lwIP eth(17, SPI, 21);

static unsigned long spin_until(unsigned long ms) {
  // Busy-pump instead of WFI sleep: keeps guest time at wall pace so the
  // async-context lwIP pump + DHCP timers track the wall-ms peer replies.
  unsigned long t0 = millis();
  while (millis() - t0 < ms) { /* spin */ }
  return millis();
}

void setup() {
  Serial1.begin(115200);
  spin_until(2000);
  Serial1.println("ETHDHCP-START");
  bool ok = eth.begin();  // DHCP (default)
  Serial1.println(ok ? "ETH-BEGIN-OK" : "ETH-BEGIN-FAIL");
}

void loop() {
  static unsigned long last = 0;
  static int phase = 0;
  unsigned long now = millis();
  if (now - last > 2000) {
    last = now;
    phase++;
    Serial1.print("ETH-TICK conn=");
    Serial1.print(eth.connected() ? 1 : 0);
    Serial1.print(" ip=");
    Serial1.println(eth.localIP());
    spin_until(1500);
    if (phase >= 10) Serial1.println("ETHDHCP-DONE");
  }
}
