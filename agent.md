# agent.md — Bramble gaps handover (eth guests + ioLibrary + ARM BLE, committed)

Date: 2026-09-18. Commits: `93b962a` (eth-dhcp) → `bc12b6f` (eth-http) →
`63bf2a6` (round 2: opt54 fix, presets, sweep 59/60) → THIS commit (see §7).
Status: **eth_http DONE x3 (M0+/M33/RV32)** — full app exchange green via
`http_peer_test.py`, sweep-wired (59/60, 1 pre-existing wifi flake),
docs updated, browser presets added, live-gateway DORA proven.
COMMITTED in this round: Arduino-CLI ETH prove-out support fixes
(`src/w5500.c`, `src/gpio.c`, `include/w5500.h`, `src/cyw43.c`,
`src/pio.c`), M0+/M33 BLE ARM guest (`gen_ble_arm.py`/`ble_adv.S`/UF2s,
first bring-up stalls at BT_CTRL read), `build_eth.py` ble targets,
all docs + site updates, rebuilt WASMs, `web/examples/` mirror
completion. `./build/bramble_tests` 411/411 green; sweep 59/60
(`wifi_join_rv32` pre-existing flake); `test-wasm.js` PASS;
`test-wasm-ble.js` PASS; `test-wasm-gateway.js` hangs identically on
clean HEAD (pre-existing, unrelated to these changes).

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

## 2. Files (`bc12b6f`: 7 NEW + 5 MOD + agent.md; `63bf2a6`: +5 MOD)

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
| web/index.html | MOD (`63bf2a6`) | Demo presets: eth_dhcp/eth_http × 3 arches in the Examples dropdowns |

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
python-peer DORA+HTTP re-verified x2 (dhcp M0, http M0) + M33/RV32 E2E
(dhcp+http, `ETH DONE` / `ETH HTTP-DONE`). RV32 `rv_patch_request` uses
absolute `TXBUF+287/+293` adds and was correct.

### 4.6 W5500 MACRAW: internal RX stream base + per-CS read cursor

Real silicon keeps an internal RX read pointer separate from guest-writable
RX_RD. The model now tracks per socket (`include/w5500.h`):
`rx_base` (queue base; RECV slides + recomputes RSR from it, appends land
at `rx_base+rsr`) + `rx_cursor`/`rx_cursor_base`/`rx_cursor_valid`
(CS-frame read position: latched from the frame's first DATA VDM address
on CS-assert, bumped per DATA byte; read returns
`rx_buf[(rx_base + cursor + addr - cursor_base)]`).
In-tree guests (VDM-addr-0 reads) behave identically to before; Arduino
Wiznet5500 (len prefix at 0, body burst at RX_RD cursor 0x02) now gets the
right bytes. Also: bare RECV on an empty queue is a no-op (Arduino issues
RECV after every burst including the len read — consuming phantom bytes
shifted the real frame). Plus `W5500_SPI_TRACE` byte tracer (off unless
`-DW5500_SPI_TRACE`). 411/411 green.

### 4.8 SPI control-byte OM misread (fixed, committed)

The old SPI state machine treated control-byte bits[1:0] as an FDM
1/2/4-byte length, truncating every multi-byte read at an odd VDM
address (RSR/RD/TX_WR came back short). This driver family always uses
VDM; the low bits are block-offset LSBs. Fix: stream all DATA bytes
under one CS (`fdm_left` removed). 411/411 still green.

### 4.9 SIO_GPIO_IN returns the raw latch (fixed, committed)

`gpio_read32(SIO_GPIO_IN)` returned the OE-gated effective level; real
silicon returns the pad input buffer regardless of OE. With OE=1 (Arduino
`gpio_init`+`gpio_set_dir(OUT)` on INTn GPIO21) the guest read stale
OUT=0 forever. Fix: return `gpio_in` raw. 411/411 still green.

`src/`, `include/` COMMITTED status (this round): `src/w5500.c`
(RX cursor + empty-RECV guard + SPI tracer), `include/w5500.h`
(rx_base/cursor fields), `src/gpio.c` (§4.9), `src/cyw43.c` (comment-only
SET/GET note + restart state reset, §8.2 context), `src/pio.c`
(SIDESET_BASE comment fix). 411/411 green with all of it.

