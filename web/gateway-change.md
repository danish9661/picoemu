# Gateway changes required for IPv6

Date: 2026-09-12. Status: RECORDED, not implemented (see why below).

## Background

The emulator side is done (see `docs/NETWORKING.md`, IPv6 row):
- vnet carries IPv6 (ethertype-agnostic forwarding).
- The CYW43 fake network answers Router Solicitations (RA with
  `fd00:4::/64` SLAAC prefix) and Neighbor Solicitations for the
  gateway addresses (`fe80::1`, `fd00:4::1`).
- Guest-to-guest IPv6 works (RV32 `ping6` E2E: NS/NA + echo, verified;
  MicroPython SLAAC + DAD observed on vnet).

What does NOT work is IPv6 **beyond the virtual LAN**: internet access,
global addresses, and DNS64/NAT64. That needs gateway work.

## Why this is a record, not a patch

- `:5090` (`~/openhw-gw5090`) is a compiled Go binary with no source
  available here — it cannot be modified.
- `:5099` source (`Documents/stm32 F4/openhw-local-gateway`, package
  `stm32F4-emulator`) belongs to another workstream — left untouched
  on purpose. Its `bridge.go` handles only `0x0800` (IPv4) and `0x0806`
  (ARP); `0x86DD` falls through to `return nil` (see `extractFrameIP`).

## Required gateway changes (for whoever owns each instance)

1. **Forward `0x86DD`.** Extend `extractFrameIP` (and any ethertype
   allow-list on the WS/room path) to pass IPv6 frames, including
   multicast (`33:33:xx:xx:xx:xx`, esp. `ff02::1`, solicited-node).
2. **Router Advertisements.** Originate periodic RAs (or relay +
   re-originate) so guests keep a default route. Prefix choice must
   agree with the emulator fake (`fd00:4::/64`) OR the emulator fake
   must defer when a gateway RA is present (preferred: gateway wins;
   emulator RA stays for offline use).
3. **NDP proxying.** Answer/forward NS/NA between room members and the
   gateway's own addresses so L2 resolution works across WS peers.
4. **Upstream connectivity.** Either NAT66 (ULA → host global, mirrors
   the existing gVisor v4 NAT) or a routed `/64` per room.
5. **DNS.** AAAA answers via the existing DNS path (likely works once
   (1) forwards; verify).
6. **DHCPv6: not required.** SLAAC covers address assignment; skip
   unless stateful addressing is wanted.

## Test plan (when implemented)

1. RV32 `ping6` guest → `ping6` to an internet host through the room.
2. MicroPython (LWIP_IPV6=1 test build) gets RA, forms global address,
   opens a UDP6 socket off-LAN.
3. Existing v4 tests (`sweep_all.sh`, `test-wasm-gateway.js`) still green
   (no regression in NAT44/DHCPv4/ARP paths).
