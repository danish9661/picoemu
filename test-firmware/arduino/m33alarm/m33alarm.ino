// M33 alarm-pool probe: stock SDK alarm_pool + async-context pump.
// The lwIP-Ethernet background pump parks its 20ms poll on an alarm-pool
// alarm serviced via hardware-alarm IRQ. If alarms never fire on M33,
// the pump never runs: handlePackets() never fires, no DISCOVER.
// Prints ALARM-FIRED with advancing millis if the path works.
#include <pico/time.h>

static alarm_pool_t *pool;
static volatile int fired = 0;
static volatile uint32_t fired_ms = 0;

static int64_t alarm_cb(alarm_id_t id, void *user) {
  (void)id; (void)user;
  fired++;
  fired_ms = to_ms_since_boot(get_absolute_time());
  return 0;  // one-shot
}

void setup() {
  Serial1.begin(115200);
  delay(2000);
  Serial1.println("ALARMPROBE-START");
  pool = alarm_pool_create_with_unused_hardware_alarm(4);
  Serial1.print("POOL=");
  Serial1.println(pool ? 1 : 0);
  if (pool) {
    alarm_id_t id = alarm_pool_add_alarm_at(pool,
        make_timeout_time_ms(1500), alarm_cb, NULL, true);
    Serial1.print("ALARM-ID=");
    Serial1.println((int)id);
  }
}

void loop() {
  static int n = 0;
  static unsigned long last = 0;
  unsigned long now = millis();
  if (now - last > 2000) {
    last = now;
    n++;
    Serial1.print("ALARMPROBE-TICK n=");
    Serial1.print(n);
    Serial1.print(" fired=");
    Serial1.print(fired);
    Serial1.print(" at=");
    Serial1.println(fired_ms);
    if (n >= 5) Serial1.println("ALARMPROBE-DONE");
  }
}
