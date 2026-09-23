# agent.md — Bramble gaps handover (M33 Arduino E2E DONE, uncommitted)

Date: 2026-09-22. Commits: `f2a57eb` (eth gaps) → `83cfe07` (9 rows) →
`e235008` (macraw+HOST_WAKE) → `6cd4c4b` (RECV refresh) → `830fb3c`
(RV32 poll) → `79ed0a4` (MACRAW cursor) → `6b698f7` (M33 IRQ map) →
THIS round (see §7b).
Status: **sweep 62/62** — `wifi_join_rv32` was NOT a flake (root-caused:
RV32 `rv_translate_shared_addr` mapped RP2350 PLL_SYS `0x40050000` to
RP2040 PWM `0x40050000`, PWM handler won, PLL CS never LOCKED, SDK
`pll_init` spun at `PC=0x10001C06` — fixed with an RV32 clock-domain
bypass in `rv_membus.c`, `test_rv_clocks_pll_sys` added).
`./build/bramble_tests` **431/431**; WASM rebuilt (`326K` + threads,
`test-wasm.js` + `test-wasm-ble.js` PASS); Arduino M33 E2E + in-tree
DORA x3 re-verified green.
Remaining: MP `import bluetooth` HCI work.

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

Round A (`f2a57eb`, pushed): ioLibrary fixes + ARM BLE boots + docs/WASM.
Round B (THIS round, uncommitted until §7c): all 9 rows + BLE LISTEN fix:

- `src/adc.c` / `include/adc.h` (9-mux, RROBIN-9, FIFO-8, THRESH-27, temp
  on 4+8, depth gate), `src/pwm.c` / `include/pwm.h` (12 slices, IRQ1
  block, RP2350 base route, mode-gated layouts), `src/dma.c` untouched
  (16ch already modeled — test only), `test-firmware/sweep_all.sh`
  (`run_ble` + 2 lines)
- `src/devtools.c` / `include/devtools.h` (HSTX serializer + TMDS,
  TRNG EHR stream, SHA-256 digest), `src/membus.c` (HSTX FIFO block +
  TRNG writes), `src/main.c` + `src/bramble_wasm.c` (init calls)
- `src/nvic.c` / `include/nvic.h` (SAU/MPU/faults/TT), `src/thumb32.c`
  (DSP scalar + MVE vectors + VFP/DCP dual-core comment),
  `src/cpu.c` + `include/emulator.h` (VFP/DCP/VPR context save),
  `src/rp2350_rv/rv_cpu.c` + `include/rp2350_rv/rv_cpu.h` (Zfinx + fcsr)
- `test-firmware/gen_ble_arm.py` (dummy swaps restored, ba_bswap fix,
  vector+1 fix) + regenerated `ble_adv.S` + rebuilt
  `web/ble_adv{,_pico2}.uf2` + `web/examples/` mirrors
- `tests/test_suite.c` (+15 tests, `devtools.h` + `rp2350_memmap.h`
  includes, `reset_cpu` inits)
- Docs: `CHANGELOG.md` (new Unreleased section), `docs/ROADMAP.md`
  (new Current State + demoted eth row), `docs/NETWORKING.md` (BLE ✅),
  `README.md` + `docs/PICOEMU.md` + `docs/WASM.md` (425 counts),
  `web/docs.html` (all 9 rows done), `web/about.html` (425 + TZ/DSP/MVE
  prose), `web/README.md` (sweep-locked extras), this `agent.md`
- `web/bramble.wasm.*` rebuilt from current sources (see §7c)

Suggested message: `all 9 support rows done: B-package ADC/PWM/DMA, HSTX/TRNG/SHA-256, SAU/MPU, DSP/MVE, Zfinx, ARM BLE LISTEN (425/426, sweep 61/62)`
(body: per-row files + tests above; BLE triple-fix; sweep 61/62 with
`wifi_join_rv32` pre-existing flake; tests 425/426 with w5500-macraw
length-prefix pre-existing flake identical on clean HEAD;
`test-wasm-gateway.js` hangs on clean HEAD too — pre-existing).

## 7c. WASM rebuild + verify checklist (do before commit)

1. `./build_wasm.sh && ./build_wasm_threads.sh` (emsdk) — `src/` changed
   a lot this round (devtools/nvic/thumb32/rv_cpu/cpu/membus/pwm/adc).