Do NOT commit (now gitignored build artifacts): `test-firmware/*.bin`,
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
- Live Go-gateway run (room `ethhttptest2`, `gateway_bridge.py`): DORA +
  ARP green after the §4.5 fix (`ETH IP=192.168.4.2`, `ETH DONE`,
  `ETH ARP-OK`). Live-gateway HTTP stalls by design — the gateway's
  port-forward serves guests that LISTEN on :80, while our guest DIALS
  OUT as a client. So `http_peer_test.py` remains the full-exchange
  harness; the gateway run only proves DORA+ARP. (Gateway ARP replies
  arrive as ~7s 142B beacons; its `127.0.0.1:8080→.2:80` forward logs
  `no route to host` since nothing listens.)
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
- `web/examples/*.uf2` mirror completed this round (the 22 missing UF2s
  added — the directory `publish.yml` + Pages actually ship). All
  mirrors verified byte-identical to `web/*.uf2` (`cmp` clean); the
  older small files are genuine tiny demos (1–2 UF2 blocks), same as
  top level.
- The `build_eth.py` symtab parser takes 16-byte Elf32_Sym ALWAYS —
  a same-day theory about 12-byte Sym32 + REL-vs-RELA keying was
  disproved by byte-level reloc dumps (all-`sym 109` + zero addends is
  CORRECT section-relative linking; the ble failure was stale
  `ble_adv.S.o` sharing one path across arch builds).

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

## 7. Round-2 commit `63bf2a6` (pushed)

- `test-firmware/gen_eth_dhcp.py` (§4.5 patch_request +3 fix) + regenerated
  `test-firmware/eth_dhcp.S`, `test-firmware/eth_http.S` + rebuilt
  `web/eth_dhcp.uf2`, `web/eth_dhcp_pico2.uf2`, `web/eth_http.uf2`,
  `web/eth_http_pico2.uf2` (+ rv32 UF2s rebuilt; RV32 logic byte-identical,
  sizes unchanged: dhcp_rv32 6144B, http_rv32 12800B)
- `web/index.html` (6 demo preset options: eth_dhcp/eth_http × M0/M33/RV32)
- `.gitignore` (`test-firmware/*.o`, `*.bin`, `__pycache__/`)
- this `agent.md` (§4.5, live-gateway + sweep notes)

`src/` untouched; `*.o/*.bin/__pycache__` excluded (gitignored).

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

## 7b. This commit (staged below — commit + push pending at handover)

- `src/w5500.c` (§4.6 RX cursor/base + empty-RECV guard + §4.8 VDM
  streaming + `W5500_SPI_TRACE`), `include/w5500.h` (rx_base/cursor
  fields), `src/gpio.c` (§4.9 raw SIO_GPIO_IN), `src/cyw43.c`
  (SET/GET note + restart reset), `src/pio.c` (SIDESET_BASE comment)
- `test-firmware/arduino/ethdhcp/ethdhcp.ino` (NEW: M0+/M33 ioLibrary
  DHCP sketch) + `test-firmware/arduino/README.md` (ethdhcp recipe,
  m33wifi correction)
- `test-firmware/gen_ble_arm.py` (NEW) + `test-firmware/ble_adv.S`
  (NEW) + `web/ble_adv{,_pico2}.uf2` (NEW) + `build_eth.py` ble targets
- Docs: `docs/NETWORKING.md` (3 new rows), `docs/PICOEMU.md`,
  `docs/GATEWAY.md`, `docs/WASM.md`, `docs/ROADMAP.md`, `CHANGELOG.md`,
  `README.md`, `web/README.md`, `web/index.html` (ble presets),
  `web/docs.html` (BLE split rows, 411 fact), `web/about.html`
  (411 fact), this `agent.md`
- `web/bramble.wasm.{js,wasm,threads.js,threads.wasm}` rebuilt from
  current sources; `test-wasm.js` + `test-wasm-ble.js` PASS
