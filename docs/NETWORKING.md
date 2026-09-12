# picoemu networking (WiFi / Ethernet)

How guest networking works in Bramble, what is verified, and what is left.
Status legend: ✅ verified E2E · 🟡 transport-ready, app E2E not run ·
❌ not modeled.

## Architecture

```
guest firmware (Arduino lwIP / pico-sdk CYW43 / lwIP)
  │  CYW43 SDPCM over PIO-SPI (gSPI)          W5500 SPI (other NIC)
  ▼                                            ▼
CYW43 model (src/cyw43.c)                     W5500 model
  │  Ethernet frames                             │
  ▼                                            ▼
+---------------- vnet bus (src/vnet.c) ------------------+
  │                │                  │                    │
TAP device   unix peers           WS mirror            fake DHCP/DNS
(root only)  (-net-peer)          (WASM browser)       (built-in, offline)
  │                │                  │
  │      gateway_bridge.py            │
  │      (unix <-> WS client)         │
  ▼                ▼                  ▼
Go gateway (gVisor NAT/DHCP/DNS, rooms) · internet / room LAN
```

- The emulator never speaks IP itself (except the tiny fake DHCP/DNS
  server). Everything above Ethernet is the guest's stack (lwIP).
- vnet switches raw ETH frames (any ethertype) between ports, TAP,
  peers, and the WS mirror. No IP awareness, no filtering.
- Ours gateway default: `ws://localhost:5090` (upstream uses 5099;
  `GATEWAY_PORT` overrides, rooms via `?sessionId=`).

## Modes

| Mode | Flags | Notes |
|---|---|---|
| Offline STA | `-wifi` | Fake APs (BrambleNet/PicoTestAP/OpenNetwork), fake DHCP .2. No host net needed. |
| APSTA/AP | `-wifi` | Soft-AP via `bsscfg:ssid` + `bss up`; guest DHCP server serves STA. |
| TAP | `-tap br0` (+sudo) | Real host bridging. |
| vnet mesh | `-net -net-peer <sock>` | Rootless instance meshing, no gateway. |
| Gateway | `-nodhcp -net -net-peer <sock>` + `gateway_bridge.py` | Real DHCP/DNS/NAT/room LAN via Go gateway. `-nodhcp` disables the fake server so DHCP flows through. |
| Per-instance MAC | `-mac DE:AD:BE:EF:00:0X` | Required: distinct MACs per room member. |

## Protocol matrix

