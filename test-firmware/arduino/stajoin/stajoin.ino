#include <WiFi.h>
#include <pico/cyw43_arch.h>
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("STAJ-START");
  WiFi.beginNoBlock("BrambleAP");
  for (int i = 0; i < 15; i++) {
    delay(1000);
    cyw43_arch_poll();
    Serial.print("STATUS=");
    Serial.println(WiFi.status());
  }
  Serial.println("STAJ-DONE");
}
void loop() { delay(5000); }
