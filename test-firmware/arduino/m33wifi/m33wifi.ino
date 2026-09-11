#include <WiFi.h>
// Pico 2 W (M33) WiFi verify over UART (Serial1 = UART0 -> emulator stdout)
static void p(const char *s) { Serial1.print(s); }
void setup() {
  Serial1.begin(115200);
  delay(2000);
  p("M33-WIFI-START\n");
  int n = WiFi.scanNetworks();
  Serial1.print("SCAN n=");
  Serial1.println(n);
  for (int i = 0; i < n; i++) {
    Serial1.print(i); Serial1.print(":");
    Serial1.print(WiFi.SSID(i));
    Serial1.print(" ch="); Serial1.print(WiFi.channel(i));
    Serial1.print(" rssi="); Serial1.println(WiFi.RSSI(i));
  }
  p("JOIN...\n");
  int st = WiFi.begin("BrambleNet", "testpassword");
  (void)st;
  delay(3000);
  Serial1.print("STATUS=");
  Serial1.println(WiFi.status());
  Serial1.print("IP=");
  Serial1.println(WiFi.localIP());
  p("M33-WIFI-DONE\n");
}
void loop() { delay(5000); p("TICK\n"); }
