// M33 stock-SDK probe: Serial1 millis + delay + GPIO, no Ethernet.
// Distinguishes "M33 model broken" from "lwIP async-context broken":
// if this prints TICKS with advancing millis, the M33 core/TIMER/UART
// path is fine and the ethdhcp_m33 stall is SDK-async specific.
void setup() {
  Serial1.begin(115200);
  delay(2000);
  Serial1.println("M33PROBE-START");
  Serial1.print("MILLIS0=");
  Serial1.println(millis());
  pinMode(25, OUTPUT);
}

void loop() {
  static int n = 0;
  static unsigned long last = 0;
  unsigned long now = millis();
  if (now - last > 2000) {
    last = now;
    n++;
    digitalWrite(25, n & 1);
    Serial1.print("M33PROBE-TICK n=");
    Serial1.print(n);
    Serial1.print(" millis=");
    Serial1.println(now);
    delay(100);
    if (n >= 5) Serial1.println("M33PROBE-DONE");
  }
}
