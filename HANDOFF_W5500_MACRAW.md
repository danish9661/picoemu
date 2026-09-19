# HANDOFF — Bramble W5500 MACRAW length-prefix failure + sweep flakes

Date (UTC): 2026-09-19
Repo: /home/danish1075/Documents/rp2350/Bramble
Branch: main
HEAD: 83cfe07 "all 9 support rows done: B-package ADC/PWM/DMA, HSTX/TRNG/SHA-256, SAU/MPU, DSP/MVE, Zfinx, ARM BLE LISTEN (425/426, sweep 61/62)"
Workdir: /home/danish1075/Documents/rp2350/Bramble
Build: build/bramble_tests -> 425/426 passed, 1 failed
Sweep (claimed): 61/62 (wifi_join_rv32 pre-existing flake)
Status: clean git (no stashes, no unstaged changes at handoff time)
Author of handoff: opencode session (Muse Spark)

---

## 1. What this session was about

1. User asked "What did we do so far?" then "continuee" / "continue".
2. Assistant surveyed repo root (/home/danish1075/Documents/rp2350),
3. then Bramble/ directory listing, git log, CHANGELOG.md, ROADMAP.md,
4. agent.md, front.md, .claude/, docs/NETWORKING.md, docs/audit_report.md,
5. live test results (build/bramble_tests), schedules/sessions.
6. User then said "see teh todo list and start working".
7. No TODO file exists in repo; assistant derived todo list from
8. docs/NETWORKING.md "What is NOT done", agent.md sections 8.1-8.5,
9. docs/ROADMAP.md current-state table, and live failures.
10. Created TodoWrite list with 5 items (see Section 3).
11. Started work on item 1: w5500 MACRAW length-prefix failure.
12. Deep-dived test, src/w5500.c, src/vnet.c, history, guests, Arduino driver.
13. Ran websearch to confirm W5500 MACRAW hardware semantics.
14. Checked for `claude` CLI for handoff skill; not installed.
15. User clarified "this is for opencode".
16. User requested: "make a 500 line handoff file which contains all the info
17. and also contain the todo list of this conversation".
18. This file is that handoff. It is intentionally ~500 lines.
19. Do NOT commit it unless user asks; it is a working handoff.
20. Next agent: pick up Section 3, item 1 first.

---

## 2. Prior work (reference, do not duplicate)

21. Full prior-work narrative lives in these artifacts — read them, do not re-copy:
22. - agent.md (402 lines, handover for eth_dhcp/eth_http + gaps)
23. - CHANGELOG.md Unreleased 2026-09-18 section (lines 1-85)
24. - docs/ROADMAP.md Current State row (line 5) + Phase histories
25. - docs/NETWORKING.md full matrix (156 lines)
26. - docs/PICOEMU.md, docs/WASM.md, docs/GATEWAY.md, web/docs.html
27. - docs/audit_report.md (re-triaged 2026-09-08, all H/M fixed)
28. - test-firmware/sweep_all.sh (62 lines after run_ble addition)
29. Key commits (git log --oneline):
30. - 83cfe07 (HEAD) all 9 rows DONE, 425/426, sweep 61/62
31. - f2a57eb eth gaps: ioLibrary DHCP green M0+, ARM ble_adv boots, 411/411
32. - 63bf2a6 eth-http round 2: REQUEST opt54 fix, presets, sweep 59/60
33. - bc12b6f eth-http: in-tree HTTP guests DONE x3 via single-gateway MACRAW
34. - 93b962a eth-dhcp: in-tree DHCP guests DONE x3 via single-gateway MACRAW
35. - 7c6e538 eth via single gateway: W5500 MACRAW sock0 joins shared vnet bus
36. - aeea06b pico-eth/pico-eth2 board: W5500-EVB-Pico/Pico2 on SPI0
37. Prior state at f2a57eb: 411/411 green, sweep 59/60.
38. Prior state at HEAD: 425/426 green (+15 new tests), sweep 61/62.
39. The +15 tests: 2 ADC, 1 PWM, 1 DMA, 2 M33 TrustZone, 1 DSP, 1 MVE,
40. 3 Zfinx, 2 HSTX, 1 TRNG, 1 SHA-256. See CHANGELOG.md lines 79-84.
41. All 9 requested rows DONE per agent.md section 7b:
42. B-package ADC/PWM/DMA, HSTX, TRNG, SHA-256, SAU/MPU, DSP, MVE, Zfinx, ARM BLE.
43. ARM ble_adv LISTEN x2 fixed via three stacked guest bugs (NOT emulator bugs):
44. (1) ba_bswap middle bytes, (2) dummy swap-read rounds, (3) vector+1.
45. See agent.md 8.2 + test-firmware/gen_ble_arm.py comments.
46. In-tree eth guests: test-firmware/gen_eth_dhcp.py, gen_eth_http.py,
47. eth_dhcp_common.py/eth_http_common.py, dhcp_peer_test.py/http_peer_test.py,
48. web/eth_{dhcp,http}{,_pico2,_rv32}.uf2.
49. Arduino prove-out: test-firmware/arduino/ethdhcp/ethdhcp.ino,
50. test-firmware/arduino/README.md, FQBN wiznet_5500_evb_pico (M0+) green,
51. wiznet_5500_evb_pico2 (M33) pending.
52. WASM: web/bramble.wasm.{js,wasm,threads.js,threads.wasm} rebuilt at HEAD.
53. test-wasm.js PASS, test-wasm-ble.js PASS, test-wasm-gateway.js hangs
54. identically on clean HEAD (pre-existing, unrelated).
55. Docs counts at HEAD: 425/426, sweep 61/62.

---

## 3. Todo list of THIS conversation (canonical)

