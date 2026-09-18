# Gateway networking (OpenHW-style)

Connect picoemu guests (Pico W / Pico 2 W WiFi via CYW43, W5500,
future STM32/ESP/BLE) to real networks through a local gateway that
speaks raw Ethernet frames over WebSocket — the same protocol as the
OpenHW Studio gateway (`ws://host:5090/api/network-gateway`).
(Ours defaults to 5090; the upstream/STM32 gateway uses 5099.)

## How it works

```
guest firmware (CYW43/W5500, DHCP)
  -> vnet (emulator Ethernet bus)
  -> WS uplink mirror -> browser/CLI --raw ETH binary WS-->
  Go gateway (gVisor NAT/DHCP/DNS, 192.168.4.0/24)
  -> real sockets -> internet
```

- Outbound: every guest frame on vnet is queued by the `ws-uplink`
  mirror and forwarded unchanged. No prefixes, no re-framing.
- Inbound: gateway frames are injected via `bramble_eth_push_rx` and
  distributed to ports/peers/TAP like any vnet frame. Gateway-originated
  frames are tagged so they are never echoed back (no loops).
- DHCP/DNS/NAT come from the gateway: guest stacks that DHCP (Pico-SDK
  CYW43 firmware, ESP, STM32 LWIP) configure themselves automatically.
- Rooms/multiplayer: the room is the gateway `?sessionId=` — two
  simulators with the same room share one virtual LAN.

## Browser UI

Controls panel → Gateway: URL defaults to
`ws://localhost:5090/api/network-gateway`. Optional Room value becomes
`?sessionId=`. Go connects (status `on`); closing/disconnect shows
`off` and disables the uplink. Then load any network firmware — no
other configuration needed.

## Node CLI

```sh
npx picoemu wifi_demo.uf2 --gateway ws://localhost:5090/api/network-gateway --room lab
```

## Native CLI (no root, no TAP)

The native emulator bridges CYW43 Ethernet into vnet; `web/gateway_bridge.py`
(union socket <-> gateway WS, stdlib only) carries it to the Go gateway,
which provides DHCP/DNS/NAT. Verified: two Pico W instances in one room
(ARP, TCP echo, DHCP lease 192.168.4.2).

```sh
python3 web/gateway_bridge.py --sock /tmp/gwA.sock --room lab &
python3 web/gateway_bridge.py --sock /tmp/gwB.sock --room lab &
./build/bramble server.uf2 -wifi -nodhcp -net -net-peer /tmp/gwA.sock -mac DE:AD:BE:EF:00:01 &
./build/bramble client.uf2 -wifi -nodhcp -net -net-peer /tmp/gwB.sock -mac DE:AD:BE:EF:00:02
```

Wired Ethernet takes the same path with no WiFi flags (the W5500
MACRAW socket 0 is just another vnet port — `-nodhcp` is not needed
because the W5500 has no fake server):

```sh
python3 web/gateway_bridge.py --sock /tmp/eth.sock --room lab &
./build/bramble web/eth_dhcp.uf2 -board pico-eth -net -net-peer /tmp/eth.sock
# in-tree guest prints ETH IP=192.168.4.2 + ETH DONE (DORA via the gateway)
```

Flags: `-nodhcp` disables the fake DHCP server so DHCP/DNS flow to the
gateway; `-mac` overrides the CYW43 MAC (needed: distinct MACs per room).
Direct instance meshing works the same way with one shared `-net-peer`
path and no gateway at all.

## Local gateway setup

```sh
cd openhw-studio-gateway
go run .                      # or: ./start-gateway.sh
# listens on ws://localhost:5090/api/network-gateway
# (GATEWAY_PORT env overrides; upstream default is 5099)
```

Private mode (default) also port-forwards `localhost:8080` → guest `:80`,
so firmware web servers are reachable from the host browser.

## Scope and limits

- BLE is **partially** emulated: the CYW43 model answers HCI bring-up/ADV
  in-emulator and runs a loopback ATT/GATT responder (MTU/Read/Write on
  handles 0x0010–0x0012; `web/wifi_ble_gatt_rv32.uf2` → `GATT-DONE`, no
  peer needed). HCI *forwarding* to an external controller (Bumble /
  RootCanal / physical `hci0` via `bumble-hci-bridge`) is the path for
  real over-the-air peers — see `docs/NETWORKING.md` BLE row. The
  gateway's `/api/ble-gateway` endpoint exists for ESP/Bumble flows;
  the internal responder needs no gateway attachment.
- WiFi modes: STA join is accepted (success events queued) and traffic
  flows as ETH. Soft-AP is modeled too: `bsscfg:ssid` + `bss up` bring
  up an AP (SSID advertised in scans, SET_SSID/LINK events on the AP
  interface); guests join it like any AP and the guest's own DHCP
  server + vnet switching carry STA traffic. Verified: Pico W AP +
  Pico W STA (DHCP + TCP echo, direct vnet mesh).
- Pico W (RP2040) and Pico 2 W (RP2350) share the CYW43 PIO hookup;
  both funnel into the same vnet/gateway path.
- End-to-end traffic needs guest firmware with a network stack
  (Pico-SDK CYW43, LWIP). The bundled bare-metal demos don't emit ETH —
  except the in-tree W5500 guests (`web/eth_dhcp{,_pico2,_rv32}.uf2`
  print `ETH DONE` after gateway DORA; `web/eth_http*` add ARP→SYN→
  GET→200→FIN against `test-firmware/http_peer_test.py`);
  unit (`test_vnet_ws_mirror`) + stub-gateway headed tests cover the
  plumbing.
