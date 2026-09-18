# agent.md — Bramble eth_http handover (HTTP-over-Ethernet E2E)

Date: 2026-09-18. Commits: `93b962a` (eth-dhcp) → `bc12b6f` (eth-http) →
round-2 commit (see §7).
Status: **eth_http DONE x3 (M0+/M33/RV32)** — full app exchange green via
`http_peer_test.py`, sweep-wired (59/60, 1 pre-existing wifi flake),
docs updated, browser presets added, live-gateway DORA proven. `src/`
untouched (no emulator changes in this round).

## 1. What this work is

In-tree guest firmware that does DHCP DORA **plus** a static-blob HTTP/1.0
client over W5500 MACRAW socket 0 (single-gateway path, same vnet bus as
eth_dhcp / WiFi). The peer (`http_peer_test.py`) plays DHCP server +
gateway (.1) + TCP:80 server.

Guest flow (identical on all 3 arches, only addresses/consts differ):

1. DORA exactly as eth_dhcp → prints `ETH … ETH DONE`
2. `ETH HTTP-START`, SEND ARP who-has .1, await reply → `ETH ARP-OK`
3. SEND SYN .1:80, await SYN-ACK → `ETH SYNACK-OK`
4. SEND handshake ACK (no wait); SEND `GET /`, await `HTTP/1.0 200` +
   `hello-eth` → `ETH HTTP-OK`
5. SEND ACK2, SEND FIN, await FIN-ACK; SEND final ACK → `ETH HTTP-DONE`
6. Any stage timeout prints `ETH FAIL <stage>-TIMEOUT` + `ETH FAIL`

Per-arch identity (`eth_http_common.HTTP_ARCHES`, mirrors eth_dhcp `ARCHES`):

| arch | MAC | XID (=cseq BE) | sport | UF2 | family |
|------|-----|----------------|-------|-----|--------|
| M0+ | 02:11:22:33:44:60 | A0B0C0D0 | 0xE100 | web/eth_http.uf2 | 0xE48BFF56 |
| M33 | 02:11:22:33:44:61 | B0B1C0D1 | 0xE101 | web/eth_http_pico2.uf2 | 0xE48BFF59 |
| RV32 | 02:11:22:33:44:62 | C0B2C0D2 | 0xE102 | web/eth_http_rv32.uf2 | 0xE48BFF5A |

Peer server SEQ (`SSEQ`) is always `0x00100000`, so guests never build
checksums at runtime — all 7 TX frames/arch are static blobs with
precomputed checksums; RX is parsed byte-at-a-time.

## 2. Files (bc12b6f: 7 NEW + 5 MOD + agent.md; uncommitted round: §7)

| File | State | Notes |
|------|-------|-------|
| test-firmware/gen_eth_http.py | NEW (1461 lines) | Generator. ARM = `gen_eth_dhcp.arm_source()` + string surgery; RV32 = `rv32_source()` + emitted funcs |
| test-firmware/eth_http_common.py | NEW (210) | Frame builders, checksums, peer-side parsers (`parse_tcp_from_guest`, `is_arp_who_has`, `arp_reply`, `tcp_seg_from_srv`, `ip_pkt_from_srv`) |
| test-firmware/http_peer_test.py | NEW (208) | E2E peer: DORA → ARP → SYN-ACK → HTTP 200 → FIN-ACK; exits 0 + `ALL HTTP CHECKS PASSED` |
| test-firmware/eth_http.S | NEW (1959) | Generated ARM source (M0/M33 via `#ifdef RP2350`) |
| test-firmware/eth_http_rv32.S | NEW (1428) | Generated RV32 source |
| web/eth_http.uf2 | NEW (10240B) | M0+ image |
| web/eth_http_pico2.uf2 | NEW (10240B) | M33 image |
| web/eth_http_rv32.uf2 | NEW (12800B) | RV32 image |
| test-firmware/build_eth.py | MOD | `http-m0/http-m33/http-rv32/http-all` targets + `--gen-http` |
| test-firmware/dhcp_peer_test.py | MOD | `if __name__ == "__main__"` guard + `wait_for` buffered-drain fix (see §4.4) |
| test-firmware/sweep_all.sh | MOD | `run_eth` dead-peer `ETH MACRAW-OK` lines for the 3 HTTP guests |
| docs/NETWORKING.md | MOD | New ✅ HTTP-guest row; NOT-done cell updated |
| agent.md | NEW | This handover file |
| web/index.html | MOD (uncommitted) | Demo presets: eth_dhcp/eth_http × 3 arches in the Examples dropdowns |

### 4.5 ARM `patch_request` server-IP off-by-one (found via LIVE gateway)

