#include <WiFi.h>
#include <pico/cyw43_arch.h>
extern "C" void sys_check_timeouts(void);
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("STAA-START");
  WiFi.beginNoBlock("BrambleAP");
  Serial.println("STAA-BEGINRET");
  for (int i = 0; i < 25; i++) {
    delay(1000);
    cyw43_arch_poll();
    sys_check_timeouts();
    int s = WiFi.status();
    Serial.print("STATUS=");
    Serial.print(s);
    Serial.print(" IP=");
    Serial.println(WiFi.localIP());
    if (s == 3 && WiFi.localIP() != IPAddress(0, 0, 0, 0)) break;
  }
  WiFiClient c;
  Serial.println("STAA-DIAL");
  bool ok = false;
  for (int a = 1; a <= 4 && !ok; a++) {
    Serial.print("STAA-TRY");
    Serial.println(a);
    if (c.connect(IPAddress(192, 168, 4, 1), 8080)) { ok = true; break; }
    delay(4000);
  }
  if (ok) {
    Serial.println("STAA-CONN");
    delay(1500);
    c.print("hello-ap\n");
    String resp = c.readStringUntil('\n');
    Serial.print("STAA-RESP:");
    Serial.println(resp);
    c.stop();
    Serial.println("STAA-DONE");
  } else {
    Serial.println("STAA-CONNFAIL");
  }
}
void loop() { delay(5000); Serial.println("STAA-TICK"); }