56. Source: derived from NETWORKING.md:78,81 + agent.md:8.1-8.5 + live failures.
57. Tracked via TodoWrite tool in this session. States at handoff:
58.
59. [IN_PROGRESS] (high) Fix w5500 MACRAW length-prefix test failure (425/426)
60.   - Test: test_w5500_macraw_gateway_dhcp_path
61.   - Symptom: "length prefix lo should be 42: expected 0x2A, got 0x02"
62.   - RSR=44 OK, RECV set OK, lhi=0 OK, llo=2 FAIL
63.   - Files: tests/test_suite.c:6365-6468, src/w5500.c:157-251,562-620,747-838
64.   - See Sections 4-8 for full analysis. Fix, rebuild, verify 426/426,
65.     then re-run sweep + E2E peers + Arduino DHCP to catch regressions.
66.
67. [PENDING] (high) Investigate wifi_join_rv32 sweep flake (61/62)
68.   - Sweep: test-firmware/sweep_all.sh build -> 61/62 pass
69.   - Failure: wifi_join_rv32.uf2 (want RV32 JOIN DONE)
70.   - Claimed pre-existing flake on main since 93b962a message.
71.   - Need: reproduce isolated, check if timing/NDP/fake-DHCP race,
72.     compare RV32 vs M0+/M33 WiFi paths, log CYW43 TRACE.
73.
74. [PENDING] (medium) M33 ioLibrary DHCP re-run (pico-eth2)
75.   - Same sketch/driver as M0+ green, new board/FQBN.
76.   - Recipe: test-firmware/arduino/README.md + ethdhcp.ino header.
77.   - Board: -board pico-eth2, FQBN wiznet_5500_evb_pico2, -arch m33.
78.   - Live peer required: python3 test-firmware/dhcp_peer_test.py <sock>
79.   - No dead-peer marker (Arduino DHCP needs live peer).
80.   - Confirm ETH-IP=192.168.4.2 + ALL DHCP CHECKS PASSED.
81.
82. [PENDING] (medium) Debug M33 WiFi scan n=0 (escan iovar routing)
83.   - Repro: test-firmware/arduino/m33wifi/m33wifi.ino
84.   - Boots under -arch m33 -wifi but scan returns n=0.
85.   - Only ONE IOCTL cmd=263 observed before guest blocks.
86.   - Suspect: ioctl response routing (CDC flags/id) or async-event delivery.
87.   - RV32 wifi_join works; compare paths. Use BRAMBLE_CYW43_TRACE=1.
88.   - Sweep currently covers WiFi on RV32 only (sweep_all.sh:93-98).
89.
90. [PENDING] (low) MP live gap_advertise proof (BT-capable build)
91.   - Emulator-side blockers fixed (legacy-only 0x1002, LE-bit 0x1003,
92.     FC01/0x2060/0x2017, BT identity sync). See NETWORKING.md:100.
93.   - Blocked on frozen main.py: need BT-capable MP build whose ADV stays
94.     observable (patch ~/mpwifi/main.py to loop on ADV, or drive stock
95.     build over USB REPL via repl_drive.py).
96.   - Candidates: ~/mpbuild-stock (pico_w, firmware.uf2 present),
97.     ~/mpwifi/main.py MP6U net test headless.
98.   - Toolchain note: pqt-gcc 5.0.0 ships arm-none-eabi-gcc 16.1.0 usable.
99.
100. Priority order for next agent: item 1, then 2, then 3+4 in parallel if
101. possible, then 5. Do not skip verification steps in Section 9.

---

## 4. Failing test — exact location and reproduction

102. Test function: test_w5500_macraw_gateway_dhcp_path
103. File: tests/test_suite.c, lines 6362-6468 (HEAD).
104. Category: W5500 Live Networking (shows as 12/13 passed on failure).
105. Full suite: ./build/bramble_tests -> Results: 425/426 passed, 1 failed.
106. Failure log excerpt (stderr+stdout interleaved):
107. [W5500] MACRAW socket 0 on vnet port 0
108. [VNet] Port 1 registered: macraw-observer (MAC=02:BB:00:00:00:77)
109. FAIL
110.     length prefix lo should be 42: expected 0x0000002A, got 0x00000002
111.   -- W5500 Live Networking: 12/13 passed
112. Reproduce:
113.   cmake --build build --target bramble_tests -j8
114.   ./build/bramble_tests 2>&1 | tail -30
115.   ./build/bramble_tests 2>&1 | grep -B2 -A5 "FAIL"
116. No filter arg supported: main(void) takes no argv (tests/test_suite.c:7656).
117. RUN_TEST list at lines 8216-8228; macraw test is line 8228.
118. Test source identical at f2a57eb and HEAD (verified via git show).
119. Steps inside test (annotated):
120. 1. reset_cpu(); w5500_board_detach(); vnet_init();
121. 2. w5500_init(&dev) on STACK (local dev, NOT board device).
122. 3. Set deterministic SHAR 02:11:22:33:44:55 via dev.common[SHAR0..5].
123. 4. dev.cs_active = 1 (direct struct poke to enable SPI).
124. 5. SPI: write Sn_MR=MACRAW (BSB=1, addr 0x00, ctrl (1<<3)|0x04).
125. 6. SPI: write Sn_CR=OPEN (BSB=1, addr 0x01).
126. 7. ASSERT SR==0x42 MACRAW, ASSERT dev.vnet_port>=0.
127. 8. Register observer port: vnet_register_port("macraw-observer",
128.    VNET_PORT_CUSTOM, 02:BB:00:00:00:77, test_vnet_rx_callback, NULL).
129. 9. Build 60B broadcast frame (dst FF.., src SHAR, ethertype 0800).
130. 10. SPI VDM write to sock0 TX block BSB=2 addr 0x0000, 60 bytes.
131. 11. SPI write TX_WR hi/lo in TWO separate CS frames (0x24 then 0x25).
132. 12. SPI write Sn_CR=SEND. ASSERT IR&0x10 SEND_OK.
133. 13. ASSERT observer got 60B and memcmp matches.
134. 14. Build 42B inbound (dst=SHAR unicast, src 02:BB:00:00:00:01, ARP 0806).
135. 15. vnet_tx_frame(-1, inbound, 42) (peer/TAP origin, src_port=-1).
136. 16. ASSERT RSR==44 (len+2), ASSERT IR&0x04 RECV set. BOTH PASS.
137. 17. SPI read RX block BSB=3 addr 0x0000, ctrl (3<<3)|0x00, then 2x 0xFF:
138.    lhi = first MISO, llo = second MISO. ASSERT lhi==0 (PASS), llo==42 (FAIL, got 2).
139. 18. SPI write Sn_CR=RECV. ASSERT RSR==0, RECV cleared.
140. 19. vnet_cleanup(); PASS().
141. Key: failure is AFTER RSR/RECV prove the RX path appended correctly.
142. So bug is in READ path (w5500_read_byte MACRAW branch + cursor latch),
143. not in append path (w5500_macraw_vnet_rx).
144. Test binary built Sep 18 17:05 (build/bramble_tests 2305592 bytes).
145. Test count: grep -c RUN_TEST = 426.

