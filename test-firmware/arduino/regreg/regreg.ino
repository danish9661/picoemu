#include <WiFi.h>
#include <pico/cyw43_arch.h>
static uint32_t rd32(uint32_t a) { return *(volatile uint32_t *)a; }
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("REG-START");
  WiFi.config(IPAddress(192, 168, 4, 200), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.begin("BrambleNet", "testpassword");
  delay(3000);
  Serial.print("INTE3="); Serial.println(rd32(0x40014114), HEX);
  Serial.print("INTS3="); Serial.println(rd32(0x40014144), HEX);
  Serial.print("INTR3="); Serial.println(rd32(0x400140FC), HEX);
  Serial.print("GPIOIN="); Serial.println(rd32(0xD0000004), HEX);
  Serial.print("WLPINS=");
  for (int i = 0; i < 4; i++) { Serial.print(rd32(0x20000F30 + i * 4), HEX); Serial.print(" "); }
  Serial.println();
  Serial.println("REG-DONE");
}
void loop() { delay(5000); }
