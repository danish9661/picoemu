#include <WiFi.h>
#include <pico/cyw43_arch.h>
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("HTTP-START");
  WiFi.config(IPAddress(192, 168, 4, 203), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.setDNS(IPAddress(192, 168, 4, 1));
  WiFi.begin("BrambleNet", "testpassword");
  delay(3000);
  cyw43_arch_poll();
  IPAddress srv;
  if (!WiFi.hostByName("example.com", srv)) {
    Serial.println("HTTP-NODNS");
    return;
  }
  Serial.print("HTTP-SRV=");
  Serial.println(srv);
  WiFiClient c;
  Serial.println("HTTP-DIAL");
  bool ok = false;
  for (int a = 1; a <= 4 && !ok; a++) {
    if (c.connect(srv, 80)) { ok = true; break; }
    delay(4000);
  }
  if (ok) {
    Serial.println("HTTP-CONN");
    delay(1500);
    c.print("GET / HTTP/1.0\r\nHost: example.com\r\n\r\n");
    unsigned long t0 = millis();
    while (!c.available() && millis() - t0 < 15000) { cyw43_arch_poll(); delay(10); }
    String resp;
    while (c.available()) { resp += (char)c.read(); }
    Serial.print("HTTP-RESP:");
    Serial.println(resp.substring(0, 120));
    c.stop();
    Serial.println("HTTP-DONE");
  } else {
    Serial.println("HTTP-CONNFAIL");
  }
}
void loop() { delay(5000); }
