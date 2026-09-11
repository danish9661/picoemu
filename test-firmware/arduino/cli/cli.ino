#include <WiFi.h>
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("CLI-START");
  WiFi.config(IPAddress(192, 168, 4, 201), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.begin("BrambleNet", "testpassword");
  delay(3000);
  Serial.print("CLIIP=");
  Serial.println(WiFi.localIP());
  WiFiClient c;
  Serial.println("CLI-DIAL");
  bool ok = false;
  for (int attempt = 1; attempt <= 5 && !ok; attempt++) {
    Serial.print("CLI-TRY");
    Serial.println(attempt);
    if (c.connect(IPAddress(192, 168, 4, 200), 8080)) { ok = true; break; }
    delay(4000);
  }
  if (ok) {
    Serial.println("CLI-CONN");
    delay(2000);
    c.print("hello-gateway\n");
    String resp = c.readStringUntil('\n');
    Serial.print("CLI-RESP:");
    Serial.println(resp);
    c.stop();
    Serial.println("CLI-DONE");
  } else {
    Serial.println("CLI-CONNFAIL");
  }
}
void loop() { delay(5000); Serial.println("CLI-TICK"); }