`patch_request` stamped the opt54 server-IP value at TXBUF+292 instead of
+293 (`adds r0, #2` after the 4th yip byte instead of `adds r0, #3`), so
the REQUEST carried `...54 04 01 C0 A8 04` (opt54 value `01 C0 A8 04`).
The python peer walks options leniently and still ACKed, so all
`dhcp/http_peer_test.py` runs stayed green — but the strict Go gateway
(`insomniacslk/dhcp`) rejected the REQUEST
(`buffer too short at position 11: have 49, want 192`). Fix:
`gen_eth_dhcp.py` `patch_request`: `adds r0, #2` → **`adds r0, #3`**.
Regen both guests (`--gen --gen-http all http-all`), all 6 UF2s rebuilt,
python-peer DORA+HTTP re-verified x2 (dhcp M0, http M0). RV32
`rv_patch_request` uses absolute `TXBUF+287/+293` adds and was correct.

`src/`, `include/` are **clean vs HEAD** (temporary `w5500.c` ring probes
were added during debugging and fully reverted — verified via
`git diff HEAD --stat -- src/` = empty, backup matched HEAD).

Do NOT commit (untracked build artifacts): `test-firmware/*.bin`,
`test-firmware/*.o`, `test-firmware/__pycache__/`.

## 3. Verified results (2026-09-18)

- `http_peer_test.py` **green x3**, peer prints `ALL HTTP CHECKS PASSED`,
  guest prints `ETH HTTP-DONE` on all arches.
- Dead-peer sweep lines (`ETH MACRAW-OK`, 3M steps): PASS x3 via the
  same `run_eth` helper.
- `./build/bramble_tests`: **411/411 passed, 0 failed**.
- Full `./test-firmware/sweep_all.sh build`: **59 passed, 1 failed** —
  the single failure is `wifi_join_rv32.uf2` (want `RV32 JOIN DONE`), a
  **pre-existing flake on main** (documented in the `93b962a` message;
  unrelated to eth — our 6 eth lines all PASS).
- Live Go-gateway DORA (room `ethhttptest2`, `gateway_bridge.py`):
  DISCOVER→OFFER→REQUEST→**ACK** all green after the §4.5 fix
  (`ETH IP=192.168.4.2`, `ETH DONE`, `ETH ARP-OK`). HTTP stage then stalls
  **by design**: the gateway has no TCP :80 on 192.168.4.1 — its
  `127.0.0.1:8080→.2:80` port-forward targets the *guest* as server
  (`no route to host` in gw log), i.e. it serves guests that LISTEN, while
  our guest DIALS OUT as a client. Full HTTP exchange stays covered by
  `http_peer_test.py`. Gateway ARP replies arrive as ~7s 142B beacons.
- Full-run VNet counters (M0): `TX=9 RX=6 Peer-TX=9 Peer-RX=6`
  (guest TX = DISCOVER, REQUEST, ARP, SYN, ACK, GET, ACK2, FIN + 1 vnet
  retransmit/duplicate observed; guest RX = OFFER, ACK, ARP-reply,
  SYN-ACK, HTTP-reply + 1).
- M0 disassembly + pool audit: parse cmp-immediate sequence matches wire
  bytes exactly; literal pools contain RXBUF×10/TXBUF(shared pools) correctly.

## 4. Bugs found & fixed (read before touching the generator)

### 4.1 ARM double-`adds` after `__SPORT_LO_CMP__` (all 3 TCP parsers)

Template is `__SPORT_LO_CMP__\nbne <no>\nadds r0, #1` (= advance to +38).
The generator appended an **extra** `adds r0, #1` to the replacement, so
the cursor landed on +39 and every later field shifted by one (SYN-ACK
failed on seq[0]=0x10). Fix (`gen_eth_http.py` ~line 915-926): replace the
placeholder with `sport_lo` **only, no trailing adds**.

### 4.2 ARM `parse_http` PSH gate inverted

`ands r1, r2` (bit 8) followed `beq ph_got_prefix` = prefix check ran when
PSH was **clear**. Fix (~line 610): `beq` → **`bne`** (`parse_fin`'s
`beq pf_no` was already correct).

### 4.3 Payload pointer lost between prefix check and body scan (ARM + RV32)

`ph_prefix` returns 1 in r0/a0, clobbering the payload pointer, so
`ph_scan` scanned address 1. Fixes:
- ARM (~lines 614/618): `mov r4, r0` before `bl ph_prefix`,
  `mov r0, r4` before `bl ph_scan` (r4 is callee-saved in the
  `push {r4-r7,lr}` frame).
- RV32 (~lines 1210-1218, 1310-1324, 1386-1392): `_tcp_head` parsers
  (`rv_parse_synack/http/fin`) got an `sp-16 / sw ra / sw s0` prologue with
  matching restores on **all** return paths; http path does
  `mv s0, t0` (save) / `mv a0, s0` (restore) around `rv_ph_prefix`.

### 4.4 Peer `wait_for` dropped already-buffered frames

Guest sends ACK2+FIN back-to-back; both can arrive in one `recv`, so FIN
sat in `p.buf` when its wait started, and the old loop blocked on `recv`
until timeout. Fix (`dhcp_peer_test.py` `Peer.wait_for`): **drain buffered
frames before each `recv`**. (This also fixes latent `dhcp_peer_test.py`
behaviour; `parse_tcp_from_guest` itself was correct — blobs verify.)