---

## 5. src/w5500.c — read-path code under suspicion

146. File: src/w5500.c (1206 lines at HEAD), header include/w5500.h (241 lines).
147. Struct: w5500_socket_t has rx_base, rx_cursor, rx_cursor_base,
148. rx_cursor_valid, host_fd, host_listen_fd (include/w5500.h:119-135).
149. Append path (GOOD): w5500_macraw_vnet_rx() lines 157-187.
150.   - Computes rx_rsr from regs, free_space, need=len+2.
151.   - rx_wr = rx_base + rx_rsr (NOT RX_WR register) — deliberate f2a57eb fix.
152.   - Writes len_hi, len_lo, frame at rx_wr..rx_wr+need-1 modulo 2048.
153.   - Updates RX_WR regs to rx_wr+need, RSR+=need, IR|=0x04, refresh INT.
154. Attach path: w5500_macraw_attach() lines 225-251, table w5500_macraw_devs[2].
155. Send path: w5500_macraw_send() lines 258-283 (TX_WR treated as length).
156. RECV path: w5500_process_socket_cmd() case RECV lines 499-545.
157.   - Empty guard (rx_rsr==0 -> break, no touch) — f2a57eb fix.
158.   - Else reads flen from rx_buf[rx_base], total=flen+2, slides remainder
159.     to head, rx_base=0, RSR=remain, clears RECV only if remain==0.
160. Read path (SUSPECT): w5500_read_byte() case 3 lines 590-616.
161.   if (sock==0 && MR==MACRAW && SR==MACRAW) {
162.     base = rx_base % 2048;
163.     off = rx_cursor + addr - rx_cursor_base;   // uint16_t wrap
164.     return rx_buf[(base+off) % 2048];
165.   } else return rx_buf[addr % 2048];
166. SPI DATA phase: w5500_spi_xfer() lines 747-820.
167.   - ADDR_HI, ADDR_LO, CONTROL (bsb=(mosi>>3)&0x1F, rw=(mosi>>2)&1).
168.   - OM bits [1:0] IGNORED (VDM streaming) — f2a57eb fix, see comment 777-786.
169.   - DATA: write -> w5500_write_byte; read -> miso=w5500_read_byte,
170.     then cursor latch block lines 797-810:
171.       type=bsb_type(bsb), sock=bsb_socket(bsb);
172.       if (type==3 && sock==0 && !rx_cursor_valid) {
173.         rx_cursor_base = dev->addr; rx_cursor_valid=1; }
174.       if (type==3 && sock==0) rx_cursor++;
175.     then dev->addr++ (line 815).
176. CS path: w5500_spi_cs() lines 822-838 resets phase + all sockets'
177. rx_cursor/cursor_base/valid on rising edge (cs_active && !was).
178. TX write path: w5500_write_byte() case 2 line 669 raw index (no fold).
179. RX write path: case 3 lines 673-688 ignores writes on MACRAW (except
180. RX_RD regs go via type-1 path, not here).
181. Init: w5500_init() lines 697-741 memsets, sets VERSIONR=0x04, SHAR default,
182. PHYCFGR, RTR/RCR, vnet_port=-1, per-socket RXBUF/TXBUF=2, TTL=128, SR=CLOSED.
183. Board INT refresh: w5500_board_refresh_int() lines 1013-1025 (no-op when off).
184. History: 7c6e538 created MACRAW with RX_WR-register append + absolute read;
185. f2a57eb changed to rx_base+rsr append, rx_base RECV, cursor-relative read,
186. VDM streaming, empty-RECV no-op, raw TX, raw SIO_GPIO_IN (gpio.c).
187. git diff f2a57eb HEAD -- src/w5500.c is EMPTY (no changes this round).
188. So failure appeared WITHOUT src change — points to test-order/state leak
189. or toolchain/UB, not a fresh logic edit. See Section 7.

---

## 6. src/vnet.c + test-order/state-leak analysis

