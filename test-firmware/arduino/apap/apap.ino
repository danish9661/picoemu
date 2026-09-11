#include <WiFi.h>
#include <pico/cyw43_arch.h>
WiFiServer server(8080);
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("AP-START");
  WiFi.beginAP("BrambleAP");
  delay(3000);
  Serial.print("APIP=");
  Serial.println(WiFi.softAPIP());
  Serial.print("APSTATUS=");
  Serial.println(WiFi.status());
  server.begin();
  Serial.println("AP-LISTEN");
}
void loop() {
  cyw43_arch_poll();
  WiFiClient c = server.available();
  if (c) {
    Serial.println("AP-ACCEPT");
    unsigned long t0 = millis();
    while (!c.available() && millis() - t0 < 8000) { cyw43_arch_poll(); delay(10); }
    String line = c.readStringUntil('\n');
    Serial.print("AP-GOT:");
    Serial.println(line);
    c.print("ECHO:" + line + "\n");
    delay(200);
    c.stop();
    Serial.println("AP-DONE");
    return;
  }
  delay(50);
}