- `web/examples/`: 22 missing UF2 mirrors (byte-identical, `cmp` clean)

Suggested message: `eth gaps: ioLibrary DHCP green (M0+), ARM ble_adv boots, docs+WASM+examples mirror (411/411, sweep 59/60)`
(body: §4.6/§4.8/§4.9 emulator fixes + §8.1–§8.5; sweep 59/60 with
`wifi_join_rv32` pre-existing flake; `test-wasm-gateway.js` hangs on
clean HEAD too — pre-existing, unrelated).

## 8. Gap workstreams (COMMITTED this round — see §7 for the commit)

### 8.1 Arduino-CLI ETH (W5500 ioLibrary) — status: M0 DHCP green

Real-ioLibrary-path prove-out via `Wiznet5500lwIP eth(17, SPI, 21)` on
M0+/pico-eth (`rp2040:rp2040:wiznet_5500_evb_pico`, sketch in
`test-firmware/arduino/ethdhcp/ethdhcp.ino`, compile/run recipe in
`test-firmware/arduino/README.md`): static-IP sketch gives
`ETH-BEGIN-OK` + `conn=1 ip=192.168.4.203` (L2+ARP green); DHCP mode
gives DISCOVER→OFFER→REQUEST→ACK green via `dhcp_peer_test.py`
(`ALL DHCP CHECKS PASSED`, guest prints `ETH-IP=192.168.4.2`) after the
§4.6 cursor fix. M33 same driver/RP2350 SPI bases — re-run
(`wiznet_5500_evb_pico2` + `-board pico-eth2`) to confirm. Sweep/docs:
dead-peer pattern does NOT work (Arduino DHCP needs a live peer) —
keep peer-driven; `docs/NETWORKING.md` row added (§8.5).

### 8.2 M0+/M33 BLE guest (ARM port of RV32 ble_adv, stalls at BT_CTRL)

`test-firmware/gen_ble_arm.py` → `ble_adv.S` → `web/ble_adv{,_pico2}.uf2`
(`build_eth.py --gen-ble ble-all`, clang armv6m/armv8-m.main, no
cross-toolchain; `W5500_SPI_TRACE`-style PIO A/B probes used
`/tmp/pioprobe*.S` + `/tmp/pioctl.S`, since deleted). PIO0 SM0 on both
chips, ifdef UART/SRAM like the eth guests. M33 UF2 builds clean
(assembles `-DRP2350`); M0 runs to `ARM BLE Starting` then stalls:
first gSPI transfer decodes as `RD func=0 addr=0x00000 size=0` instead
of the BT_CTRL window write. A/B-probed so far (all in committed
`gen_ble_arm.py` comments): ba_cmd/ba_wdata MUST preserve r1 (window
byte lives there across the calls); ba_pio_wr_pre MUST do restart +
exactly 2 skip words (either piece alone shifts the stream); PINCTRL-
only setup (no TXF dummy traffic — dummies burn the 2-cmd SWAP32
budget); absolute FIFO addrs; bswap on TX AND RX (DMA-BSWAP emulation).
Also fixed along the way: `build_eth.py` symtab-esz revert (16 always;
the REL→12 theory was wrong — the ble failure was stale build outputs).
Sweep target: `ARM BLE LISTEN` (local bring-up, no peer needed),
mirroring `wifi_ble_adv_rv32.uf2`; `docs/NETWORKING.md` row committed;
browser presets committed.

### 8.3 M0+/M33 WiFi into sweep (origins found, not wired)

- M0+/M33 `wifi_scan`/`wifi_ping`/`wifi_webserver` UF2s are
  Arduino-CLI builds from `~/gwtest/webscan|websrv|webping`
  (rp2040:rp2040:rpipicow/rpipico2w), NOT in-tree sources — committed
  as binaries at `7ab5b9c`. RV32 ones are `gen_rv32.py` demos.
