# picoemu networking (WiFi / Ethernet)

How guest networking works in Bramble, what is verified, and what is left.
Status legend: ✅ verified E2E · 🟡 transport-ready, app E2E not run ·
❌ not modeled.

## Architecture: ONE gateway for WiFi + Ethernet

```
guest firmware (Arduino lwIP / pico-sdk CYW43 / lwIP / ioLibrary)
  │  CYW43 SDPCM over PIO-SPI (gSPI)     W5500 SPI (MACRAW sock 0)
  ▼                                       ▼
CYW43 model (src/cyw43.c)                W5500 model (src/w5500.c)
  │  Ethernet frames                        │  Ethernet frames
  ▼                                       ▼
+--------------- vnet bus (src/vnet.c): ONE shared bus ----------------+
  │                │                  │                    │            │
TAP device   unix peers           WS mirror            fake DHCP/DNS  W5500
(root only)  (-net-peer)          (WASM browser)       (built-in,     MACRAW
  │                │              incl. gateway         offline)      port
  │      gateway_bridge.py  uplink (Go gateway)                         │
  │      (unix <-> WS client)                                           │
  ▼                ▼                  ▼                                 │
Go gateway (gVisor NAT/DHCP/DNS, rooms) · internet / room LAN ◄────────┘
(single gateway: same room, same DHCP, same NAT for WiFi + eth)
```

- The emulator never speaks IP itself (except the tiny fake DHCP/DNS
  server). Everything above Ethernet is the guest's stack (lwIP on
  WiFi, lwIP *or* WIZnet ioLibrary on Ethernet).
- vnet switches raw ETH frames (any ethertype) between ports, TAP,
  peers, and the WS mirror. No IP awareness, no filtering. The W5500
  MACRAW socket is just another vnet port — that is the whole
  unification: **no second bridge, no per-socket host NAT for Ethernet**.
- Ours gateway default: `ws://localhost:5090` (upstream uses 5099;
  `GATEWAY_PORT` overrides, rooms via `?sessionId=`).
- The old per-socket "live" path (`-net-live` host TCP/UDP dial,
  `net_proxy.py /w5500` socket bridge) still exists for offload-mode
  sockets 1–7 and for WASM-behind-proxy use, but it is NOT the gateway:
  it bypasses DHCP/rooms and diverges from WiFi. Prefer MACRAW.

## Modes

| Mode | Flags | Notes |
|---|---|---|
| Offline STA | `-wifi` | Fake APs (BrambleNet/PicoTestAP/OpenNetwork), fake DHCP .2. No host net needed. |
| APSTA/AP | `-wifi` | Soft-AP via `bsscfg:ssid` + `bss up`; guest DHCP server serves STA. |
| TAP | `-tap br0` (+sudo) | Real host bridging. |
| vnet mesh | `-net -net-peer <sock>` | Rootless instance meshing, no gateway. |
| Gateway (WiFi) | `-wifi -nodhcp -net -net-peer <sock>` + `gateway_bridge.py` | Real DHCP/DNS/NAT/room LAN via Go gateway. `-nodhcp` disables the fake server so DHCP flows through. |
| Gateway (Ethernet) | `-board pico-eth -net -net-peer <sock>` + `gateway_bridge.py` | SAME gateway/room/DHCP as WiFi via MACRAW socket 0. No `-wifi`, no `-nodhcp` needed (W5500 has no fake server). |
| Gateway (WiFi+eth) | WiFi flags + `-board pico-eth -net -net-peer <sock>`, same `--room` | One room serves both NICs; distinct MACs per interface (use `-mac` for WiFi, SHAR for eth). |
| Per-instance MAC | `-mac DE:AD:BE:EF:00:0X` | Required: distinct MACs per room member. |
| Isolate eth (debug) | `-no-eth-gw` | Keep MACRAW off the shared bus. |

## Ethernet (W5500 MACRAW) support matrix

Guest firmware picks the mode per socket: socket 0 in `MR_MACRAW`
joins the shared vnet bus (gateway path); sockets 1–7 in TCP/UDP keep
the classic offload behavior (host-stack or proxy sockets).

