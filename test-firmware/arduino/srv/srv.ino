#include <WiFi.h>
#include <pico/cyw43_arch.h>
WiFiServer server(8080);
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("SRV-START");
  WiFi.config(IPAddress(192, 168, 4, 200), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.begin("BrambleNet", "testpassword");
  delay(3000);
  Serial.print("SRVIP=");
  Serial.println(WiFi.localIP());
  server.begin();
  Serial.println("SRV-LISTEN");
}
void loop() {
  static int n = 0;
  cyw43_arch_poll();
  if ((n++ % 100) == 0) { Serial.print("SRV-STATUS="); Serial.println(WiFi.status()); }
  WiFiClient c = server.available();
  if (c) {
    Serial.println("SRV-ACCEPT");
    unsigned long t0 = millis();
    while (!c.available() && millis() - t0 < 8000) { cyw43_arch_poll(); delay(10); }
    String line = c.readStringUntil('\n');
    Serial.print("SRV-GOT:");
    Serial.println(line);
    c.print("ECHO:" + line + "\n");
    delay(200);
    c.stop();
    Serial.println("SRV-DONE");
  }
  delay(50);
}
