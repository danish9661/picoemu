# Gateway changes

This file records modifications to the OpenHW Studio gateway
(`openhw-studio-gateway/`, default port **5090** — unchanged).

## 2026-09-12 — IPv6 NDP + echo for rooms (no затрагивание v4 paths)

**Why:** emulator guests speak IPv6 (SLAAC, Neighbor Discovery, ping6),
but the gateway only handled IPv4/ARP. IPv6 frames need gateway-side
answers for the router identity; everything else already flowed through
the room hub broadcast untouched.

**What changed** (`handleICMPv6.go` new, hooks in `main.go`, one case in
`bridge.go`):

- `handleICMPv6(msg, client) bool` — answers, directly back to the
  requester (same pattern as the DHCP intercept):
  - Router Solicitation (133) → Router Advertisement (134) with the
    `fd00:4::/64` SLAAC prefix (L+A flags, valid 86400 / preferred
    14400), MTU 1500, source link-layer option. Unspecified source
    gets a multicast reply (`ff02::1`); otherwise unicast.
  - Neighbor Solicitation (135) for `fe80::1` / `fd00:4::1` →
    Advertisement (136) with solicited+override flags + target
    link-layer option. Other targets fall through (room broadcast).
  - Echo Request (128) to a gateway address → Echo Reply with payload
    echoed. Other destinations fall through.
  - All replies carry correct hop limit (255 for NDP) and pseudo-header
    checksums; short/runt frames are ignored, never answered.
- Gateway identity: MAC `5a:94:ef:e4:0c:dd` (same as the v4 config),
  `fe80::1`, `fd00:4::1` — mirrors the emulator fake network so guests
  see one consistent router with or without `-nodhcp`.
- `main.go` `handleClient`: after the DHCP intercept, IPv6 frames are
  offered to `handleICMPv6`; unhandled ones keep the old path (room
  broadcast + gVisor uplink, which drops v6).
- `bridge.go` `extractFrameIP`: `0x86DD` returns the source address so
  v6 devices show up in BOARD_IP discovery like v4 ones.

**Deliberately NOT changed:**

- Port stays **5090** (`const PORT`, `GATEWAY_PORT` override kept).
- No DHCPv6 (SLAAC covers addressing).
- No gVisor-v6 (not needed — userspace NAT64 below rides on host v4 sockets).
- ICMP echo to WKP untranslated by design (falls through; would need raw sockets).

## 2026-09-13 — userspace NAT64 + DNS64 + RDNSS (v6 egress, no raw sockets)

**Tests** (`handleICMPv6_test.go`, stdlib + gorilla only):

- `TestRStoRA` — RA arrives, checksum valid, prefix option correct.
- `TestNStoNA` — NA has S+O flags, right target/option/checksum;
  foreign NS gets silence.
- `TestEchoReply` — payload echoed, checksum valid; foreign echo silent.
- `TestShortFramesIgnored` — runts/junk never answered, no crash.
- E2E: RV32 `ping6` guest via `gateway_bridge.py` gets the gateway RA
  (`RV32 PING6 RA-OK`) with emulator fake services off (`-nodhcp`).
- v4 untouched by construction (new ethertype branch only); existing
  DHCP/NAT flows unchanged.

**Why:** gVisor VN is v4-only, so off-link guest v6 died in the pipe.
Translate it to host v4 with kernel sockets (no root/raw sockets).

**What changed** (`nat64.go` new ~750L, `handleICMPv6.go` RDNSS, `main.go`
wiring — `Room.NAT64`, `newNat64Engine(room.Ctx.Done())` + `sweepLoop`,
`handleNAT64()` after `handleICMPv6` in `handleClient`):

- WKP `64:ff9b::/96` only; link-local/multicast/on-link `fd00:4::/64` /
  `fe80::1` / `fd00:4::1` fall through to room broadcast as before.
- TCP stateful (SYN→`Dial("tcp4")` 10s, SYN-ACK MSS 1440, seq/ack
  translation, duplicate-SYN resend, FIN half-close, RST on Refused).
- UDP stateful (per-4-tuple `DialUDP`, 64 hop echo-back, reverse unicast
  to mapping owner; UDP 90s / TCP 600s sweepers, room-scoped ctx).
- DNS64 for UDP (+TCP passthrough) port 53 to `fd00:4::1` only
  (miekg/dns; `/etc/resolv.conf` else 8.8.8.8/1.1.1.1): AAAA passthrough,
  else synthesize from A with TTL clamp 600; NODATA/NXDOMAIN pass through.
- RA 64→88B with RDNSS (type 25 len 3, lifetime 600, `fd00:4::1`) so
  v6-only guests learn the resolver from SLAAC alone.

**Tests** (`nat64_test.go` 7 new, loopback via `64:ff9b::7f00:1`, no
internet; `TestBuildRA` now expects 14+40+88 + RDNSS header/lifetime/server):

- `TestNAT64TCP` / `TestNAT64TCPRefused` / `TestNAT64UDP` /
  `TestNAT64Passthrough` / `TestDNS64Synthesize` / `TestDNS64Passthrough`
  / `TestDNS64TTLClamp` — all PASS; full suite 12/12.
- Live (real binary :5091): RA 88B with RDNSS (25,3); UDP `ECHO:hello`
  + TCP SYN→SYN-ACK (0x12) via WKP loopback echo servers; DNS64
  `example.com` → synthesized AAAA over live internet.
