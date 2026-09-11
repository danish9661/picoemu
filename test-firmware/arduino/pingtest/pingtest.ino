#include <WiFi.h>
#include <pico/cyw43_arch.h>
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("PING-START");
  WiFi.config(IPAddress(192, 168, 4, 201), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.begin("BrambleNet", "testpassword");
  delay(3000);
  cyw43_arch_poll();
  int t1 = WiFi.ping(IPAddress(192, 168, 4, 1));
  Serial.print("PING-GW=");
  Serial.println(t1);
  int t2 = WiFi.ping(IPAddress(192, 168, 4, 200));
  Serial.print("PING-PEER=");
  Serial.println(t2);
  int t3 = WiFi.ping(IPAddress(192, 168, 4, 200));
  Serial.print("PING-PEER2=");
  Serial.println(t3);
  Serial.println("PING-DONE");
}
void loop() { delay(5000); }
