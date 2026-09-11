#include <WiFi.h>
#include <WiFiUdp.h>
#include <IPAddress.h>
#include <coap-simple.h>
#include <pico/cyw43_arch.h>
extern "C" void sys_check_timeouts(void);
WiFiUDP udp;
Coap coap(udp);
volatile bool gotResp = false;
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("COAPC-START");
  WiFi.config(IPAddress(192, 168, 4, 211), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.begin("BrambleNet", "testpassword");
  delay(3000);
  Serial.print("COAPC-LINK=");
  Serial.println(WiFi.status());
  coap.response([](CoapPacket &pkt, IPAddress ip, int port) {
    Serial.print("COAPC-RESP len=");
    Serial.println(pkt.payloadlen);
    gotResp = true;
  });
  coap.start();
  Serial.println("COAPC-DIAL");
  coap.get(IPAddress(192, 168, 4, 210), 5683, "test");
  unsigned long t0 = millis();
  while (millis() - t0 < 20000 && !gotResp) {
    coap.loop();
    cyw43_arch_poll();
    sys_check_timeouts();
    delay(10);
  }
  Serial.print("COAPC-GOT=");
  Serial.println(gotResp ? 1 : 0);
  Serial.println("COAPC-DONE");
}
void loop() { delay(5000); }