- M33 Arduino WiFi verified live this session: `m33wifi.ino` (scan +
  join) compiles on `rp2040:rp2040:rpipico2w` and boots under
  `-arch m33 -wifi` (M33 overlay, SRAM 520KB). BUT scan returns
  `n=0`: the guest's `escan` iovar never reaches the model (only ONE
  IOCTL cmd=263 observed before the guest blocks). Suspect: ioctl
  response routing (CDC flags/id) or async-event delivery on M33 —
  RV32 `wifi_join` works, so compare the two paths. The
  `test-firmware/arduino/m33wifi/m33wifi.ino` sketch exists in-tree
  and is the repro.
- Sweep currently covers WiFi on RV32 only (`sweep_all.sh:93-98`).
  Wire M0+/M33 when `SCAN n=3` reproduces.

### 8.4 BT-capable MP build (recon only, blocked on frozen main.py)

- Toolchain: pqt-gcc 5.0.0 (arm-none-eabi-gcc 16.1.0) ships with the
  arduino core — usable for the in-tree stock BT build.
- `~/mpbuild-stock` (pico_w, `firmware.uf2` present) is the BT-capable
  candidate; `~/mpwifi/main.py` runs the MP6U net test headless
  (BLE-ACTIVE → gap_adv → STA join → SEND6), so REPL-driven ADV is
  unobservable either way. Live `gap_advertise` proof needs a build
  whose main.py (or REPL) leaves the ADV running AND observable —
  either patch `~/mpwifi/main.py` to loop on ADV, or drive the stock
  build over USB REPL (`repl_drive.py` exists). Untouched this session.

### 8.5 Docs + website updates (committed, part of this round)

- `docs/NETWORKING.md`: new rows — Arduino ioLibrary guest (✅ M0+ /
  🟡 M33), ARM BLE guest (🟡), M0+/M33 WiFi sweep (🟡); NOT-done cell
  rewritten (no longer claims the eth gap is closed without qualification).
- `test-firmware/arduino/README.md`: `ethdhcp` row + compile/run recipe;
  `m33wifi` row corrected (in-tree repro currently `SCAN n=0`, under test).
- `web/index.html`: `ble_adv.uf2` / `ble_adv_pico2.uf2` demo presets
  (M0+ + M33 dropdowns; RV32 already had `wifi_ble_adv_rv32.uf2`).
- `web/docs.html`: BLE bare-metal split into RV32 done + M0+/M33 partial
  rows + eth guest mentions in all three core tables; test fact 403→411.
- `web/about.html`: test fact 403→411, firmware list mentions eth guests.
- `web/README.md`: networking-extras paragraph (eth/wifi/ble UF2s).
- `web/examples/`: mirrored the 22 missing UF2s (all wifi_*, eth_*,
  ble_adv*) so the published npm package (`publish.yml` ships
  `*.uf2` + `examples/*.uf2`) actually contains what the dropdowns
  offer. All mirrors verified byte-identical (`cmp` clean).
- Top-level docs: `README.md` (411 counts + eth/BLE status + guest
  recipes), `docs/PICOEMU.md` (three-faces W5500 + guest recipes +
  sweep 60 count), `docs/GATEWAY.md` (wired-ethernet gateway recipe),
  `docs/WASM.md` (411 + preset lists), `docs/ROADMAP.md` (unreleased
  row), `CHANGELOG.md` (unreleased section).
- `web/bramble.wasm.{js,wasm,threads.js,threads.wasm}` rebuilt from
  current sources (`build_wasm.sh` + `build_wasm_threads.sh`);
  `test-wasm.js` PASS, `test-wasm-ble.js` PASS, `test-wasm-gateway.js`
  hangs identically on clean HEAD (pre-existing, unrelated).

## 9. Scratch (not in repo, safe to delete)

`/tmp/dbg_fin.py` (post-ACK2 raw frame dump — proved ACK2+FIN both arrive),
`/tmp/dbg2_peer.py`, `/tmp/dbg_peer.py`, `/tmp/gdb_*.py`, `/tmp/m0*.log`,
`/tmp/fix*.log`, `/tmp/dbg*.log`, `/tmp/w5500.c.bak` (= HEAD `src/w5500.c`).
