#include <WiFi.h>
#include <WiFiUdp.h>
#include <IPAddress.h>
#include <coap-simple.h>
#include <pico/cyw43_arch.h>
extern "C" void sys_check_timeouts(void);
WiFiUDP udp;
Coap coap(udp);
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("COAPS-START");
  WiFi.config(IPAddress(192, 168, 4, 210), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.begin("BrambleNet", "testpassword");
  delay(3000);
  Serial.print("COAPS-LINK=");
  Serial.println(WiFi.status());
  coap.server([](CoapPacket &pkt, IPAddress ip, int port) {
    Serial.println("COAPS-HIT");
    coap.sendResponse(ip, port, pkt.messageid, "ok-coap");
  }, "test");
  coap.start(5683);
  Serial.println("COAPS-LISTEN");
}
void loop() {
  coap.loop();
  cyw43_arch_poll();
  sys_check_timeouts();
  delay(10);
}
