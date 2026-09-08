# Gateway networking (OpenHW-style)

Connect picoemu guests (Pico W / Pico 2 W WiFi via CYW43, W5500,
future STM32/ESP/BLE) to real networks through a local gateway that
speaks raw Ethernet frames over WebSocket — the same protocol as the
OpenHW Studio gateway (`ws://host:5099/api/network-gateway`).

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
`ws://localhost:5099/api/network-gateway`. Optional Room value becomes
`?sessionId=`. Go connects (status `on`); closing/disconnect shows
`off` and disables the uplink. Then load any network firmware — no
other configuration needed.

## Node CLI

```sh
npx picoemu wifi_demo.uf2 --gateway ws://localhost:5099/api/network-gateway --room lab
```

## Local gateway setup

```sh
cd openhw-studio-gateway
go run .                      # or: ./start-gateway.sh
# listens on ws://localhost:5099/api/network-gateway
```

Private mode (default) also port-forwards `localhost:8080` → guest `:80`,
so firmware web servers are reachable from the host browser.

## Scope and limits

- BLE is **not** emulated (neither here nor upstream): Pico W Bluetooth
  needs an HCI-UART transport + BT stack on both sides. The gateway's
  `/api/ble-gateway` endpoint exists for ESP/Bumble flows; picoemu has
  nothing to attach to it yet.
- WiFi modes: STA join is accepted (success events queued) and traffic
  flows as ETH; AP mode is not modeled.
- Pico W (RP2040) and Pico 2 W (RP2350) share the CYW43 PIO hookup;
  both funnel into the same vnet/gateway path.
- End-to-end traffic needs guest firmware with a network stack
  (Pico-SDK CYW43, LWIP). The bundled bare-metal demos don't emit ETH;
  unit (`test_vnet_ws_mirror`) + stub-gateway headed tests cover the
  plumbing.