| Protocol | Status | Evidence / notes |
|---|---|---|
| ARP | ✅ | Request+reply verified direct, via gateway flood, and AP↔STA. |
| DHCP client | ✅ | Fake server (.2); Go gateway (.2, DISCOVER→OFFER→REQUEST→ACK logged both sides — Arduino sketches and MicroPython guest); AP guest server (.16). Needs `sys_check_timeouts()` pumped in sketch. |
| DHCP server (guest) | ✅ | Arduino `DHCPServer` on AP served .16 to STA. |
| IPv4 | ✅ | Carries all below; forwarding is ethertype-agnostic. |
| ICMP (ping) | ✅ | Gateway replies (TTL=64); peer echo replies seen on wire (coexist with tight 5 s default timeout — retry). |
| UDP | ✅ | M33 `UDPSEND=1`; DHCP (UDP/67) both directions; DNS-shaped datagrams flow (see DNS). |
| TCP client/server | ✅ | Handshake + data + reply through 2 emulators + Go gateway; also AP STA↔AP. One flake noted: first data segment occasionally lost under tight timing (retransmit covers it when timers are pumped). |
| HTTP client/server | ✅ | Client: `GET http://example.com/` → `HTTP/1.1 200 OK` via gateway NAT/internet. Server: Arduino-style static handler on :8080, peer GET → `200 OK` (verified emulator↔emulator). |
| DNS | ✅ | `example.com` → 104.20.23.154 and `test.mosquitto.org` → 54.36.178.49 via gateway NAT. Guest needs `setDNS(gw)` (fake DHCP is off with `-nodhcp`) and `sys_check_timeouts()` pumped. |
| MQTT | ✅ | PubSubClient to `test.mosquitto.org:1883`: CONN + SUBSCRIBE + PUBLISH + received own `hello-mqtt` (full broker round-trip). |
| CoAP / CoAP server | ✅ | coap-simple client GET ↔ server (`/test` → `ok-coap`, 7 B payload) over room UDP. Needs `coap.loop()` + `sys_check_timeouts()` pumped both ends. |
| Soft-AP | ✅ | `beginAP`: beacon in scans, STA join, guest DHCP, TCP echo — all verified Pico W↔Pico W. |
| IPv6 | 🟡 L3 + UDP-RX, TX-origination gap | vnet carries it; fake NDP (RS→RA with fd00:4::/64, NS→NA) + periodic unsolicited RA (7s). Gateway: `0x86DD` forwarding + RS→RA/NS→NA/echo + periodic RA ticker (`openhw-studio-gateway/`, room v6 echo verified through a live room). RV32 `ping6` does RS/RA + NS/NA + echo E2E. MP proof (`mp6_peer_test.py` green): LWIP_IPV6=1+DUP_DETECT=0 build SLAACs fd00:4::dcad:beff:feef:cafe, answers Echo (16B, from ULA), receives UDPv6 (addr formatting OK), sends multicast UDP. MP socket API backported for AF_INET6 create/bind/connect/sendto/recvfrom/accept/getaddrinfo-literal (external tree, v4 re-verified). Root cause fixed for timers: MP RP2 never pumped lwIP timeouts (no async path linked) — `MICROPY_INTERNAL_EVENT_HOOK` now runs `sys_check_timeouts` under `lwip_lock` (TCP RTO verified firing). Gaps: originated-unicast-UDP TX emits nothing (ND-entry lifecycle without full timer support; under diagnosis), gateway NAT66/DNS64 (needs gVisor-v6 or userspace NAT66 — recorded in `web/gateway-change.md`). |
| BT / BLE advertising | ✅ bare-metal + room + HCI forward | HCI ring fixed (B2H payload excludes H4 type byte; event `len` covers ncmd+opcode+status/params; BT events off the GPIO wake line — a level storm starved the thread-mode scheduler task). Bare-metal `ble_adv_rv32` does BT bring-up + RESET/ADV_PARAMS/ADV_DATA/ADV_ENABLE/SCAN_ENABLE with CC waits (RESET-OK → ADV-OK → LISTEN). Room: ADV announced over vnet ethertype `0x88B5` + 2s beacons; scanners synthesize LE Advertising Reports — two peered instances both print SCAN-OK (`ble_peer_test.py` locks the wire format). **HCI forwarding** (`-bt-hci <sock>` + `web/hci_bridge.py`, H4 unix↔TCP): guest drives a real Bumble virtual controller (`bumble.apps.controllers tcp-server:_:9544 …:9545`) — bare-metal ADV appears in `bumble.apps.scan` as BRV32, and MicroPython `BLE().active()` completes against it (needs bridge `--short-circuit fc01,0c63,0c6d,2017` + canned LE_Read_Buffer_Size_V2/LE_Encrypt replies for Bumble's minimal controller). Physical adapters via `bumble-hci-bridge tcp-server:_:PORT hci-socket:N` (needs the adapter DOWN + CAP_NET_RAW, i.e. root — verified to EPERM as user). Browser guests reach Bumble through `cli.js --ble-hci ws://host:port/api/ble-gateway` (WASM H4 export + WS pump, `test-wasm-ble.js`). Known limitation: MP `gap_advertise` emits no HCI (guest btstack-GAP-side; bare-metal ADV proves the forwarding path carries ADV fine). |

### BLE backends compared

| Backend | Type | Accuracy | Transport | Needs | Status |
|---|---|---|---|---|---|
| RootCanal (`pip install rootcanal`) | standalone virtual controller (ex-Android CTS) | highest — MP init passes with zero shims | H4/TCP `:6402` (new connection = new controller) | pip package | ✅ default backend |
| Bumble virtual controller (`bumble.apps.controllers`) | minimal test controller + rich apps | partial — needs bridge `--short-circuit fc01,0c63,0c6d,2017` + canned `2060`/encrypt replies | H4/TCP | `pip install bumble` | ✅ apps/peers (scan, GATT), controller needs shims |
| Internal responder (`src/cyw43.c`) | bring-up stub | init/ADV only, no link layer | in-emulator | nothing | ✅ offline default |
| Physical `hci0` via `bumble-hci-bridge` | real silicon (here: Realtek) | exact | `hci-socket:N` | adapter DOWN + root (EPERM verified as user) | 🟡 documented, needs root session |
| BlueZ `btvirt` / VHCI attach | kernel virtual controllers | n/a here | VHCI `/dev/vhci` | not installed + root-only node | 🟡 deferred |
| Zephyr `native_sim` | production controller, H4/TCP `--bt-dev=` | highest (spec-conformant) | bridge-compatible as-is | west + toolchain + build | 🟡 deferred backstop |
| BTstack / NimBLE | host-side stacks | n/a (wrong end for controller role) | — | — | peers only; NimBLE notable for ESP32-C6 parity work |
| Go: `go-ble/ble` (host), `muka/go-bluetooth` (BlueZ D-Bus) | host/peer/control in Go | — | HCI / D-Bus | — | no mature pure-Go virtual controller exists |

## Per-arch WiFi status

| Arch | Scan | STA join | DHCP | TCP/UDP | Notes |
|---|---|---|---|---|---|
| RP2040 M0+ (Pico W) | ✅ | ✅ WPA2 + open | ✅ fake/real/guest | ✅ | Reference path. |
| RP2350 M33 (Pico 2 W) | ✅ | ✅ | ✅ fake | ✅ UDP send; TCP via same models | Needed DMA CTRL remap + SXTAB family. |
| RP2350 RV32 (Hazard3) | ✅ | ✅ WPA2 | ✅ fake (.2) | ✅ TCP server | pico-sdk `wifi_scan` finds 3/3 APs; `rvwifi_join` (lwip_poll) joins BrambleNet + DHCP .2. Bare-metal `webserver_rv32` serves HTTP on :80 (ARP→SYN→HTTP→FIN vs vnet peer, checksums OK). Bare-metal `ble_adv_rv32` advertises + scans via BT shared bus (room-tested). |

## CYW43 model notes (for debuggers)

- PIO-SPI intercept auto-detects the SM by `sideset_base==29`
  (PIO0/1/2 all modeled; Arduino uses PIO1/PICO2).
- SDPCM channels 0/1/2 (CTL/EVENT/DATA), RX queue (32), GPIO24
  HOST_WAKE level IRQ (needs correct per-chip IO_BANK0 maps).
- RP2350 DMA CTRL differs (BSWAP 22→24, IRQ_QUIET, INCR_WRITE,
  CHAIN_TO); decoded by arch.
- Thumb-2 SXTAB/SXTAH/UXTAB/UXTAH implemented (M33 lwIP needs them).
- BDC `flags2` selects STA(0)/AP(1) netif; AP-up duplicates inbound.
- `BRAMBLE_CYW43_TRACE=1`: full-speed gSPI/IOCTL/RX/escan logging.

## What's left

1. ~~App E2E: HTTP client+server, DNS lookup, MQTT pub/sub, CoAP~~ — done
   (see matrix; sketches in `~/gwtest`, need arduino-cli so they stay
   out of `ctest`/sweep).
2. ~~ICMP ping test~~ — done (gateway TTL=64).
3. IPv6 (needs gateway + lwIP6 + SLAAC work; parked).
4. ~~BLE~~ — done (bare-metal ADV + vnet room + HCI forwarding to Bumble/virtual + physical-adapter path, see matrix; MP `gap_advertise` emits no HCI guest-side).
5. ~~MicroPython WiFi~~ — done: Pico W MP firmware joins + DHCP (.2) under `-wifi`. Needed F2-watermark scratch reg (BT builds abort bus_init on readback mismatch) and join events queued at SET_SSID-response pop (ACTIVE race). Verified on pristine local build AND official v1.24.1 release image via USB REPL (`B 3 ('192.168.4.2', ...)`); needed stdin target reorder (USB CDC preferred when viable, UART banner must not steal REPL input; littleOS unaffected). Real-gateway DHCP also proven: `-nodhcp -net -net-peer` + `gateway_bridge.py` → Go gateway DORA, `STAT1: 3`, `.2` (`GOTIP at iter 102`). Needed vnet pre-accept TX backlog (16 frames, flushed on accept) + accept-on-TX: guest WFE fast-forward starves `vnet_poll`, so the DHCP burst was dropped before the bridge attached; `gateway_bridge.py` also survives guest exit (reconnect instead of `TypeError` crash). Exchange is sub-second wall-clock once attached (DORA + ARP in <1s); `cooperative` WFE fast-forward also runs host polls via `bramble_ff_poll_hook` so RX/accept don't starve during sleeps. Known limitation: guest lwIP fine-timers don't retransmit (3-frame initial burst only, verified 60s with no server) — harmless since live servers answer the burst synchronously.
6. ~~WASM gateway E2E against a live Go gateway (plumbing merged,
   headed test pending)~~ — done via `test-wasm-gateway.js`: in-process
   WS gateway (DHCP+ARP) + Pico SDK join sample in WASM gets .2. Needed
   `bramble_wifi_enable` export (`--wifi` in cli.js, `--gateway` implies
   nodhcp) since WASM never called `cyw43_init`.
7. Arduino E2E sketches live in `test-firmware/arduino/` (need
   arduino-cli; stay out of `ctest`/sweep — see its README).