2. `node test-wasm.js` PASS, `node test-wasm-ble.js` PASS.
3. Full `./test-firmware/sweep_all.sh build` → 61/62 (only
   `wifi_join_rv32` flake).
4. Stage + commit + push (message above).

## 8. Gap workstreams (COMMITTED this round — see §7 for the commit)

### 8.1 Arduino-CLI ETH (W5500 ioLibrary) — status: M0 + M33 DHCP green

Real-ioLibrary-path prove-out via `Wiznet5500lwIP eth(17, SPI, 21)` on
M0+/pico-eth (`rp2040:rp2040:wiznet_5500_evb_pico`, sketch in
`test-firmware/arduino/ethdhcp/ethdhcp.ino`, compile/run recipe in
`test-firmware/arduino/README.md`): static-IP sketch gives
`ETH-BEGIN-OK` + `conn=1 ip=192.168.4.203` (L2+ARP green); DHCP mode
gives DISCOVER→OFFER→REQUEST→ACK green via `dhcp_peer_test.py`
(`ALL DHCP CHECKS PASSED`, guest prints `ETH-IP=192.168.4.2`) after the
§4.6 cursor fix. **M33 GREEN 2026-09-22** (`ethdhcp_m33`, `Serial1`,
`wiznet_5500_evb_pico2`, `-board pico-eth2 -arch m33`): full DORA after
the `6b698f7` RP2350-map fixes (IO_BANK0 base routing + IRQ map 13→21;
build `arduino-cli compile --fqbn rp2040:rp2040:wiznet_5500_evb_pico2
--output-dir /tmp/ethdhcp_m33 .../ethdhcp_m33.ino`): peer `ALL DHCP
CHECKS PASSED` (`chaddr=020123520001`) + guest `conn=1 ip=192.168.4.2`.
Sweep/docs: dead-peer pattern does NOT work (Arduino DHCP needs a live
peer) — keep peer-driven; `docs/NETWORKING.md` row updated.

### 8.2 M0+/M33 BLE guest (ARM port of RV32 ble_adv — FIXED, LISTEN x2)

`test-firmware/gen_ble_arm.py` → `ble_adv.S` → `web/ble_adv{,_pico2}.uf2`
(`build_eth.py --gen-ble ble-all`, clang armv6m/armv8-m.main, no
cross-toolchain). Both cores now print `BT-CTRL 01000100`, `RAM-BASE
001C0000`, `HOST-READY`, `RESET-OK`, `ADV-OK`, `LISTEN`; sweep-locked via
new `run_ble` helper in `sweep_all.sh` (62 lines, 61/62 with the
pre-existing `wifi_join_rv32` flake). Root-caused three stacked guest
bugs (NOT emulator bugs — `/tmp/ble_unit*.c` probes proved the model
path correct throughout):
1. `ba_bswap` never swapped the middle two bytes (byte1′/byte2′
   identity) — every window/frame command garbled; fixed one `lsrs`
   (`#16` → `#8`).
2. The two dummy swap-read rounds had been deleted as "burning the
   budget" — backwards: the model grants SWAP32 to the first two TXF
   commands unconditionally, so the dummies must come first to absorb
   them; restored verbatim from the RV32 rhythm.
3. `.word reset_handler + 1` double-set the Thumb bit (`build_eth.py`
   ABS32 already adds `st_value`, which has bit0 set) — reset vector was
   `…141+1`, execution started 2 bytes high and died on the first nested
   `bl` (pop `{r0-r1,pc}` → 0 → ROM slide → HardFault loop). Now plain
   `.word reset_handler`.
Earlier (still-valid) notes in `gen_ble_arm.py` comments: ba_cmd/ba_wdata
MUST preserve r1; ba_pio_wr_pre MUST do restart + exactly 2 skip words;
absolute FIFO addrs; bswap on TX AND RX; `build_eth.py` symtab-esz 16
always (the REL→12 theory was wrong — that failure was stale outputs).

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

- `docs/NETWORKING.md`: Arduino ioLibrary guest row now ✅ M0+ / ✅ M33
  (was 🟡 M33 pump-gap — closed by `6b698f7`); NOT-done cell now only
  MP `gap_advertise` + `wifi_join_rv32` flake.
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
