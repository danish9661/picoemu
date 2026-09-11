#include <WiFi.h>
#include <pico/cyw43_arch.h>
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("PIN-START");
  WiFi.config(IPAddress(192, 168, 4, 200), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.begin("BrambleNet", "testpassword");
  delay(3000);
  Serial.println("PIN-LOOP");
}
void loop() {
  int v = digitalRead(24);
  Serial.print("P24=");
  Serial.println(v);
  cyw43_arch_poll();
  delay(500);
}
