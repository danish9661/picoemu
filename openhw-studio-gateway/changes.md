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
- No NAT66 / upstream v6 (gVisor VN here is v4-only; room-local and
  gateway-reachable v6 work, internet v6 does not — see
  `../../web/gateway-change.md` for the full scope).
- No RDNSS option (guests keep v4 DNS from DHCP).

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