## 5. Debug leftovers & gotchas

- `dbg_hex`/`dbg_dump` (ARM hex-dump helpers) are **kept in the
  generator as dead code, zero call sites** (the only 2 `bl dbg_dump`
  matches in `eth_http.S` are the how-to comment). To re-enable: insert
  `push {r0-r1} / ldr r0, =RXBUF / bl dbg_dump / pop {r0-r1}` before any
  `bl parse_*`, rebuild `http-m0`, remove afterwards (it pollutes UART).
- `#if DEBUG_DUMP` does **NOT** work in this ARM asm path (clang skipped
  the block → 0 `bl` refs in the object). Use unconditional code, never `#if`.
- `test-firmware/eth_http.S.o` / `.bin` share one path per arch build, so
  the on-disk `.o` is **whichever arch built last** (M33 after `http-all`).
  Rebuild `http-m0` alone before inspecting M0 disassembly.
- v6m rules for ARM guest code: `adds #imm≤7`, no `[Rn,#imm>31]`
  (`ldrb [r2,#1..#11]` OK), `cmp #imm≤255`, balanced push/pop per function,
  `.ltorg` within range of its `ldr` pool.
- Frame sizes: ARP 42, SYN/ACK/ACK2/FIN/FACK 54, GET 72, HTTP reply 120
  (57B header + 9B `hello-eth` at payload+57; `ph_scan` covers 64 positions).

## 6. Commands

```bash
# regenerate + rebuild all three (single command):
python3 test-firmware/build_eth.py --gen-http http-all

# M0 E2E (two terminals; guest first):
./build/bramble web/eth_http.uf2 -board pico-eth -net-peer /tmp/m0.sock \
  -clock 125 -timeout 90 -max-steps 2000000000
python3 test-firmware/http_peer_test.py /tmp/m0.sock   # expect ALL HTTP CHECKS PASSED

# M33 E2E: same with web/eth_http_pico2.uf2 -board pico-eth2
# RV32 E2E: web/eth_http_rv32.uf2 -board pico-eth -arch rv32

# sweep-style dead-peer check (no peer needed):
./build/bramble web/eth_http.uf2 -board pico-eth -net-peer /tmp/dead.sock \
  -clock 125 -timeout 50 -max-steps 3000000   # expect ETH DHCP-START + ETH MACRAW-OK

# native unit tests:
./build/bramble_tests   # 411/411
```

## 7. Round-2 commit (all staged — commit + push pending at handover)

- `test-firmware/gen_eth_dhcp.py` (§4.5 patch_request +3 fix) + regenerated
  `test-firmware/eth_dhcp.S`, `test-firmware/eth_http.S` + rebuilt
  `web/eth_dhcp.uf2`, `web/eth_dhcp_pico2.uf2`, `web/eth_http.uf2`,
  `web/eth_http_pico2.uf2` (+ rv32 UF2s rebuilt; RV32 logic byte-identical,
  sizes unchanged: dhcp_rv32 6144B, http_rv32 12800B)
- `web/index.html` (6 demo preset options: eth_dhcp/eth_http × M0/M33/RV32)
- `.gitignore` (`test-firmware/*.o`, `*.bin`, `__pycache__/`)
- this `agent.md` (§4.5, live-gateway + sweep notes)

Suggested message: `eth-http round 2: REQUEST opt54 fix, presets, sweep 59/60`
(body: §4.5 + live-gateway DORA + wifi_join_rv32 pre-existing flake).
`src/` untouched; `*.o/*.bin/__pycache__` excluded (gitignored now).

Prior round (already in `bc12b6f`):

1. ~~**Sweep wiring**~~ — DONE: `test-firmware/sweep_all.sh` has dead-peer
   `ETH MACRAW-OK` lines for all 3 HTTP guests (verified PASS x3).
2. ~~**Docs**~~ — DONE: `docs/NETWORKING.md` HTTP row ✅, NOT-done cell updated.
3. **WASMs** — `web/bramble.wasm*.wasm` are tracked and were rebuilt for
   eth-dhcp at HEAD. eth_http adds UF2s only (no `src/` change), so no WASM
   rebuild was done.
4. ~~**Commit bc12b6f**~~ — DONE (`src/` untouched, artifacts excluded).
5. ~~Full `./test-firmware/sweep_all.sh`~~ — DONE: 59/60 (1 pre-existing
   wifi flake); live `http_peer_test.py` re-verify after any generator
   edit via §6.

## 8. Scratch (not in repo, safe to delete)

`/tmp/dbg_fin.py` (post-ACK2 raw frame dump — proved ACK2+FIN both arrive),
`/tmp/dbg2_peer.py`, `/tmp/dbg_peer.py`, `/tmp/gdb_*.py`, `/tmp/m0*.log`,
`/tmp/fix*.log`, `/tmp/dbg*.log`, `/tmp/w5500.c.bak` (= HEAD `src/w5500.c`).