| Feature | Status | Evidence / notes |
|---|---|---|
| MACRAW OPEN (sock 0) | ✅ | `Sn_MR=0x04` + `OPEN` → `SR=0x42 MACRAW`, vnet port registered with SHAR MAC (`test_w5500_macraw_gateway_dhcp_path`). |
| MACRAW SEND → vnet/gateway | ✅ | TX buffer + `SEND` emits the raw frame; observed byte-identical on a vnet peer port; live-tested DHCP DISCOVER → Go gateway OFFER (`192.168.4.2`). |
| MACRAW RX (gateway → guest) | ✅ | Unicast-to-SHAR + broadcast land length-prefixed (`len_hi,len_lo,frame…`, real-hardware layout) with `RECV` set; `RSR=len+2`. Gateway OFFER + ARP reply both land. |
| MACRAW RECV consume | ✅ | `RECV` slides one `[len+frame]` entry, recomputes `RSR`, clears `RECV` only when empty (level semantics → INTn follows). |
| DHCP via gateway | ✅ | DISCOVER→OFFER verified E2E (WASM MACRAW → WS uplink → Go gateway → OFFER → MACRAW RX, `RSR=344`). Same lease pool as WiFi (`.2`). |
| ARP via gateway | ✅ | `who-has 192.168.4.1` → gateway ARP reply (`5a:94:ef:e4:0c:dd`), lands in MACRAW RX. |
| WiFi+eth coexistence | ✅ | CYW43 STA port + MACRAW port on one shared room: eth DHCP OFFER arrives with WiFi attached and pumping. |
| TCP/UDP offload (sock 1–7) | ✅ (unchanged) | Classic path: native host sockets (`-net-live`), WASM proxy pump (`net_proxy.py /w5500`). No gateway DHCP/rooms on this path by design. |
| WASM browser path | ✅ | MACRAW SEND → `vnet_ws_mirror` → `bramble_eth_pop_tx` → gateway WS; gateway → `bramble_eth_push_rx` → vnet → MACRAW RX. `Ethernet via gateway` checkbox (default on) + `bramble_w5500_gw_enable()`. |
| Node path | ✅ | Same WS uplink via `cli.js --gateway`; MACRAW frames flow without `--board-live` (no proxy sockets needed). |
| RP2350 (M33/RV32) | ✅ | RP2350 SPI bases route to the same instances (`spi_match` RP2350-aware); `pico-eth2` alias; VERSIONR-via-`0x40080000` test green. |
| INTn on RECV | ✅ | Socket IR → `w5500_board_refresh_int()` → GPIO21 active-low; W1C clear deasserts. |
| In-tree DHCP guest (M0+/M33/RV32) | ✅ | `test-firmware/gen_eth_dhcp.py` → `eth_dhcp.S`/`eth_dhcp_rv32.S` → `web/eth_dhcp{,_pico2,_rv32}.uf2`; full DORA (`DISCOVER→OFFER→REQUEST→ACK`, `ETH DONE`) green on all three via `test-firmware/dhcp_peer_test.py` (per-arch MAC/XID, `.2/.1` pool). |
| In-tree HTTP guest (M0+/M33/RV32) | ✅ | `test-firmware/gen_eth_http.py` → `eth_http.S`/`eth_http_rv32.S` → `web/eth_http{,_pico2,_rv32}.uf2`; DORA + ARP→SYN→ACK→GET→200 `hello-eth`→ACK→FIN→`ETH HTTP-DONE` green on all three via `test-firmware/http_peer_test.py` (static TX blobs, per-arch MAC/sport/cseq, server SSEQ `0x00100000`). |
| Arduino ioLibrary guest (M0+ ✅, M33 ✅) | ✅ both arches | `test-firmware/arduino/ethdhcp/ethdhcp.ino` (`Wiznet5500lwIP`, `rp2040:rp2040:wiznet_5500_evb_pico`): full DORA green since the RX cursor-latch fix (first DATA byte of each CS frame latched the stale base 0, corrupting nonzero-address RX bursts: OFFER head at RX_RD=0 worked, ACK head at RX_RD=0x0158 read hi=0x00 not 0x01, len 342→86, ACK desynced — latch now happens BEFORE the read; peer `ALL DHCP CHECKS PASSED` + guest `conn=1 ip=192.168.4.2`). **M33 green since 2026-09-22** (`test-firmware/arduino/ethdhcp_m33/ethdhcp_m33.ino`, `Serial1`/UART0 because USB-CDC is unmodeled on M33, `rp2040:rp2040:wiznet_5500_evb_pico2`, `-board pico-eth2 -arch m33`): full DORA — peer `ALL DHCP CHECKS PASSED` (DISCOVER `chaddr=020123520001` → REQUEST same xid) + guest `conn=1 ip=192.168.4.2`. Root-caused two stacked RP2350-map-vs-RP2040-map mismatches (`6b698f7`): (1) RP2350 `IO_BANK0` base `0x40028000` unrouted (writes fell into the `PLL_SYS` clocks stub, GPIO21 never armed); (2) RP2350 IRQ map (IO_IRQ_BANK0 13→21 etc.) — NVIC widened to 64 IRQs with `nvic_rp2350_irq()` translation; the ISR had pended at wrong vector 29 (no handler → bkpt) so the OFFER was never polled. |
| ARM BLE guest (M0+/M33) | ✅ | `test-firmware/gen_ble_arm.py` → `ble_adv.S` → `web/ble_adv{,_pico2}.uf2` (PIO0 SM0): BT bring-up + ADV + scan, prints `BT-CTRL 01000100` / `RAM-BASE 001C0000` / `HOST-READY` / `RESET-OK` / `ADV-OK` / `LISTEN` on both cores, sweep-locked via `run_ble`. Fixed three stacked guest bugs: broken `ba_bswap` middle bytes, missing dummy swap rounds, `.word reset_handler + 1` double Thumb bit. |
| M0+/M33 Arduino WiFi in sweep | ✅ | `wifi_scan/ping/webserver` M0+/M33 UF2s are Arduino-CLI builds (`~/gwtest`, committed at `7ab5b9c`); sweep covers WiFi on RV32 only. M33 `m33wifi.ino` repro (`test-firmware/arduino/m33wifi/`): **green since 2026-09-19** — prints `SCAN n=3`, `STATUS=3`, `IP=192.168.4.2` under `-arch m33 -wifi` (the `n=0` row was stale; same HOST_WAKE level fix that unblocked RV32 join unblocked the escan IOCTL response path). |
| What is NOT done | 🟡 | MP live `gap_advertise`: stock Pico-W MP boots to REPL but `import bluetooth` hangs the guest (BT stack never reaches `BLE-ACTIVE`; needs deeper HCI bring-up work — see CHANGELOG). |

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
| IPv6 | ✅ L3 + UDP-RX/TX (unicast E2E) | vnet carries it; fake NDP (RS→RA with fd00:4::/64, NS→NA) + periodic unsolicited RA (7s). Gateway: `0x86DD` forwarding + RS→RA/NS→NA/echo + periodic RA ticker (`openhw-studio-gateway/`, room v6 echo verified through a live room). RV32 `ping6` does RS/RA + NS/NA + echo E2E. MP proof (`mp6_peer_test.py` green): LWIP_IPV6=1+DUP_DETECT=0 build SLAACs fd00:4::dcad:beff:feef:cafe, answers Echo (16B, from ULA), receives UDPv6 (addr formatting OK), sends multicast UDP. MP socket API backported for AF_INET6 create/bind/connect/sendto/recvfrom/accept/getaddrinfo-literal (external tree, v4 re-verified). Root cause fixed for timers: MP RP2 never pumped lwIP timeouts (no async path linked) — `MICROPY_INTERNAL_EVENT_HOOK` now runs `sys_check_timeouts` under `lwip_lock` (TCP RTO verified firing). Fixed originated-unicast-UDP TX E2E: MP `SEND6` (`sendto fd00:4::9:12346 hello6` → `recvfrom world6`) passes against a vnet peer that answers NS S+O (guest ND goes REACHABLE, queued UDP flushes; `RECV6: b'world6'`, `mp6_peer_test.py` still green). Two root causes: (1) CYW43 SDPCM `bus_data_credit` window +4 starved immediate TX (now +20; free-running NS went 0→3 frames); (2) WFE fast-forward (10ms chunks at 100-1000× wall) let 1s `nd6_tmr` free the INCOMPLETE entry before wall-ms peer NAs were processed (GDB: destination entry + queued packet observed right after SENDTO execution, neighbor entry gone by NA time, `nd6_send_q` never hit) — Bramble now snoops guest NS TX and freezes fast-forward for 4s wall (host polls still run) so the NA wins the race. Gateway NAT64/DNS64 green (userspace, no root, no raw sockets): WKP `64:ff9b::/96` TCP/UDP/ICMP-echo stateful proxy to host v4 (SYN/SYN-ACK/ACK + seq/ack translation, FIN/RST; per-4-tuple UDP; per-(guest,ID,dst) unprivileged ping sockets for echo with SEQ-match + guest ID/seq re-stamp since the kernel rewrites the v4 echo ID per-socket; reverse unicast to mapping owner; UDP/ping 90s, TCP 600s sweepers), DNS64 for UDP/TCP-53 to `fd00:4::1` (AAAA passthrough, else synthesize from A with TTL clamp 600 via miekg/dns), RDNSS in RA (type 25 len 3, lifetime 600, server `fd00:4::1`, RA 64→88B); `nat64.go` + `nat64_test.go` (8 tests, loopback, no internet), live RA/RDNSS + UDP/TCP + ICMP-echo (loopback + `example.com` internet) + DNS64-internet verified (see `web/gateway-change.md`). |
| BT / BLE advertising | ✅ bare-metal + room + HCI forward | HCI ring fixed (B2H payload excludes H4 type byte; event `len` covers ncmd+opcode+status/params; BT events off the GPIO wake line — a level storm starved the thread-mode scheduler task). Bare-metal `ble_adv_rv32` does BT bring-up + RESET/ADV_PARAMS/ADV_DATA/ADV_ENABLE/SCAN_ENABLE with CC waits (RESET-OK → ADV-OK → LISTEN). Room: ADV announced over vnet ethertype `0x88B5` + 2s beacons; scanners synthesize LE Advertising Reports — two peered instances both print SCAN-OK (`ble_peer_test.py` locks the wire format). **HCI forwarding** (`-bt-hci <sock>` + `web/hci_bridge.py`, H4 unix↔TCP): guest drives a real Bumble virtual controller (`bumble.apps.controllers tcp-server:_:9544 …:9545`) — bare-metal ADV appears in `bumble.apps.scan` as BRV32, and MicroPython `BLE().active()` completes against it (needs bridge `--short-circuit fc01,0c63,0c6d,2017` + canned LE_Read_Buffer_Size_V2/LE_Encrypt replies for Bumble's minimal controller). Physical adapters via `bumble-hci-bridge tcp-server:_:PORT hci-socket:N` (needs the adapter DOWN + CAP_NET_RAW, i.e. root — verified to EPERM as user). Browser guests reach Bumble through `cli.js --ble-hci ws://host:port/api/ble-gateway` (WASM H4 export + WS pump, `test-wasm-ble.js`). **Internal GATT** (no controller needed): `ble_gatt_rv32` loopbacks `LE_Create_Connection` to its own MAC (link 0x0042) then runs ATT MTU-exchange → Read 0x0011 (`Bramble`) → Write 0x0012 (`Hi`) → Read-back (`GATT-DONE`, sweep-locked, WASM-verified). MP `gap_advertise` path (fixed 2026-09-13, all emulator-side): the bundled btstack gates legacy ADV on `Read_Local_Supported_Commands` (ext-ADV bit byte36:6 must be CLEAR) and gates the whole init on `Read_Local_Supported_Features` (bit38 LE-Supported, byte4:6 — our old reply had only bit37 set, so `hci_le_supported()` was false and no init HCI beyond Reset ever completed); missing `FC01` (chipset Set_BD_ADDR), `0x2060` (LE_Read_Buffer_Size_V2), `0x2017` (LE_Encrypt) stalls/short-circuits removed the same way; `bt_local_addr` identity now follows FC01 (Read_BD_ADDR + ADV carry mac+1, WiFi MAC untouched; `test_gatt_fc01_bdaddr_sync`). Proven at unit level: `test_gatt_hci_init_path` drives Reset→Version→0x1002→0x1003→BD_ADDR and asserts the exact decision bytes (legacy block set, ext-ADV/V2 clear, LE bit set, 64B CC — the old 32B-truncated CC was also fixed). Live-MP E2E re-tested 2026-09-20: stock Pico-W BT build boots to REPL over `-stdin`/UART0, but `import bluetooth` hangs the guest (no output past the import — the BT stack never reaches `BLE-ACTIVE`; REPL drive via `repl_drive.py` works mechanically). Needs deeper HCI bring-up work (see CHANGELOG). External-controller self-connect: `0x200D` to our own `bt_local_addr` is now served by internal loopback even with `-bt-hci` attached (RootCanal accepts it with status 0 but never emits Connection Complete — no LL peer exists — so forwarding it stalls GATT forever; verified: fake-controller capture shows zero forwarded `0x200D`, `GATT-DONE` green). |

### BLE backends compared

| Backend | Type | Accuracy | Transport | Needs | Status |
|---|---|---|---|---|---|
| RootCanal (`pip install rootcanal`) | standalone virtual controller (ex-Android CTS) | highest — MP init passes with zero shims | H4/TCP `:6402` (new connection = new controller) | pip package | ✅ default backend |
| Bumble virtual controller (`bumble.apps.controllers`) | minimal test controller + rich apps | partial — needs bridge `--short-circuit fc01,0c63,0c6d,2017` + canned `2060`/encrypt replies | H4/TCP | `pip install bumble` | ✅ apps/peers (scan, GATT), controller needs shims |
| Internal responder (`src/cyw43.c`) | ATT DB + loopback LE-U link | ADV + GATT (MTU/Read/Write, handles 0x0010–0x0012, link 0x0042) | in-emulator | nothing | ✅ offline default |
| Physical `hci0` via `bumble-hci-bridge` | real silicon (here: Realtek) | exact | `hci-socket:N` | adapter DOWN + root (EPERM verified as user) | 🟡 documented, needs root session |
| BlueZ `btvirt` / VHCI attach | kernel virtual controllers | n/a here | VHCI `/dev/vhci` | not installed + root-only node | 🟡 deferred |
| Zephyr `native_sim` | production controller, H4/TCP `--bt-dev=` | highest (spec-conformant) | bridge-compatible as-is | west + toolchain + build | 🟡 deferred backstop |
| BTstack / NimBLE | host-side stacks | n/a (wrong end for controller role) | — | — | peers only; NimBLE notable for ESP32-C6 parity work |
| Go: `go-ble/ble` (host), `muka/go-bluetooth` (BlueZ D-Bus) | host/peer/control in Go | — | HCI / D-Bus | — | no mature pure-Go virtual controller exists |

## Per-arch WiFi status

| Arch | Scan | STA join | DHCP | TCP/UDP | Notes |
|---|---|---|---|---|---|
| RP2040 M0+ (Pico W) | ✅ | ✅ WPA2 + open | ✅ fake/real/guest | ✅ | Reference path. |
| RP2350 M33 (Pico 2 W) | ✅ | ✅ | ✅ fake | ✅ UDP send; TCP via same models | Needed DMA CTRL remap + SXTAB family. Arduino `m33wifi.ino` green since 2026-09-19 (`SCAN n=3`, join `.2`). |
| RP2350 RV32 (Hazard3) | ✅ | ✅ WPA2 | ✅ fake (.2) | ✅ TCP server | pico-sdk `wifi_scan` finds 3/3 APs; `rvwifi_join` (lwip_poll) joins BrambleNet + DHCP .2 (HOST_WAKE level fix 2026-09-19 — was STALLing on bus credit). Bare-metal `webserver_rv32` serves HTTP on :80 (ARP→SYN→HTTP→FIN vs vnet peer, checksums OK). Bare-metal `ble_adv_rv32` advertises + scans via BT shared bus (room-tested). |

## CYW43 model notes (for debuggers)

- PIO-SPI intercept auto-detects the SM by `sideset_base==29`
  (PIO0/1/2 all modeled; Arduino uses PIO1/PICO2).
- SDPCM channels 0/1/2 (CTL/EVENT/DATA), RX queue (32), GPIO24
  HOST_WAKE LEVEL-held HIGH while frames wait (edge-pulse broke it:
  SDK uses LEVEL_HIGH + disable-until-POST_POLL_HOOK; needs correct
  per-chip IO_BANK0 maps).
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
3. ~~IPv6 (needs gateway + lwIP6 + SLAAC work; parked)~~ — done (unicast E2E green, see matrix; gateway NAT64/DNS64 green — userspace WKP+NAT+DNS64+RDNSS, see `web/gateway-change.md`).
4. ~~BLE~~ — done (bare-metal ADV + vnet room + HCI forwarding to Bumble/virtual + physical-adapter path, see matrix; MP `gap_advertise` emulator blockers fixed — legacy-only 0x1002, LE-bit 0x1003, FC01/0x2060/0x2017, BT identity sync; live-MP proof awaits a BT-capable MP build).
5. ~~MicroPython WiFi~~ — done: Pico W MP firmware joins + DHCP (.2) under `-wifi`. Needed F2-watermark scratch reg (BT builds abort bus_init on readback mismatch) and join events queued at SET_SSID-response pop (ACTIVE race). Verified on pristine local build AND official v1.24.1 release image via USB REPL (`B 3 ('192.168.4.2', ...)`); needed stdin target reorder (USB CDC preferred when viable, UART banner must not steal REPL input; littleOS unaffected). Real-gateway DHCP also proven: `-nodhcp -net -net-peer` + `gateway_bridge.py` → Go gateway DORA, `STAT1: 3`, `.2` (`GOTIP at iter 102`). Needed vnet pre-accept TX backlog (16 frames, flushed on accept) + accept-on-TX: guest WFE fast-forward starves `vnet_poll`, so the DHCP burst was dropped before the bridge attached; `gateway_bridge.py` also survives guest exit (reconnect instead of `TypeError` crash). Exchange is sub-second wall-clock once attached (DORA + ARP in <1s); `cooperative` WFE fast-forward also runs host polls via `bramble_ff_poll_hook` so RX/accept don't starve during sleeps. Known limitation: guest lwIP fine-timers don't retransmit (3-frame initial burst only, verified 60s with no server) — harmless since live servers answer the burst synchronously.
6. ~~WASM gateway E2E against a live Go gateway (plumbing merged,
   headed test pending)~~ — done via `test-wasm-gateway.js`: in-process
   WS gateway (DHCP+ARP) + Pico SDK join sample in WASM gets .2. Needed
   `bramble_wifi_enable` export (`--wifi` in cli.js, `--gateway` implies
   nodhcp) since WASM never called `cyw43_init`.
7. Arduino E2E sketches live in `test-firmware/arduino/` (need
   arduino-cli; stay out of `ctest`/sweep — see its README).
8. ~~Single gateway for WiFi + Ethernet~~ — done: socket-0 MACRAW joins
   the shared vnet bus (see matrix above). Guest DHCP/ARP/IP flow to the
   same Go gateway room as CYW43; verified DHCP OFFER (`.2`), ARP reply,
   and WiFi+eth coexistence live against the gateway. The per-socket
   live path (`-net-live`, `net_proxy.py /w5500`) remains for offload
   sockets 1–7 only — it is not a second gateway.
