#include <WiFi.h>
#include <PubSubClient.h>
#include <pico/cyw43_arch.h>
extern "C" void sys_check_timeouts(void);
WiFiClient net;
PubSubClient mqtt(net);
volatile bool gotMsg = false;
void cb(char* topic, byte* pl, unsigned int n) {
  gotMsg = true;
  Serial.print("MQTT-MSG:");
  for (unsigned int i = 0; i < n; i++) Serial.print((char)pl[i]);
  Serial.println();
}
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("MQTT-START");
  WiFi.config(IPAddress(192, 168, 4, 204), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.setDNS(IPAddress(192, 168, 4, 1));
  WiFi.begin("BrambleNet", "testpassword");
  delay(3000);
  cyw43_arch_poll();
  sys_check_timeouts();
  Serial.print("MQTT-LINK=");
  Serial.println(WiFi.status());
  IPAddress bip;
  Serial.print("MQTT-DNSRC=");
  Serial.println(WiFi.hostByName("test.mosquitto.org", bip));
  Serial.print("MQTT-DNSIP=");
  Serial.println(bip);
  mqtt.setServer("test.mosquitto.org", 1883);
  Serial.println("MQTT-DIAL");
  bool ok = false;
  for (int a = 1; a <= 3 && !ok; a++) {
    Serial.print("MQTT-TRY");
    Serial.println(a);
    if (mqtt.connect("picoemu-test-client")) { ok = true; break; }
    delay(5000);
  }
  if (ok) {
    Serial.println("MQTT-CONN");
    mqtt.setCallback(cb);
    mqtt.subscribe("picoemu/test");
    mqtt.publish("picoemu/test", "hello-mqtt");
    unsigned long t0 = millis();
    while (millis() - t0 < 15000 && !gotMsg) { mqtt.loop(); cyw43_arch_poll(); sys_check_timeouts(); delay(10); }
    Serial.print("MQTT-GOT=");
    Serial.println(gotMsg ? 1 : 0);
    Serial.println("MQTT-DONE");
  } else {
    Serial.print("MQTT-CONNFAIL rc=");
    Serial.println(mqtt.state());
  }
}
void loop() { delay(5000); }