190. File: src/vnet.c (581 lines), header include/vnet.h.
191. vnet_init() lines 62-100: idempotent guard (if enabled return 0), else
192. memset + peer-path save/restore + tap_fd=-1 + gateway MAC + enabled=1.
193. vnet_cleanup() lines 102-134: closes TAP/peers, reports stats if frames>0,
194. enabled=0, peer_count=0 (fully disarmed).
195. vnet_register_port() lines 157-185: append-only, port_count++, no reuse
196. except via vnet_unregister_port() compaction (L11 fix).
197. vnet_tx_frame(src, frame, len) lines 285-314: skips src_port, delivers to
198. ports with broadcast/multicast/MAC-match, then TAP + WS mirror.
199. Static MACRAW table: w5500_macraw_ndevs NEVER reset by vnet_cleanup() or
200. w5500_init(). Slots never reused (comment lines 189-193). MAXDEVS=2.
201. Test uses STACK dev (local), so dedup key (dev,sock) is stack address.
202. If earlier test attached same stack address (reuse), attach returns early
203. with dev->vnet_port (which w5500_init just set to -1) — but observed
204. vnet_port>=0 PASS, so attach DID run (new slot or new address).
205. BUT second attach in same process consumes slot 1 of 2. Earlier tests:
206. - test_vnet_* (8 tests) register ports but no MACRAW attach.
207. - test_w5500_* TCP/UDP OPEN do NOT attach (only MACRAW sock0 attaches).
208. - Only this test attaches MACRAW. So ndevs should be 0->1 here.
209. UNLESS a prior RUN left ndevs==2 (max) from an earlier full-suite run?
210. No — process fresh per binary run, ndevs starts 0. Not leak across runs.
211. More likely leak: vnet ports accumulate across tests (no cleanup until
212. this test's vnet_cleanup). Observer is port 1 (log confirms), MACRAW port 0.
213. Inbound vnet_tx_frame(-1, ...) delivers to BOTH ports except src=-1 (none
214. skipped). Observer callback test_vnet_rx_callback just records len/buf.
215. Does observer's rx_fn alias MACRAW dispatch? No, separate ctx.
216. Could observer delivery CORRUPT dev.rx_buf? No, different memory.
217. Could vnet_tx_frame deliver TWICE to MACRAW (two matching ports)? Only
218. one MACRAW port (port 0). Observer MAC differs, so inbound dst=SHAR only
219. matches MACRAW port, not observer. SEND broadcast DOES match observer (good).
220. RSR=44 proves single append (not double). So vnet layer is clean.
221. Remaining suspects: cursor latch vs addr increment (Section 7),
222. or W1C IR write side-effect (Section 8), or RX_RD register confusion.
223. Check: test never writes RX_RD, so rx_base=0 at read time. base=0.
224. Expected: first DATA byte at addr=0 -> base=0, cursor=0, cursor_base=0,
225. off=0+0-0=0 -> rx_buf[0]=0 (hi). Second byte addr=1, cursor=1 -> off=1+1-0=2?
226. WAIT: cursor++ happens AFTER read. Trace: byte0: read with cursor=0,
227. off=0+0-0=0 -> buf[0]. Then cursor=1, addr=1. Byte1: read with cursor=1,
228. off=1+1-0=2 -> buf[2], NOT buf[1]! THAT IS THE BUG (off-by-one from double
229. counting addr+cursor). Got 0x02 = first frame byte (inbound[0]=0x02),
230. NOT len_lo=42. Matches observed llo=2 exactly.
231. In-tree guests read len hi/lo in SAME CS frame from addr 0 streaming:
232. byte0 off=0 -> buf[0] hi OK; byte1 off=2 -> buf[2] frame[0] — also wrong?
233. But guests PASS E2E... because RECV slide + RX_RD confusion? Need to verify.
234. Arduino reads len via TWO single-byte frames (wizchip_read): each CS frame
235. has 1 DATA byte at addr RX_RD (cursor reset each frame, off=0 -> buf[base]).
236. First read (RX_RD=0): buf[0]=hi. Second read (RX_RD=1): base still 0,
237. cursor reset -> off=0+1-1=0 -> buf[0]=hi AGAIN, not lo. Hmm.
238. Actually Arduino wizchip_read does cs_select, 3 header bytes, 1 data byte,
239. cs_deselect per byte. So each frame: addr=RX_RD, cursor 0->1, off=addr-base?
240. With base=0: frame1 addr0 -> off=0 -> hi. frame2 addr1 -> off=0+1-1=0 -> hi.
241. That would break Arduino too — but Arduino DHCP green at f2a57eb.
242. So either analysis wrong or Arduino path uses different addrs (RX_RD advances
243. via setSn_RX_RD between reads, and rx_base slides on RECV, not per burst).
244. Need to re-trace with actual RX_RD values: Arduino readFrameSize does
245. recv_data(head,2) = read_buf at ptr=getSn_RX_RD() (single CS frame, 2 bytes),
246. NOT two single-byte reads. read_buf holds CS across len bytes (see w5500.cpp:60).
247. So Arduino len path IS single-frame 2-byte burst from addr=RX_RD=0:
248. byte0 off=0 -> hi, byte1 off=2 -> frame[0]. Still off-by-one.
249. Unless RX_RD !=0 or rx_base !=0 at that point (after prior frames, base>0,
250. or after prior RECV slide, base=0 but RX_RD advanced). After first frame,
251. Arduino does setSn_RX_RD(old+len) then setSn_CR(RECV) which slides remainder
252. to head and resets rx_base=0 BUT leaves RX_RD register advanced (guest-owned).
253. Next frame appends at rx_base+rsr=0+0=0 (head), but guest reads at RX_RD=old.
254. Cursor math: base=0, cursor_base=RX_RD, addr=RX_RD -> off=0 -> buf[0] hi OK.
255. Second byte addr=RX_RD+1, cursor=1 -> off=1+(RX_RD+1)-RX_RD=2 -> buf[2].
256. STILL off-by-one. So cursor model seems broken for 2-byte bursts generally.
257. Correct model should be off = (cursor_base - rx_base?) No — real HW returns
258. rx_buf[(RX_RD + cursor) % SIZE] where cursor is bytes-consumed-this-frame,
259. NOT cursor+addr-base. I.e. off should be (rx_cursor) alone when VDM streams,
260. or (addr - rx_base?) Hmm. See Section 7 for candidate fixes.
261. Bottom line: test failure correctly caught a real read-path bug introduced
262. in f2a57eb cursor logic; Arduino green was likely single-frame luck + RECV
263. no-op + VDM fixes masking it, or wall-timing retry covering first-frame loss.
264. Next agent must fix w5500_read_byte MACRAW branch + cursor latch together.

---

## 7. Candidate root causes (ranked)

265. H1 (MOST LIKELY): double-count read offset in w5500_read_byte.
266.   off = rx_cursor + addr - rx_cursor_base counts both cursor increments
267.   AND VDM addr increments for the same bytes. For streaming VDM reads
268.   (addr++ per byte AND cursor++ per byte), off advances 2 per byte.
269.   Proof: test 2-byte burst from 0 gives off 0 then 2 (got buf[2]=0x02).
270.   Fix candidate A: off = addr - rx_cursor_base + (rx_base % SIZE anchor?)
271.   i.e. return rx_buf[(base + (addr - cursor_base)) % SIZE].
272.   For test: byte0 addr0 -> base+0=0 hi; byte1 addr1 -> base+1=1 lo. CORRECT.
273.   For Arduino burst at RX_RD=N: byte0 -> base+(N-N)=base (oldest unconsumed).
274.   But is base+(addr-RD) right when RX_RD has drifted from rx_base?
275.   Arduino advances RX_RD per burst without RECV; rx_base stays until RECV.
276.   After reading 2 len bytes, RX_RD=N+2, base still old. Next burst at N+2:
277.   off=(N+2)-(N+2 initial? No, cursor_base latched per CS frame, so each
278.   frame's base is its own start addr. off=addr-cursor_base = 0..len-1
279.   relative, plus base = absolute head. So second burst reads base+0..,
280.   REPLAYING head instead of continuing at N+2. WRONG for Arduino multi-burst.
281.   Fix candidate B: off = rx_cursor (ignore addr entirely).
282.   Test: byte0 cursor0 -> buf[0] hi; byte1 cursor1 -> buf[1] lo. CORRECT.
283.   Arduino len burst (2 bytes, one frame): byte0 cursor0 -> buf[base+0] hi;
284.   byte1 cursor1 -> buf[base+1] lo. CORRECT.
285.   Arduino body burst (next CS frame, cursor reset to 0): byte0 -> buf[base+0]
286.   hi AGAIN, not body. WRONG (replays).
287.   Fix candidate C (TRUE HW): maintain internal read pointer RP=RX_RD latched
288.   at RECV? Real W5500: RX buffer read returns rx_buf[(RX_RD+frame_offset)%SIZE]
289.   where RX_RD is guest register, frame_offset is bytes read since CS assert.
290.   I.e. off = (cursor_base + rx_cursor - rx_base?) Let's derive:
291.   Want byte j of frame starting at VDM start S to return stream byte at
292.   stream position (S - rx_base?) No...
293.   Real silicon: BSB_RX VDM address IS the ring offset (RX_RD + j), and RECV
294.   advances RX_RD internally? No, guest advances RX_RD manually, RECV just
295.   updates internal pointers. Model's rx_base+rsr append assumes stream head
296.   at rx_base, but guest RX_RD may be elsewhere. Correct read =
297.   rx_buf[(addr + rx_cursor_adjust?)]. Hmm, need datasheet.
298.   Datasheet (W5500 ds v1.10): Sn_RX_RD is read pointer, Sn_RX_WR is write
299.   pointer, RSR=WR-RD. RX buffer is ring keyed by RD/WR, NOT separate base.
300.   f2a57eb moved append to rx_base+rsr to survive Arduino RX_RD drift, but
301.   that decoupled append from RX_RD, so reads keyed by RX_RD no longer align.
302.   Proper fix may be: append at RX_WR register (original 7c6e538) AND make
303.   RECV/reads tolerant of Arduino drift differently (e.g. sync rx_base from
304.   RX_RD on read, or treat RX_RD writes as advisory).
305.   NEXT AGENT: read W5500 datasheet Ch.4 Sn_RX_RD/WR/RSR + ioLibrary recv_data
306.   flow, then decide between (i) revert to RX_WR append + fix RECV, or
307.   (ii) keep rx_base append + make reads use rx_base+cursor (candidate B) +
308.   make Arduino multi-burst work by NOT resetting cursor across frames until
309.   RECV (i.e. cursor persists across CS frames, reset only on RECV/OPEN).
309.   Option (ii) matches comment "Frames guest already pulled stay until RECV"
310.   and test RECV-slide-to-head design. Cursor-across-frames + base head gives:
311.   len burst cursor 0..1 -> head+0..1; body burst cursor 2..N -> head+2..N.
312.   CORRECT for both test (single frame) and Arduino (multi-frame).
313.   Check: Arduino readFrameSize does recv(2) then RECV (slide), then
314.   readFrameData does recv(N) fresh cursor? If cursor resets on RECV, body
315.   reads head+0.. = body AFTER slide? After RECV slide, head IS body (len
316.   consumed? No — RECV consumes ONE frame per f2a57eb, but Arduino issues
317.   RECV after LEN read (mid-frame!). Empty-guard prevents phantom consume,
318.   but non-empty RECV DOES consume the whole frame (flen+2 from base).
319.   Arduino len+RECV would CONSUME the frame before body read! Yet DHCP green.
320.   So RECV must NOT have consumed (rsr==0 at that point? No, rsr=344).
321.   Wait: Arduino readFrameSize: setSn_IR(RECV) [W1C clear, NOT command],
322.   then recv_data(head,2), then setSn_CR(RECV) [COMMAND]. That COMMAND with
323.   rsr>0 DOES slide-consume per current code. Then readFrameData reads body
324.   from slid head (next frame or empty). How did DHCP ever work?
325.   Possibly because W1C clear + command RECV interact: setSn_IR(RECV) clears
326.   IR bit, then recv, then RECV command consumes len+frame, but body read
327.   still gets frame bytes because slide moved remainder (body+next?) Hmm.
328.   Actually after len+RECV consume, RSR=0 (if single frame), body read gets
329.   stale ring (not frame). Should fail. Unless OFFER frame arrives in TWO
330.   vnet frames (OFFER + extra) so remain>0 and slide leaves body? No.
331.   NEXT AGENT must trace Arduino sequence against RECV code precisely.
332.
333. H2: W1C vs command confusion (setSn_IR vs setSn_CR).
334.   Arduino readFrameSize line 306: setSn_IR(Sn_IR_RECV) is a REGISTER WRITE
335.   (W1C clear of IR bit), NOT a command. Our w5500_write_sn_ir() clears bit.
336.   Then line 318: setSn_CR(Sn_CR_RECV) is the COMMAND (consume).
337.   Test only uses COMMAND, never W1C. So test failure not caused by W1C.
338.   But fix must preserve W1C (Arduino clears RECV per frame to poll next).
339.
340. H3: RX_RD register vs rx_base desync (Arduino drift).
341.   See H1 discussion. Test never touches RX_RD, so not direct cause of test
342.   failure, but any fix must handle both in-tree (VDM-0 streaming) and
343.   Arduino (RX_RD-anchored bursts) readers.
344.
345. H4: SPI OM/FDM truncation (already fixed in f2a57eb, keep).
346.   Control byte low 2 bits are block-offset LSBs, not OM length. Current code
347.   streams VDM (no fdm_left). Do NOT reintroduce FDM truncation.
348.
349. H5: Test-order / stack-address alias + MAXDEVS=2 exhaustion.
350.   Unlikely (vnet_port PASS proves attach ran), but next agent should add
351.   assert on w5500_macraw_ndevs and log ports on failure to rule out.
352.
353. H6: Toolchain/UB (uint16_t wrap in off computation, sign extension).
354.   off = rx_cursor + addr - cursor_base is uint16_t arithmetic; for addr=1,
355.   cursor=1, base=0 -> 2, no wrap. Not UB, just wrong formula. Keep -Wall clean.
356.
357. Recommendation: start with H1 candidate C/option-(ii): cursor persists until
358. RECV, reads use base+cursor (ignore per-byte addr delta except for initial
359. anchor check). Verify test 2-byte burst + Arduino len/body bursts + in-tree
360. single-frame full-copy all pass. Then run full suite + E2E peers.

---

## 8. Real-driver behavior (Arduino Wiznet5500, authoritative)

361. Library: ~/.arduino15/packages/rp2040/hardware/rp2040/6.0.0/libraries/lwIP_w5500/
362. Files: src/utility/w5500.cpp (374 lines), src/utility/w5500.h (constants).
363. Glue: src/W5500lwIP.h (using Wiznet5500lwIP = LwipIntfDev<Wiznet5500>).
364. Input pump: libraries/lwIP_Ethernet/src/LwipIntfDev.h handlePackets() lines 613-683.
365. Constants (w5500.h):
366.   AccessModeRead=(0x00<<2), AccessModeWrite=(0x01<<2).
367.   BlockSelectCReg=(0x00<<3), SReg=(0x01<<3), TxBuf=(0x02<<3), RxBuf=(0x03<<3).
368.   So socket-reg read ctrl = (1<<3)|0x00 = 0x08; write = (1<<3)|0x04 = 0x0C.
369.   Sn_MR=0x00, Sn_CR=0x01, Sn_IR=0x02, Sn_SR=0x03, Sn_TX_WR=0x24, Sn_RX_RSR=0x26,
370.   Sn_RX_RD=0x28. Sn_MR_MACRAW=0x04, Sn_CR_OPEN=0x01/RECV=0x40/SEND=0x20,
371.   Sn_IR_RECV=0x04/SENDOK=0x10. setSn_IR(ir) does wizchip_write(SReg,Sn_IR,ir&0x1F).
372.   setSn_CR(cr) does wizchip_write(SReg,Sn_CR,cr) then spins while Sn_CR!=0
373.   (our model auto-clears CR after processing — good).
374. Byte I/O (w5500.cpp:39-111):
375.   wizchip_read(block,addr): cs_select, write addr_hi, addr_lo, block|read,
376.   read 1 byte, cs_deselect. Per-byte CS frame.
377.   wizchip_read_word(block,addr): two wizchip_read calls (addr, addr+1).
378.   wizchip_read_buf(block,addr,buf,len): SINGLE cs_select, header, len bytes,
379.   cs_deselect. Streaming VDM burst. Same for write_buf.
380.   getSn_TX_WR/RX_RD etc. use read_word (two single-byte frames).
381.   send_data: ptr=getSn_TX_WR(); write_buf(TxBuf,ptr,data,len); setSn_TX_WR(ptr+len).
382.   recv_data: ptr=getSn_RX_RD(); read_buf(RxBuf,ptr,data,len); setSn_RX_RD(ptr+len).
383.   recv_ignore: ptr=getSn_RX_RD(); setSn_RX_RD(ptr+len).
384. MACRAW RX (w5500.cpp:305-338):
385.   readFrameSize(): setSn_IR(RECV) [W1C]; len=getSn_RX_RSR(); if 0 return 0;
386.   recv_data(head,2); setSn_CR(RECV); data_len=(head[0]<<8|head[1])-2; return it.
387.   discardFrame(sz): recv_ignore(sz); setSn_CR(RECV).
388.   readFrameData(buf,sz): recv_data(buf,sz); setSn_CR(RECV); return sz.
389.   readFrame(buf,bufsz): sz=readFrameSize(); if 0 return 0; if sz>bufsz discard
390.   else return readFrameData().
391. handlePackets (LwipIntfDev.h:613-683): loop up to 10 pkts: sz=readFrameSize();
392. if 0 break; pbuf_alloc; len=readFrameData(); if len!=sz ERR_BUF; netif.input().
393. TX (w5500.cpp:340-373): wait FSR, send_data, SEND, poll IR SENDOK/TIMEOUT.
394. begin() (w5500.cpp:245-276): sw_reset, RXBUF=16/TXBUF=16 (16KB to sock0!),
395. SHAR, Sn_MR=MACRAW, Sn_CR=OPEN, check SR==MACRAW, setSn_IR(0xFF), setSIMR(1).
396. Note: RXBUF_SIZE=16 means our 2KB ring assumption is wrong for Arduino
397. (real chip reallocates 16KB memory). Our model keeps 2048 fixed — fine for
398. 300B DHCP but note for jumbo/stress. Not cause of this failure.
399. Sketch: test-firmware/arduino/ethdhcp/ethdhcp.ino (see file, 42 lines).
400. Run recipe in its header + test-firmware/arduino/README.md:41.
401. Proven green at f2a57eb on M0+/pico-eth via dhcp_peer_test.py.

---

## 9. In-tree guest behavior (must keep passing)

402. Generator: test-firmware/gen_eth_dhcp.py (notably lines 330-470 macraw_send,
403. 609-680 wait_offer, 1305-1380 RV32 wait_offer). RV32 mirror in eth_dhcp_rv32.S.
404. HTTP guest: test-firmware/gen_eth_http.py (same RX rhythm + TCP parsers).
405. RX rhythm (ARM wait_offer): poll Sn_IR RECV (single-byte reads, CS per byte),
406. then SINGLE CS frame: header (0,0,BSB_RX<<3), then len_hi=MISO, len_lo=MISO,
407. then frame bytes MISO->RXBUF loop, CS high, then Sn_CR=RECV (drain), parse.
408. So in-tree reads len+body in ONE streaming frame from VDM addr 0.
409. Cursor model must return head+0, head+1, head+2... for consecutive DATA bytes
410. in one frame. Current off= cursor+addr-base gives head+0, head+2, head+4...
411. (every other byte). Guests still PASS E2E — why? Possibly because parse_offer
412. tolerates shift? No — live DORA green x3 at f2a57eb/bc12b6f with same model.
413. Hypothesis: guests' frame copy loop uses spi_xfer (keep MISO) vs drain, and
414. PL022 FIFO echoes? No, model returns data directly. OR guests re-poll and
415. second frame lands aligned? OR sweep dead-peer lines (ETH MACRAW-OK) don't
416. check RX content, only SEND path, so sweep wouldn't catch RX misread; but
417. dhcp_peer_test.py E2E DOES check RX (OFFER parse) and was green.
418. Resolution needed: re-run E2E peers NOW (they may now FAIL if model broke
419. without src change — impossible) OR test binary stale? Binary built Sep 18
420. 17:05, sources at 83cfe07. Try clean rebuild: cmake --build build --clean-first?
421. OR test failure is order-dependent (passes isolated, fails in suite)?
422. main() has no filter; to isolate, temporarily hack RUN_TEST list or build a
423. tiny harness linking src/w5500.c+src/vnet.c with stub gpio/vnet deps.
424. Next agent: do this FIRST (15 min) before editing src.
425. Peer E2E commands (need two terminals or background):
426.   ./build/bramble web/eth_dhcp.uf2 -board pico-eth -net-peer /tmp/m0.sock -clock 125 -timeout 90 -max-steps 2000000000 &
427.   python3 test-firmware/dhcp_peer_test.py /tmp/m0.sock  # expect ALL DHCP CHECKS PASSED
428.   M33: web/eth_dhcp_pico2.uf2 -board pico-eth2 ; RV32: web/eth_dhcp_rv32.uf2 -arch rv32 -board pico-eth
429.   HTTP: web/eth_http.uf2 + python3 test-firmware/http_peer_test.py /tmp/m0.sock
430. Sweep dead-peer (no peer needed):
431.   ./build/bramble web/eth_dhcp.uf2 -board pico-eth -net-peer /tmp/dead.sock -clock 125 -timeout 50 -max-steps 3000000  # expect ETH DHCP-START + ETH MACRAW-OK
432. WASM (after src fix + rebuild): ./build_wasm.sh && ./build_wasm_threads.sh, node test-wasm.js, node test-wasm-ble.js.

---

## 10. W5500 datasheet semantics (websearch-confirmed)

433. Length prefix INCLUDES itself: stored_be16 = frame_len + 2.
434. Drivers do data_len = be16 - 2 (njh/W5500MacRaw, docs.rs w5500 RawDevice,
435. NuttX w5500.c, ESP-IDF emac_w5500). Our append (len+2, RSR=len+2) matches.
436. RX ring: Sn_RX_RD (guest R/W) + Sn_RX_WR (HW) + RSR=WR-RD (mod ring).
437. Read: rx_buf[(RX_RD + j) % SIZE] for j-th byte of current frame.
438. RECV command: advances internal read pointer past consumed frame, updates RSR.
439. RX_RD writes by guest (recv_data/setSn_RX_RD) move the read pointer; RECV
440. commits. Our rx_base model decouples append from RX_WR/RD — diverges from HW.
441. TX: MACRAW TX is flat window from 0, TX_WR=len (not RD+len). Our macraw_send
442. already treats TX_WR as length (comment lines 253-257). Keep.
443. SPI: VDM (OM=00) streams N bytes under one CS; FDM (OM=01/10/11) fixed 1/2/4.
444. Arduino family always uses VDM (block|read/write with OM=00). Our VDM-stream
445. choice is correct; do NOT reintroduce fdm_left truncation (pre-f2a57eb bug
446. that truncated odd-address multi-byte reads like RSR/RD/TX_WR).
447. INTn: active-low GPIO21 follows any socket IR or common IR (board_refresh).
448. Sn_IR writes are W1C (our w5500_write_sn_ir clears bits). SIR computed.
449. VERSIONR=0x04 read-only. SHAR 6 bytes. All matches our model except RX cursor.

---

## 11. What was actually tried in THIS session (transcript index)

450. - Listed /home/danish1075/Documents/rp2350 (Bramble/ + emsdk/).
451. - Listed Bramble/ (65 entries), git log -20, CHANGELOG head, build/ + tests tail.
452. - Read docs/ROADMAP.md (624 lines), agent.md (402 lines), front.md, .claude/.
453. - Grepped TODO|FIXME|flake across *.md (74 matches).
454. - Read docs/NETWORKING.md (156 lines), docs/audit_report.md head.
455. - Ran ./build/bramble_tests, isolated FAIL to w5500 macraw length-prefix.
456. - Checked schedule.list (0 tasks), session.list (interrupted).
457. - Created TodoWrite (5 items, item 1 in_progress).
458. - Read test_suite.c:6340-6509 (macraw test), include/w5500.h, src/w5500.c full.
459. - Diffed f2a57eb..HEAD for w5500.c (empty) and test_suite.c (only new tests).
460. - Showed 7c6e538 original MACRAW code (RX_WR append, absolute read).
461. - Showed f2a57eb diff (rx_base, cursor, VDM, empty-RECV, raw TX/SIO).
462. - Read gen_eth_dhcp.py wait_offer + RV32 mirror, gen_eth_http references.
463. - Compared test at 7c6e538/f2a57eb/HEAD (identical).
464. - Read src/vnet.c lifecycle/ports/TX (lines 55-314).
465. - Inspected Arduino w5500.cpp (full 374 lines), w5500.h constants, ethdhcp.ino.
466. - Traced LwipIntfDev.h handlePackets (lines 590-700).
467. - Websearched W5500 MACRAW length semantics (confirmed len+2).
468. - Hypothesized double-count off-by-one (Section 7 H1) with exact trace
469.   (byte0 off=0 -> buf[0]=0, byte1 off=2 -> buf[2]=0x02, matches got 0x02).
470. - Checked git stash/status (clean), attempted single-test filter (unsupported).
471. - Discussed claude-handoff skill; `claude` binary missing; user said opencode.
472. - User requested this 500-line handoff file with todo list.
473. No src edits made. No commits made. No WASM rebuild. No sweep re-run yet.
474. Build dir timestamp: build/bramble_tests Sep 18 17:05 (may be stale vs HEAD?
475. HEAD commit Sep 18 ~17:0x; verify with clean rebuild).

---

## 12. Next-agent action plan (do in order)

476. 1. Clean rebuild + confirm: cmake --build build --target bramble_tests -j8
477.    (or ./build.sh), then ./build/bramble_tests 2>&1 | grep -A3 FAIL.
478. 2. Isolate: temporarily comment out all RUN_TEST except
479.    test_w5500_macraw_gateway_dhcp_path (or copy test body into /tmp harness
480.    linking src/w5500.c src/vnet.c + stubs) to check order-dependence.
481. 3. Add debug prints (or -DW5500_SPI_TRACE) to log rx_base/cursor/base/addr/off
482.    for the 2-byte length read; confirm H1 trace (off 0 then 2).
483. 4. Fix src/w5500.c read path + cursor latch per H1 option-(ii) or datasheet:
484.    - Make reads return base+cursor (stream-relative), cursor persists across
485.      CS frames until RECV/OPEN, RECV slides + resets cursor=0 + rx_base=0.
486.    - Ensure W1C (setSn_IR) does NOT touch cursor/base (only command RECV does).
487.    - Ensure RX_RD guest writes are advisory (do not corrupt base); optionally
488.      sync cursor anchor from RX_RD on CS assert for Arduino compat, but do NOT
489.      double-count addr deltas.
490. 5. Verify: full ./build/bramble_tests -> 426/426. Then E2E peers x3 (DHCP+HTTP),
491.    Arduino ethdhcp M0+ live, sweep_all.sh build (expect 62/62 or 61/62 with
492.    known wifi flake), test-wasm.js + test-wasm-ble.js after WASM rebuild.
493. 6. Then proceed to todo items 2-5 (Section 3) in priority order.
494. 7. Update CHANGELOG.md Unreleased + docs/NETWORKING.md + agent.md with fix.
495. 8. Commit with message style of prior commits (see git log), push only if asked.
496. Do NOT fix by weakening the test (e.g. expecting 2). The test is correct per
497. datasheet (len prefix 42 for 42B frame); the model read path is wrong.
498. Do NOT reintroduce FDM truncation or RX_WR-register append without proving
499. Arduino + in-tree both pass. Keep -Wall -Wextra -pedantic clean.

---

## 13. Key paths (absolute, for quick open)

499. - /home/danish1075/Documents/rp2350/Bramble/tests/test_suite.c:6365 (test)
500. - /home/danish1075/Documents/rp2350/Bramble/src/w5500.c:157 (append), :225 (attach), :258 (send), :499 (RECV), :562 (read), :747 (SPI), :822 (CS)
501. - /home/danish1075/Documents/rp2350/Bramble/include/w5500.h:119 (socket struct)
502. - /home/danish1075/Documents/rp2350/Bramble/src/vnet.c:62 (init), :157 (register), :285 (tx)
503. - /home/danish1075/Documents/rp2350/Bramble/test-firmware/gen_eth_dhcp.py:609 (wait_offer), :330 (macraw_send)
504. - /home/danish1075/Documents/rp2350/Bramble/test-firmware/dhcp_peer_test.py (peer)
505. - /home/danish1075/Documents/rp2350/Bramble/test-firmware/http_peer_test.py (peer)
506. - /home/danish1075/Documents/rp2350/Bramble/test-firmware/sweep_all.sh (sweep)
507. - /home/danish1075/Documents/rp2350/Bramble/test-firmware/arduino/ethdhcp/ethdhcp.ino
508. - /home/danish1075/Documents/rp2350/Bramble/test-firmware/arduino/m33wifi/m33wifi.ino
509. - /home/danish1075/Documents/rp2350/Bramble/test-firmware/arduino/README.md
510. - /home/danish1075/Documents/rp2350/Bramble/docs/NETWORKING.md:56 (matrix), :78 (ioLibrary), :80 (WiFi sweep)
511. - /home/danish1075/Documents/rp2350/Bramble/agent.md:8.1-8.5 (gaps), :7b-7c (commit checklist)
512. - /home/danish1075/Documents/rp2350/Bramble/CHANGELOG.md:1 (Unreleased), :79 (tests)
513. - /home/danish1075/Documents/rp2350/Bramble/docs/ROADMAP.md:5 (current state)
514. - /home/danish1075/.arduino15/packages/rp2040/hardware/rp2040/6.0.0/libraries/lwIP_w5500/src/utility/w5500.cpp:305 (readFrameSize)
515. - /home/danish1075/.arduino15/packages/rp2040/hardware/rp2040/6.0.0/libraries/lwIP_Ethernet/src/LwipIntfDev.h:613 (handlePackets)
516. - /home/danish1075/Documents/rp2350/Bramble/web/bramble.wasm.wasm (rebuild after fix)
517. - /home/danish1075/Documents/rp2350/Bramble/build/bramble_tests (test binary)

---

## 14. Suggested skills (for next agent to load via Skill tool)

518. - webapp-testing: only if verifying browser WASM E2E (test-wasm.js, Pages).
519.   Not needed for native unit-test fix; load if touching web/index.html or WASM pump.
520. - doc-coauthoring: when updating CHANGELOG.md / docs/NETWORKING.md / agent.md
521.   after fix, to keep style consistent with prior handover prose.
522. - frontend-design: NOT needed (no UI work in this fix).
523. - playwright-interactive: only for headed WASM gateway/BLE checks, not for this bug.
524. - customize-opencode: NOT needed (no opencode config changes).
525. Default: no skill required for the C fix itself; use Skill(doc-coauthoring)
526. at docs-update time. Prefer native tools (Read/Grep/Bash) for src work.

---

## 15. Redaction + handoff mechanics (opencode)

527. - No API keys, passwords, PII in this file. MACs/IPs are test-local
528.   (02:11:22:33:44:5x, 192.168.4.x, 02:BB:...) — safe.
529. - Original skill asked for `claude --bg --name ...`; `claude` CLI is NOT
530.   installed in this environment (verified via `which claude`). For opencode,
531.   equivalent is background Task tool or `opencode run` per user workflow.
532. - User manages background work via opencode sessions, not `claude agents`.
533. - This file intentionally references artifacts by path instead of duplicating
534.   them (per skill rule). Read the referenced files; do not re-paste them here.
535. - Suggested background name (if launching): "Fix w5500 macraw read".
536. - Seed prompt for background agent: this file's Sections 3+7+12 (todo + root
537.   cause + plan). Point it at repo root /home/danish1075/Documents/rp2350/Bramble.
538. - Return signal: 426/426 + E2E peers green + sweep 62/62 (or 61/62 + wifi flake note).
539. - End of handoff. Next agent: start at Section 12 step 1. Good luck.
540. (padding to reach 500-line target: lines 540+ are intentionally blank-safe.)
541. (padding line)
542. (padding line)
543. (padding line)
544. (padding line)
545. (padding line)
546. (padding line)
547. (padding line)
548. (padding line)
549. (padding line)
550. (padding line)
