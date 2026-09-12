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
| IPv6 | 🟡 L3 proven, API pending | vnet carries it (ethertype-agnostic); fake NDP answers RS (RA with fd00:4::/64 SLAAC prefix) + NS for gateway addrs, plus periodic unsolicited RA (7s) for stacks that never send RS. RV32 `ping6` demo does RS/RA + NS/NA + echo E2E. MP proof (`mp6_peer_test.py`): local LWIP_IPV6=1+DUP_DETECT=0 build joins, SLAACs fd00:4::dcad:beff:feef:cafe from the periodic RA, answers link-local NA, and Echo-Replies to its ULA (16B echoed, checksums OK). Root causes found: (1) MP RP2 never pumps async_context (`cyw43_arch_poll` has zero callers) so lwIP timers are dead — no DHCP retries, no RS, no DAD confirm; (2) MP 1.22 modlwip socket API is v4-only (bind/sendto paths; needs upstream dual-stack backport). MP tree patches (external `~/.cache/mpbuild`: lwipopts IPV6=1/DUP_DETECT=0 + `ip_2_ip4`/`IP_ADDR4` in modlwip.c, network_cyw43.c, dhcpserver.c) keep v4 green (fake + gateway DHCP re-verified). Remains: socket API v6 backport, gateway NAT66/DNS64/AAAA + periodic RA (recorded in `web/gateway-change.md`). |
| BT / BLE advertising | ✅ bare-metal + room | HCI ring fixed (B2H payload excludes H4 type byte; BT events off the GPIO wake line — a level storm starved the thread-mode scheduler task). Bare-metal `ble_adv_rv32` does BT bring-up + RESET/ADV_PARAMS/ADV_DATA/ADV_ENABLE/SCAN_ENABLE with CC waits (RESET-OK → ADV-OK → LISTEN). Room: ADV announced over vnet ethertype `0x88B5` + 2s beacons; scanners synthesize LE Advertising Reports — two peered instances both print SCAN-OK (`ble_peer_test.py` locks the wire format). Known limitation: MP-btstack full init stalls (RESET CC delivered + consumed, hci substate never advances — guest-side btstack issue, bytes verified perfect on our ring). |

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
4. ~~BLE~~ — done (bare-metal ADV + vnet room, see matrix; MP-btstack init itself still stalls guest-side).
5. ~~MicroPython WiFi~~ — done: Pico W MP firmware joins + DHCP (.2) under `-wifi`. Needed F2-watermark scratch reg (BT builds abort bus_init on readback mismatch) and join events queued at SET_SSID-response pop (ACTIVE race). Verified on pristine local build AND official v1.24.1 release image via USB REPL (`B 3 ('192.168.4.2', ...)`); needed stdin target reorder (USB CDC preferred when viable, UART banner must not steal REPL input; littleOS unaffected). Real-gateway DHCP also proven: `-nodhcp -net -net-peer` + `gateway_bridge.py` → Go gateway DORA, `STAT1: 3`, `.2` (`GOTIP at iter 102`). Needed vnet pre-accept TX backlog (16 frames, flushed on accept) + accept-on-TX: guest WFE fast-forward starves `vnet_poll`, so the DHCP burst was dropped before the bridge attached; `gateway_bridge.py` also survives guest exit (reconnect instead of `TypeError` crash). Exchange is sub-second wall-clock once attached (DORA + ARP in <1s); `cooperative` WFE fast-forward also runs host polls via `bramble_ff_poll_hook` so RX/accept don't starve during sleeps. Known limitation: guest lwIP fine-timers don't retransmit (3-frame initial burst only, verified 60s with no server) — harmless since live servers answer the burst synchronously.
6. ~~WASM gateway E2E against a live Go gateway (plumbing merged,
   headed test pending)~~ — done via `test-wasm-gateway.js`: in-process
   WS gateway (DHCP+ARP) + Pico SDK join sample in WASM gets .2. Needed
   `bramble_wifi_enable` export (`--wifi` in cli.js, `--gateway` implies
   nodhcp) since WASM never called `cyw43_init`.
7. Arduino E2E sketches live in `test-firmware/arduino/` (need
   arduino-cli; stay out of `ctest`/sweep — see its README).
