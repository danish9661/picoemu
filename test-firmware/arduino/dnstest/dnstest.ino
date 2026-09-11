#include <WiFi.h>
#include <pico/cyw43_arch.h>
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("DNS-START");
  WiFi.config(IPAddress(192, 168, 4, 202), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.setDNS(IPAddress(192, 168, 4, 1));
  WiFi.begin("BrambleNet", "testpassword");
  delay(3000);
  cyw43_arch_poll();
  Serial.print("DNS-LINK=");
  Serial.println(WiFi.status());
  IPAddress ip;
  int r = WiFi.hostByName("example.com", ip);
  Serial.print("DNS-RC=");
  Serial.println(r);
  Serial.print("DNS-IP=");
  Serial.println(ip);
  Serial.println("DNS-DONE");
}
void loop() { delay(5000); }
