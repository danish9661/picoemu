#include <WiFi.h>
#include <pico/cyw43_arch.h>
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("HTTP-START2");
  WiFi.config(IPAddress(192, 168, 4, 205), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.setDNS(IPAddress(192, 168, 4, 1));
  WiFi.begin("BrambleNet", "testpassword");
  delay(3000);
  cyw43_arch_poll();
  WiFiClient c;
  Serial.println("HTTP-DIAL2");
  bool ok = false;
  for (int a = 1; a <= 4 && !ok; a++) {
    if (c.connect(IPAddress(192, 168, 4, 200), 8080)) { ok = true; break; }
    delay(4000);
  }
  if (ok) {
    Serial.println("HTTP-CONN2");
    delay(1500);
    c.print("GET / HTTP/1.0\r\nHost: pico\r\n\r\n");
    unsigned long t0 = millis();
    while (!c.available() && millis() - t0 < 15000) { cyw43_arch_poll(); delay(10); }
    String resp;
    while (c.available()) { resp += (char)c.read(); }
    Serial.print("HTTP-RESP2:");
    Serial.println(resp.substring(0, 120));
    c.stop();
    Serial.println("HTTP-DONE2");
  } else {
    Serial.println("HTTP-CONN2FAIL");
  }
}
void loop() { delay(5000); }
