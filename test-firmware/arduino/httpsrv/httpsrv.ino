#include <WiFi.h>
#include <pico/cyw43_arch.h>
WiFiServer server(8080);
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("HTTPSRV-START");
  WiFi.config(IPAddress(192, 168, 4, 200), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.begin("BrambleNet", "testpassword");
  delay(3000);
  server.begin();
  Serial.println("HTTPSRV-LISTEN");
}
void loop() {
  cyw43_arch_poll();
  WiFiClient c = server.available();
  if (c) {
    Serial.println("HTTPSRV-ACCEPT");
    unsigned long t0 = millis();
    while (!c.available() && millis() - t0 < 8000) { cyw43_arch_poll(); delay(10); }
    String req;
    while (c.available()) { char ch = (char)c.read(); req += ch; if (req.length() > 200 || ch == '\n') break; }
    Serial.print("HTTPSRV-REQ:");
    Serial.println(req.substring(0, 60));
    const char *body = "hello-pico";
    c.print("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 10\r\nConnection: close\r\n\r\n");
    c.print(body);
    delay(300);
    c.stop();
    Serial.println("HTTPSRV-DONE");
    return;
  }
  delay(50);
}
