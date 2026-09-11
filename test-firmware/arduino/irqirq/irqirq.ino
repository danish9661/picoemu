#include <WiFi.h>
#include <pico/cyw43_arch.h>
volatile int irqcount = 0;
void onwake() { irqcount++; }
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("IRQ-START");
  WiFi.config(IPAddress(192, 168, 4, 200), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.begin("BrambleNet", "testpassword");
  delay(3000);
  attachInterrupt(24, onwake, RISING);
  Serial.println("IRQ-ARMED");
}
void loop() {
  Serial.print("IRQC=");
  Serial.println(irqcount);
  cyw43_arch_poll();
  delay(2000);
}
