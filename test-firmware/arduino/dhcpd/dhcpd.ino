#include <WiFi.h>
#include <pico/cyw43_arch.h>
extern "C" void sys_check_timeouts(void);
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("DHCP-START");
  WiFi.begin("BrambleNet", "testpassword");
  for (int i = 0; i < 20; i++) {
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
  Serial.println("DHCP-DONE");
}
void loop() { delay(5000); }
