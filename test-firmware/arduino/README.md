# Arduino E2E sketches (Pico W / Pico 2 W WiFi verification)

These need `arduino-cli` + the `rp2040:rp2040` core (they stay out of
`ctest`/sweep, which are fully offline). FQBNs used:

- Pico W (RP2040 M0+): `rp2040:rp2040:rpipicow`
- Pico 2 W (RP2350 M33): `rp2040:rp2040:rpipico2w` (M33 sketches use
  `Serial1`/UART0 because USB-CDC output isn't modeled on M33 yet)

Compile e.g.:

```sh
arduino-cli compile --fqbn rp2040:rp2040:rpipicow \
  --output-dir /tmp/out test-firmware/arduino/srv/srv.ino
```

## Sketches

| Sketch | Board | What it proves |
|---|---|---|
| `srv` / `cli` | Pico W | TCP echo server+client over gateway/vnet (static IP). |
| `m33wifi` | Pico 2 W | Scan n=3 + DHCP join (STATUS=3) on M33. |
| `apap` | Pico W | Soft-AP (`beginAP`, .1): beacon, DHCP server, TCP echo. |
| `staap` / `stajoin` | Pico W | STA join to emulated AP (open; DHCP+TCP / status-only). |
| `dhcpd` | Pico W | DHCP via real gateway (needs `sys_check_timeouts()` pumped). |
| `dnstest` | Pico W | `example.com`/`test.mosquitto.org` resolve via gateway NAT (needs `setDNS`). |
| `httpcli` | Pico W | `GET http://example.com/` → 200 OK (internet). |
| `httpsrv` / `httpsrvcli` | Pico W | Static HTTP server + peer GET → 200 OK. |
| `mqtttest` | Pico W | PubSubClient to `test.mosquitto.org`: CONN+SUB+PUB+own-msg RX. |
| `pingtest` | Pico W | ICMP to gateway (TTL=64) + peer. |
| `coapsrv` / `coapcli` | Pico W | coap-simple GET `/test` → `ok-coap` (needs lib + `IPAddress.h` patch, see below). |
| `pinpin` / `regreg` / `irqirq` | Pico W | Debug: GPIO24 level, IO_BANK0 INTE/INTS, GPIO IRQ delivery. |

Run pattern (native gateway path, room `lab`):

```sh
python3 web/gateway_bridge.py --sock /tmp/gwA.sock --room lab &
python3 web/gateway_bridge.py --sock /tmp/gwB.sock --room lab &
./build/bramble /tmp/out/srv.ino.uf2 -clock 125 -wifi -nodhcp -net \
  -net-peer /tmp/gwA.sock -mac DE:AD:BE:EF:00:01 &
./build/bramble /tmp/out/cli.ino.uf2 -clock 125 -wifi -nodhcp -net \
  -net-peer /tmp/gwB.sock -mac DE:AD:BE:EF:00:02
```

Notes:

- `-nodhcp` disables the fake server so DHCP/DNS use the gateway;
  `-mac` gives each room member a distinct MAC.
- Guest lwIP timers don't run themselves: sketches that need DHCP,
  TCP retransmits, or CoAP retries must pump `sys_check_timeouts()`
  (and `cyw43_arch_poll()`); Arduino `delay()` does neither.
- `coapsrv`/`coapcli` need the `CoAP simple library` with one added
  line (`#include <IPAddress.h>` in `coap-simple.h`).
