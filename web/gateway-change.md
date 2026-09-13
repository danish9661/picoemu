# Gateway changes required for IPv6

Date: 2026-09-12 (updated 2026-09-13). Status: DONE (vendored source: NDP + NAT64/DNS64 + RDNSS).

## Background

The emulator side is done (see `docs/NETWORKING.md`, IPv6 row):
- vnet carries IPv6 (ethertype-agnostic forwarding).
- The CYW43 fake network answers Router Solicitations (RA with
  `fd00:4::/64` SLAAC prefix) and Neighbor Solicitations for the
  gateway addresses (`fe80::1`, `fd00:4::1`), plus periodic
  unsolicited RAs (7s) for stacks that never send RS.
- Guest-to-guest IPv6 works (RV32 `ping6` E2E: NS/NA + echo, verified;
  MicroPython LWIP_IPV6=1 SLAAC + NA + echo verified via
  `test-firmware/mp6_peer_test.py`).

What does NOT work is IPv6 **beyond the virtual LAN**: internet access,
global addresses, and DNS64/NAT64. That needs gateway work.

## Source situation (updated)

- `openhw-studio-gateway/` is now vendored in this repo (committed) and
  already handles `0x86DD` (`handleICMPv6.go`: RS→RA, NS→NA, echo;
  `bridge.go` extracts frame IPs) — item 1 below is DONE there.
- `:5090` (`~/openhw-gw5090`) is a compiled Go binary with no source
  available here — it cannot be modified.
- `:5099` source (`Documents/stm32 F4/openhw-local-gateway`, package
  `stm32F4-emulator`) belongs to another workstream — left untouched
  on purpose. Its `bridge.go` handles only `0x0800` (IPv4) and `0x0806`
  (ARP); `0x86DD` falls through to `return nil` (see `extractFrameIP`).

## Required gateway changes (for whoever owns each instance)

1. ~~Forward `0x86DD`~~ — DONE in vendored `openhw-studio-gateway/`
   (`handleICMPv6.go`, `bridge.go`).
2. **Router Advertisements.** Originate periodic RAs (or relay +
   re-originate) so guests keep a default route — REQUIRED for
   timer-less stacks (MP RP2 never sends RS: no async_context pump),
   not just nice-to-have. Prefix choice must agree with the emulator
   fake (`fd00:4::/64`) OR the emulator fake must defer when a gateway
   RA is present (preferred: gateway wins; emulator RA stays for
   offline use).
3. **NDP proxying.** Answer/forward NS/NA between room members and the
   gateway's own addresses so L2 resolution works across WS peers.
4. ~~Upstream connectivity.~~ DONE (2026-09-13, userspace NAT64): WKP `64:ff9b::/96` TCP/UDP/ICMP-echo stateful proxy to host v4 sockets (no root, no raw sockets, no gVisor-v6 needed) — `openhw-studio-gateway/nat64.go`, `Room.NAT64` + `handleNAT64()` after `handleICMPv6` in `handleClient`. Link-local/multicast/on-link/`fe80::1`/`fd00:4::1` fall through to room broadcast as before; non-WKP v6 and non-echo ICMPv6 by design untranslated. ICMP echo uses unprivileged `icmp.ListenPacket("udp4")` ping sockets (kernel rewrites the v4 echo ID per-socket, so flows match on SEQ and re-stamp the guest ID/seq in the v6 type-129 reply).
5. ~~DNS.~~ DONE (2026-09-13, DNS64): AAAA passthrough, else synthesize `64:ff9b::/96` AAAA from A with TTL clamp 600 (miekg/dns, UDP + TCP-53 to `fd00:4::1` only); RA carries RDNSS (type 25 len 3, lifetime 600, server `fd00:4::1`, RA 64→88B) so v6-only guests learn the resolver from SLAAC alone.
6. **DHCPv6: not required.** SLAAC covers address assignment; skip
   unless stateful addressing is wanted.

## Test plan (when implemented)

Live-verified 2026-09-13 (all against a real gateway binary on :5091):
1. RA arrives with 88B ICMP (RDNSS type 25 len 3 present).
2. UDP/TCP to `64:ff9b::7f00:1` (127.0.0.1 echo servers) round-trip through the room (`ECHO:hello`, SYN→SYN-ACK flags 0x12).
3. ICMPv6 echo to WKP loopback → type-129 reply (guest ID/seq preserved); ICMPv6 echo to WKP-mapped `example.com` → live-internet echo reply.
4. DNS64 to `fd00:4::1:53` for `example.com` returns synthesized AAAA (live internet, no test shim).
5. Unit: `go test ./...` 13/13 (`nat64_test.go` 8 tests on loopback + fake DNS: TCP echo/Refused, UDP echo, ICMP echo, DNS64 synthesize/passthrough/TTL-clamp, passthrough — no internet needed).
6. Existing v4 tests (`sweep_all.sh` 54/54, `test-wasm-gateway.js`) still green (new ethertype/UDP-53 branches only).
