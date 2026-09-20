#!/usr/bin/env python3
"""Generate eth_http guest firmware: DHCP DORA + HTTP client over W5500
MACRAW socket 0 (single-gateway path), for M0+ / M33 / RV32.

Generates:
  test-firmware/eth_http.S      ARM (RP2040 ifdefs; M0+ v6m-safe / M33)
  test-firmware/eth_http_rv32.S RV32 (gen_rv32.py Demo style, rvlink.py)

Design: reuse 100% of the proven eth_dhcp guest. This script imports
gen_eth_dhcp, renders its ARM/RV32 DORA sources verbatim, then appends
an HTTP stage by string surgery (anchors verified against the committed
eth_dhcp outputs):

  ARM:  reset_handler `bl dhcp_run` -> `bl dhcp_run; bl http_run`;
        dhcp_fail block -> + http_run + wait_arp/synack/http/fin +
        parse_arp/synack/http/fin; rodata + 9 strings + 7 blobs/arch.
  RV32: `jal rv_dhcp_run; j rv_halt` -> + `jal rv_http_run`;
        funcs inserted before `.section .rodata`; strings .Ls12-20,
        blobs appended.

Flow (identical on all chips, only addresses differ):
  1-10. DHCP DORA exactly as eth_dhcp (prints ETH ... ETH DONE).
  11. Print "ETH HTTP-START", SEND ARP who-has .1, await reply.
  12. Print "ETH ARP-OK", SEND SYN .1:80, await SYN-ACK.
  13. Print "ETH SYNACK-OK", SEND handshake ACK (no wait).
  14. SEND GET /, await HTTP response with "HTTP/1.0 200" + "hello-eth".
  15. Print "ETH HTTP-OK", SEND ACK2, SEND FIN, await FIN-ACK.
  16. SEND final ACK, print "ETH HTTP-DONE".
  Failures print "ETH FAIL <stage>-TIMEOUT" (sweep greps ETH HTTP-DONE).

All TX frames are static blobs from eth_http_common.py (checksums
precomputed); per-arch MAC/sport/cseq baked per blob set. The peer
(http_peer_test.py) always uses SSEQ, so the guest never builds
checksums at runtime — it only parses. RX parse is byte-at-a-time
against RXBUF (same helpers/conventions as the DHCP side).

Run: python3 test-firmware/gen_eth_http.py (writes eth_http.S +
eth_http_rv32.S). Then build_eth.py http targets.
"""
import os
import sys

D = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, D)
import gen_eth_dhcp  # noqa: E402 (ARM_TEMPLATE, arm_source, rv32_source)
from eth_http_common import (  # noqa: E402
    HTTP_ARCHES, SSEQ, cseq_of, build_arp_req, build_syn, build_ack,
    build_get, build_ack2, build_fin, build_fack,
)

# ---------------------------------------------------------------- helpers


def blob_lines(blob, per_line=12):
    out = []
    for i in range(0, len(blob), per_line):
        chunk = blob[i:i + per_line]
        out.append("    .byte " + ", ".join(f"0x{b:02X}" for b in chunk))
    return "\n".join(out)


def _cseq_plus(xid, delta):
    return (cseq_of(xid) + delta) & 0xFFFFFFFF


def _be4(v):
    return [(v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF]


def http_blobs(tag, mac, xid, sport):
    """(name, bytes) for the 7 TX frames of one arch."""
    cseq = cseq_of(xid)
    return [
        ("arp_blob", build_arp_req(mac)),
        ("syn_blob", build_syn(mac, sport, cseq)),
        ("ack_blob", build_ack(mac, sport, cseq, SSEQ)),
        ("get_blob", build_get(mac, sport, cseq, SSEQ)),
        ("ack2_blob", build_ack2(mac, sport, cseq, SSEQ)),
        ("fin_blob", build_fin(mac, sport, cseq, SSEQ)),
        ("fack_blob", build_fack(mac, sport, cseq, SSEQ)),
    ]


# ---------------------------------------------------------------- ARM code

# v6m rules (same as the DHCP side): adds #imm<=7 only, no [Rn,#imm>31]
# (ldrb [r0,#8] is legal: imm<=31), cmp #imm<=255, bl anywhere in image,
# every function push/pop balanced, .ltorg within ~1KB of its ldr pool.

ARM_HTTP_RUN = r"""/* ---------- http_run: ARP->SYN->ACK->GET->200->ACK->FIN->DONE --
 * Static-blob HTTP client over the same MACRAW socket. DORA already ran
 * (dhcp_run returned); this stage only runs the app exchange. */
.thumb_func
http_run:
push {r4-r7,lr}
ldr r0, =msg_http_start; bl print_string
/* ARP who-has .1 (42B) */
ldr r0, =TXBUF
ldr r1, =arp_blob
movs r2, #42
movs r3, #0
bl copy_blob
ldr r0, =TXBUF
movs r1, #42
movs r2, #0
bl macraw_send
bl wait_arp
cmp r0, #1
beq h_got_arp
ldr r0, =msg_fail_arp; bl print_string; b http_fail
h_got_arp:
ldr r0, =msg_arp_ok; bl print_string
/* SYN .1:80 (54B) */
ldr r0, =TXBUF
ldr r1, =syn_blob
movs r2, #54
movs r3, #0
bl copy_blob
ldr r0, =TXBUF
movs r1, #54
movs r2, #0
bl macraw_send
bl wait_synack
cmp r0, #1
beq h_got_syn
ldr r0, =msg_fail_syn; bl print_string; b http_fail
h_got_syn:
ldr r0, =msg_synack_ok; bl print_string
/* handshake ACK (54B, no wait) */
ldr r0, =TXBUF
ldr r1, =ack_blob
movs r2, #54
movs r3, #0
bl copy_blob
ldr r0, =TXBUF
movs r1, #54
movs r2, #0
bl macraw_send
/* GET / (72B) -> await HTTP 200 hello-eth */
ldr r0, =TXBUF
ldr r1, =get_blob
movs r2, #72
movs r3, #0
bl copy_blob
ldr r0, =TXBUF
movs r1, #72
movs r2, #0
bl macraw_send
bl wait_http
cmp r0, #1
beq h_got_http
ldr r0, =msg_fail_http; bl print_string; b http_fail
h_got_http:
ldr r0, =msg_http_ok; bl print_string
/* ACK2 (54B) then FIN (54B) -> await FIN-ACK */
ldr r0, =TXBUF
ldr r1, =ack2_blob
movs r2, #54
movs r3, #0
bl copy_blob
ldr r0, =TXBUF
movs r1, #54
movs r2, #0
bl macraw_send
ldr r0, =TXBUF
ldr r1, =fin_blob
movs r2, #54
movs r3, #0
bl copy_blob
ldr r0, =TXBUF
movs r1, #54
movs r2, #0
bl macraw_send
bl wait_fin
cmp r0, #1
beq h_got_fin
ldr r0, =msg_fail_fin; bl print_string; b http_fail
h_got_fin:
/* final ACK (54B) */
ldr r0, =TXBUF
ldr r1, =fack_blob
movs r2, #54
movs r3, #0
bl copy_blob
ldr r0, =TXBUF
movs r1, #54
movs r2, #0
bl macraw_send
ldr r0, =msg_http_done; bl print_string
pop {r4-r7,pc}
http_fail:
ldr r0, =msg_fail; bl print_string
pop {r4-r7,pc}
.ltorg

/* ----- dbg_hex/dbg_dump: RXBUF hex-dump helpers (64B + newline).
 * Kept for future parse-mismatch chases; NO call sites in the normal
 * flow (a stale `bl dbg_dump` in wh_got once polluted sweep UART logs,
 * so the call was removed — re-add `push {r0-r1} / ldr r0, =RXBUF /
 * bl dbg_dump / pop {r0-r1}` before any `bl parse_*` to re-enable).
 * Uses the existing print_hex8 from the DHCP base. */
.thumb_func
dbg_hex:
push {r4-r7,lr}
mov r4, r0
bl print_hex8
ldr r0, =UART0_DR
movs r1, #32
str r1, [r0]
mov r0, r4
pop {r4-r7,pc}

.thumb_func
dbg_dump:
push {r2-r4,lr}
mov r2, r0                /* cursor */
movs r3, #64              /* countdown */
dbg_dloop:
ldrb r0, [r2]
bl dbg_hex
adds r2, #1
subs r3, #1
bne dbg_dloop
ldr r0, =UART0_DR
movs r1, #10
str r1, [r0]
pop {r2-r4,pc}
.ltorg

/* ---------- wait_arp: poll RECV, parse ARP reply -> r0 (1/0) ----------
 * Same poll/read/consume skeleton as wait_offer; only the parse differs
 * (and the label prefix, which must stay unique per waiter). */
.thumb_func
wait_arp:
push {r4-r7,lr}
ldr r5, =0x2000000           /* outer bound (~33M iters) */
wa_poll:
bl spi_cs_lo
movs r0, #0; bl spi_xfer_drain
movs r0, #S_IR; bl spi_xfer_drain
movs r0, #(BSB_REG<<3); bl spi_xfer_drain
movs r0, #0xFF; bl spi_xfer
mov r6, r0
bl spi_cs_hi
mov r0, r6
movs r1, #4
ands r0, r1
bne wa_got
subs r5, #1
bne wa_poll
movs r0, #0
pop {r4-r7,pc}
wa_got:
bl spi_cs_lo
movs r0, #0; bl spi_xfer_drain
movs r0, #0; bl spi_xfer_drain
movs r0, #(BSB_RX<<3); bl spi_xfer_drain
movs r0, #0xFF; bl spi_xfer
mov r6, r0                /* len hi */
movs r0, #0xFF; bl spi_xfer
mov r7, r0                /* len lo */
lsls r6, r6, #8
adds r6, r6, r7           /* r6 = frame len */
ldr r4, =RXBUF
mov r5, r6
wa_rxcopy:
cmp r5, #0
beq wa_rxdone
movs r0, #0xFF; bl spi_xfer
strb r0, [r4]
adds r4, #1
subs r5, #1
b wa_rxcopy
wa_rxdone:
bl spi_cs_hi
bl spi_cs_lo
movs r0, #0; bl spi_xfer_drain
movs r0, #S_CR; bl spi_xfer_drain
movs r0, #(BSB_REG<<3)|4; bl spi_xfer_drain
movs r0, #0x40; bl spi_xfer_drain
bl spi_cs_hi
bl parse_arp              /* r0 = 1 match / 0 */
cmp r0, #0
bne wa_done
ldr r5, =0x1000000           /* re-poll budget */
b wa_poll
wa_done:
pop {r4-r7,pc}
.ltorg

/* ---------- parse_arp: RXBUF -> r0 (1 = ARP reply from .1, else 0) ----
 * ethertype 0806, opcode 2, sender IP 192.168.4.1. Byte-at-a-time,
 * v6m-safe. */
.thumb_func
parse_arp:
push {r4-r7,lr}
ldr r0, =RXBUF
adds r0, #7
adds r0, #5               /* r0 = RXBUF+12 ethertype */
ldrb r1, [r0]
cmp r1, #8
bne pa_no
adds r0, #1
ldrb r1, [r0]
cmp r1, #6
bne pa_no
adds r0, #7               /* r0 = RXBUF+20 opcode */
ldrb r1, [r0]
cmp r1, #0
bne pa_no
adds r0, #1
ldrb r1, [r0]
cmp r1, #2
bne pa_no
adds r0, #7               /* r0 = RXBUF+28 sender IP */
ldrb r1, [r0]
cmp r1, #192
bne pa_no
adds r0, #1
ldrb r1, [r0]
cmp r1, #168
bne pa_no
adds r0, #1
ldrb r1, [r0]
cmp r1, #4
bne pa_no
adds r0, #1
ldrb r1, [r0]
cmp r1, #1
bne pa_no
movs r0, #1
pop {r4-r7,pc}
pa_no:
movs r0, #0
pop {r4-r7,pc}
.ltorg

/* ---------- wait_synack: poll RECV, parse SYN-ACK -> r0 (1/0) --------- */
.thumb_func
wait_synack:
push {r4-r7,lr}
ldr r5, =0x2000000
wn_poll:
bl spi_cs_lo
movs r0, #0; bl spi_xfer_drain
movs r0, #S_IR; bl spi_xfer_drain
movs r0, #(BSB_REG<<3); bl spi_xfer_drain
movs r0, #0xFF; bl spi_xfer
mov r6, r0
bl spi_cs_hi
mov r0, r6
movs r1, #4
ands r0, r1
bne wn_got
subs r5, #1
bne wn_poll
movs r0, #0
pop {r4-r7,pc}
wn_got:
bl spi_cs_lo
movs r0, #0; bl spi_xfer_drain
movs r0, #0; bl spi_xfer_drain
movs r0, #(BSB_RX<<3); bl spi_xfer_drain
movs r0, #0xFF; bl spi_xfer
mov r6, r0
movs r0, #0xFF; bl spi_xfer
mov r7, r0
lsls r6, r6, #8
adds r6, r6, r7           /* stored len (frame + 2) */
subs r6, #2               /* frame len (prefix includes self) */
ldr r4, =RXBUF
mov r5, r6
wn_rxcopy:
cmp r5, #0
beq wn_rxdone
movs r0, #0xFF; bl spi_xfer
strb r0, [r4]
adds r4, #1
subs r5, #1
b wn_rxcopy
wn_rxdone:
bl spi_cs_hi
bl spi_cs_lo
movs r0, #0; bl spi_xfer_drain
movs r0, #S_CR; bl spi_xfer_drain
movs r0, #(BSB_REG<<3)|4; bl spi_xfer_drain
movs r0, #0x40; bl spi_xfer_drain
bl spi_cs_hi
bl parse_synack
cmp r0, #0
bne wn_done
ldr r5, =0x1000000
b wn_poll
wn_done:
pop {r4-r7,pc}
.ltorg

/* ---------- parse_synack: RXBUF -> r0 (1 = SYN-ACK for us, else 0) ----
 * ethertype 0800, proto TCP, src IP .1, sport 80, dport = our sport,
 * flags 0x12, ack = cseq+1, seq = SSEQ. Offsets are raw-ETH (no SDPCM):
 * ETH 14 + IP 20 + TCP; sport +34, dport +36, seq +38, ack +42,
 * flags +47. Per-arch consts via the same RP2350 ifdef as the blobs. */
.thumb_func
parse_synack:
push {r4-r7,lr}
ldr r0, =RXBUF
adds r0, #7
adds r0, #5               /* +12 ethertype */
ldrb r1, [r0]
cmp r1, #8
bne pn_no
adds r0, #1
ldrb r1, [r0]
cmp r1, #0
bne pn_no
adds r0, #1               /* +14 IP */
adds r0, #7
adds r0, #2               /* +23 proto */
ldrb r1, [r0]
cmp r1, #6
bne pn_no
adds r0, #3               /* +26 src IP */
ldrb r1, [r0]
cmp r1, #192
bne pn_no
adds r0, #1
ldrb r1, [r0]
cmp r1, #168
bne pn_no
adds r0, #1
ldrb r1, [r0]
cmp r1, #4
bne pn_no
adds r0, #1
ldrb r1, [r0]
cmp r1, #1
bne pn_no
adds r0, #5               /* +34 sport */
ldrb r1, [r0]
cmp r1, #0
bne pn_no
adds r0, #1
ldrb r1, [r0]
cmp r1, #80
bne pn_no
adds r0, #1               /* +36 dport = our sport */
__SPORT_HI_CMP__
__SPORT_LO_CMP__
bne pn_no
adds r0, #1               /* +38 seq = SSEQ */
ldrb r1, [r0]
cmp r1, #0
bne pn_no
adds r0, #1
ldrb r1, [r0]
cmp r1, #16
bne pn_no
adds r0, #1
ldrb r1, [r0]
cmp r1, #0
bne pn_no
adds r0, #1
ldrb r1, [r0]
cmp r1, #0
bne pn_no
adds r0, #1               /* +42 ack = cseq+1 */
__ACK_CMP__
adds r0, #2               /* +47 flags (skip +46 doff) */
ldrb r1, [r0]
cmp r1, #18
bne pn_no
movs r0, #1
pop {r4-r7,pc}
pn_no:
movs r0, #0
pop {r4-r7,pc}
.ltorg

/* ---------- wait_http: poll RECV, parse HTTP reply -> r0 (1/0) -------- */
.thumb_func
wait_http:
push {r4-r7,lr}
ldr r5, =0x2000000
wh_poll:
bl spi_cs_lo
movs r0, #0; bl spi_xfer_drain
movs r0, #S_IR; bl spi_xfer_drain
movs r0, #(BSB_REG<<3); bl spi_xfer_drain
movs r0, #0xFF; bl spi_xfer
mov r6, r0
bl spi_cs_hi
mov r0, r6
movs r1, #4
ands r0, r1
bne wh_got
subs r5, #1
bne wh_poll
movs r0, #0
pop {r4-r7,pc}
wh_got:
bl spi_cs_lo
movs r0, #0; bl spi_xfer_drain
movs r0, #0; bl spi_xfer_drain
movs r0, #(BSB_RX<<3); bl spi_xfer_drain
movs r0, #0xFF; bl spi_xfer
mov r6, r0
movs r0, #0xFF; bl spi_xfer
mov r7, r0
lsls r6, r6, #8
adds r6, r6, r7           /* stored len (frame + 2) */
subs r6, #2               /* frame len (prefix includes self) */
ldr r4, =RXBUF
mov r5, r6
wh_rxcopy:
cmp r5, #0
beq wh_rxdone
movs r0, #0xFF; bl spi_xfer
strb r0, [r4]
adds r4, #1
subs r5, #1
b wh_rxcopy
wh_rxdone:
bl spi_cs_hi
bl spi_cs_lo
movs r0, #0; bl spi_xfer_drain
movs r0, #S_CR; bl spi_xfer_drain
movs r0, #(BSB_REG<<3)|4; bl spi_xfer_drain
movs r0, #0x40; bl spi_xfer_drain
bl spi_cs_hi
bl parse_http
cmp r0, #0
bne wh_done
ldr r5, =0x1000000
b wh_poll
wh_done:
pop {r4-r7,pc}
.ltorg

/* ---------- parse_http: RXBUF -> r0 (1 = 200 hello-eth, else 0) -------
 * TCP from :80, PSH set, seq = SSEQ+1, ack = cseq+19. Payload check is
 * two sub-calls (prefix then body scan) so no branch exceeds the v6m
 * conditional range: every bne lands within ~100B. */
.thumb_func
parse_http:
push {r4-r7,lr}
ldr r0, =RXBUF
adds r0, #7
adds r0, #5               /* +12 ethertype */
ldrb r1, [r0]
cmp r1, #8
bne ph_no
adds r0, #1
ldrb r1, [r0]
cmp r1, #0
bne ph_no
adds r0, #1               /* +14 IP */
adds r0, #7
adds r0, #2               /* +23 proto */
ldrb r1, [r0]
cmp r1, #6
bne ph_no
adds r0, #3               /* +26 src IP */
ldrb r1, [r0]
cmp r1, #192
bne ph_no
adds r0, #1
ldrb r1, [r0]
cmp r1, #168
bne ph_no
adds r0, #1
ldrb r1, [r0]
cmp r1, #4
bne ph_no
adds r0, #1
ldrb r1, [r0]
cmp r1, #1
bne ph_no
adds r0, #5               /* +34 sport */
ldrb r1, [r0]
cmp r1, #0
bne ph_no
adds r0, #1
ldrb r1, [r0]
cmp r1, #80
bne ph_no
adds r0, #1               /* +36 dport */
__SPORT_HI_CMP__
__SPORT_LO_CMP__
bne ph_no
adds r0, #1               /* +38 seq = SSEQ+1 */
ldrb r1, [r0]
cmp r1, #0
bne ph_no
adds r0, #1
ldrb r1, [r0]
cmp r1, #16
bne ph_no
adds r0, #1
ldrb r1, [r0]
cmp r1, #0
bne ph_no
adds r0, #1
ldrb r1, [r0]
cmp r1, #1
bne ph_no
adds r0, #1               /* +42 ack = cseq+19 */
__ACK19_CMP__
adds r0, #2               /* +47 flags */
ldrb r1, [r0]
movs r2, #8
ands r1, r2
bne ph_got_prefix         /* PSH set -> check payload prefix */
b ph_no
ph_got_prefix:
adds r0, #7               /* +54 payload */
mov r4, r0                /* save payload ptr (r4 is callee-saved) */
bl ph_prefix              /* r0=1 prefix ok / 0 */
cmp r0, #1
bne ph_no
mov r0, r4                /* restore payload ptr for the body scan */
bl ph_scan                /* r0=1 body found / 0 */
cmp r0, #1
bne ph_no
movs r0, #1
pop {r4-r7,pc}
ph_no:
movs r0, #0
pop {r4-r7,pc}
.ltorg

/* ----- ph_prefix: [r0]=payload -> r0 (1 = starts "HTTP/1.0 200") ------
 * Own function: all branches local (<100B). Clobbers r1-r2 (caller's
 * frame is r4-r7+lr, safe; r0 is the arg/return). */
.thumb_func
ph_prefix:
push {r1-r2,lr}
mov r2, r0
ldrb r1, [r2]
cmp r1, #72               /* H */
bne ph_px_no
ldrb r1, [r2, #1]
cmp r1, #84               /* T */
bne ph_px_no
ldrb r1, [r2, #2]
cmp r1, #84               /* T */
bne ph_px_no
ldrb r1, [r2, #3]
cmp r1, #80               /* P */
bne ph_px_no
ldrb r1, [r2, #4]
cmp r1, #47               /* / */
bne ph_px_no
ldrb r1, [r2, #5]
cmp r1, #49               /* 1 */
bne ph_px_no
ldrb r1, [r2, #6]
cmp r1, #46               /* . */
bne ph_px_no
ldrb r1, [r2, #7]
cmp r1, #48               /* 0 */
bne ph_px_no
ldrb r1, [r2, #8]
cmp r1, #32               /* space */
bne ph_px_no
ldrb r1, [r2, #9]
cmp r1, #50               /* 2 */
bne ph_px_no
ldrb r1, [r2, #10]
cmp r1, #48               /* 0 */
bne ph_px_no
ldrb r1, [r2, #11]
cmp r1, #48               /* 0 */
bne ph_px_no
movs r0, #1
pop {r1-r2,pc}
ph_px_no:
movs r0, #0
pop {r1-r2,pc}
.ltorg

/* ----- ph_scan: [r0]=payload -> r0 (1 = "hello-eth" in 64B) ------------
 * Own function: 64-position loop, all branches local. Clobbers r0-r2,r5
 * (caller's r4-r7 frame is preserved; r5 saved/restored here). */
.thumb_func
ph_scan:
push {r1-r2,r5,lr}
mov r2, r0
movs r5, #64
ph_ss_loop:
ldrb r1, [r2]
cmp r1, #104              /* h */
bne ph_ss_next
ldrb r1, [r2, #1]
cmp r1, #101              /* e */
bne ph_ss_next
ldrb r1, [r2, #2]
cmp r1, #108              /* l */
bne ph_ss_next
ldrb r1, [r2, #3]
cmp r1, #108              /* l */
bne ph_ss_next
ldrb r1, [r2, #4]
cmp r1, #111              /* o */
bne ph_ss_next
ldrb r1, [r2, #5]
cmp r1, #45               /* - */
bne ph_ss_next
ldrb r1, [r2, #6]
cmp r1, #101              /* e */
bne ph_ss_next
ldrb r1, [r2, #7]
cmp r1, #116              /* t */
bne ph_ss_next
ldrb r1, [r2, #8]
cmp r1, #104              /* h */
bne ph_ss_next
movs r0, #1               /* found */
pop {r1-r2,r5,pc}
ph_ss_next:
adds r2, #1
subs r5, #1
bne ph_ss_loop
movs r0, #0
pop {r1-r2,r5,pc}
.ltorg

/* ---------- wait_fin: poll RECV, parse FIN-ACK -> r0 (1/0) ------------ */
.thumb_func
wait_fin:
push {r4-r7,lr}
ldr r5, =0x2000000
wf_poll:
bl spi_cs_lo
movs r0, #0; bl spi_xfer_drain
movs r0, #S_IR; bl spi_xfer_drain
movs r0, #(BSB_REG<<3); bl spi_xfer_drain
movs r0, #0xFF; bl spi_xfer
mov r6, r0
bl spi_cs_hi
mov r0, r6
movs r1, #4
ands r0, r1
bne wf_got
subs r5, #1
bne wf_poll
movs r0, #0
pop {r4-r7,pc}
wf_got:
bl spi_cs_lo
movs r0, #0; bl spi_xfer_drain
movs r0, #0; bl spi_xfer_drain
movs r0, #(BSB_RX<<3); bl spi_xfer_drain
movs r0, #0xFF; bl spi_xfer
mov r6, r0
movs r0, #0xFF; bl spi_xfer
mov r7, r0
lsls r6, r6, #8
adds r6, r6, r7           /* stored len (frame + 2) */
subs r6, #2               /* frame len (prefix includes self) */
ldr r4, =RXBUF
mov r5, r6
wf_rxcopy:
cmp r5, #0
beq wf_rxdone
movs r0, #0xFF; bl spi_xfer
strb r0, [r4]
adds r4, #1
subs r5, #1
b wf_rxcopy
wf_rxdone:
bl spi_cs_hi
bl spi_cs_lo
movs r0, #0; bl spi_xfer_drain
movs r0, #S_CR; bl spi_xfer_drain
movs r0, #(BSB_REG<<3)|4; bl spi_xfer_drain
movs r0, #0x40; bl spi_xfer_drain
bl spi_cs_hi
bl parse_fin
cmp r0, #0
bne wf_done
ldr r5, =0x1000000
b wf_poll
wf_done:
pop {r4-r7,pc}
.ltorg

/* ---------- parse_fin: RXBUF -> r0 (1 = FIN-ACK for us, else 0) -------
 * TCP from :80, FIN set, seq = SSEQ+67, ack = cseq+19. */
.thumb_func
parse_fin:
push {r4-r7,lr}
ldr r0, =RXBUF
adds r0, #7
adds r0, #5               /* +12 ethertype */
ldrb r1, [r0]
cmp r1, #8
bne pf_no
adds r0, #1
ldrb r1, [r0]
cmp r1, #0
bne pf_no
adds r0, #1               /* +14 IP */
adds r0, #7
adds r0, #2               /* +23 proto */
ldrb r1, [r0]
cmp r1, #6
bne pf_no
adds r0, #3               /* +26 src IP */
ldrb r1, [r0]
cmp r1, #192
bne pf_no
adds r0, #1
ldrb r1, [r0]
cmp r1, #168
bne pf_no
adds r0, #1
ldrb r1, [r0]
cmp r1, #4
bne pf_no
adds r0, #1
ldrb r1, [r0]
cmp r1, #1
bne pf_no
adds r0, #5               /* +34 sport */
ldrb r1, [r0]
cmp r1, #0
bne pf_no
adds r0, #1
ldrb r1, [r0]
cmp r1, #80
bne pf_no
adds r0, #1               /* +36 dport */
__SPORT_HI_CMP__
__SPORT_LO_CMP__
bne pf_no
adds r0, #1               /* +38 seq = SSEQ+67 */
ldrb r1, [r0]
cmp r1, #0
bne pf_no
adds r0, #1
ldrb r1, [r0]
cmp r1, #16
bne pf_no
adds r0, #1
ldrb r1, [r0]
cmp r1, #0
bne pf_no
adds r0, #1
ldrb r1, [r0]
cmp r1, #67
bne pf_no
adds r0, #1               /* +42 ack = cseq+19 */
__ACK19_CMP__
adds r0, #2               /* +47 flags */
ldrb r1, [r0]
movs r2, #1
ands r1, r2
beq pf_no                 /* FIN must be set */
movs r0, #1
pop {r4-r7,pc}
pf_no:
movs r0, #0
pop {r4-r7,pc}
.ltorg
"""

ARM_HTTP_STRINGS = """msg_http_start: .asciz "ETH HTTP-START\\n"
msg_arp_ok: .asciz "ETH ARP-OK\\n"
msg_synack_ok: .asciz "ETH SYNACK-OK\\n"
msg_http_ok: .asciz "ETH HTTP-OK\\n"
msg_http_done: .asciz "ETH HTTP-DONE\\n"
msg_fail_arp: .asciz "ETH FAIL ARP-TIMEOUT\\n"
msg_fail_syn: .asciz "ETH FAIL SYN-TIMEOUT\\n"
msg_fail_http: .asciz "ETH FAIL HTTP-TIMEOUT\\n"
msg_fail_fin: .asciz "ETH FAIL FIN-TIMEOUT\\n"
"""


HTTP_ARCHES_LO = {}  # filled in arm_http_source (sport lo per arch)


def arm_http_source():
    """Full eth_http.S: resolved eth_dhcp ARM source + HTTP stage."""
    for tag in ("m0", "m33", "rv32"):
        HTTP_ARCHES_LO[tag] = __import__("eth_http_common").HTTP_ARCHES[tag]["sport"] & 0xFF
    import eth_http_common as H  # noqa: F841 (kept for blob tables below)
    src = gen_eth_dhcp.arm_source()
    # 1. run http after dhcp in reset_handler.
    old = "bl dhcp_run\nhalt: b halt"
    assert src.count(old) == 1, src.count(old)
    src = src.replace(old, "bl dhcp_run\nbl http_run\nhalt: b halt")
    # 2. per-arch const blocks for the placeholder compares.
    m0c1 = _be4(_cseq_plus(gen_eth_dhcp.ARCHES["m0"]["xid"], 1))
    m33c1 = _be4(_cseq_plus(gen_eth_dhcp.ARCHES["m33"]["xid"], 1))
    m0c19 = _be4(_cseq_plus(gen_eth_dhcp.ARCHES["m0"]["xid"], 19))
    m33c19 = _be4(_cseq_plus(gen_eth_dhcp.ARCHES["m33"]["xid"], 19))

    def cmp4(m0b, m33b, nolabel):
        lines = []
        for i, (b0, b1) in enumerate(zip(m0b, m33b)):
            lines.append("ldrb r1, [r0]")
            lines.append("#ifdef RP2350\ncmp r1, #0x%02X\n#else\ncmp r1, #0x%02X\n#endif"
                         % (b1, b0))
            lines.append("bne %s" % nolabel)
            if i < 3:
                lines.append("adds r0, #1")
        return "\n".join(lines)

    sport_hi = "ldrb r1, [r0]\ncmp r1, #225\nbne XX_NO\nadds r0, #1"
    sport_lo = ("ldrb r1, [r0]\n#ifdef RP2350\ncmp r1, #0x%02X\n#else\ncmp r1, #0x%02X\n#endif"
                % (0x01, 0x00))  # sport lo: m33=0xE101 / m0=0xE100
    code = ARM_HTTP_RUN
    for _nolabel, _tag in (("pn_no", "n"), ("ph_no", "h"), ("pf_no", "f")):
        hi = sport_hi.replace("XX_NO", _nolabel)
        assert code.count("__SPORT_HI_CMP__") >= 1
        code = code.replace("__SPORT_HI_CMP__", hi, 1)
    assert code.count("__SPORT_HI_CMP__") == 0
    # NOTE: NO trailing adds here: the template already has
    # "__SPORT_LO_CMP__\nbne <no>\nadds r0, #1" (+38), so appending
    # another adds would double-advance to +39 and shift every later
    # field by one (SYN-ACK parse failed on seq[0]=0x10, see history).
    for _nolabel in ("pn_no", "ph_no", "pf_no"):
        assert code.count("__SPORT_LO_CMP__") >= 1
        code = code.replace("__SPORT_LO_CMP__", sport_lo, 1)
    assert code.count("__SPORT_LO_CMP__") == 0
    assert code.count("__ACK_CMP__") == 1
    code = code.replace("__ACK_CMP__", cmp4(m0c1, m33c1, "pn_no"))
    assert code.count("__ACK19_CMP__") == 2, code.count("__ACK19_CMP__")
    # pn_no/ph_no/pf_no differ per parser: split replace in order.
    parts = code.split("__ACK19_CMP__")
    assert len(parts) == 3, len(parts)
    code = (parts[0] + cmp4(m0c19, m33c19, "ph_no") + parts[1] +
            cmp4(m0c19, m33c19, "pf_no") + parts[2])
    # 3. insert after the dhcp_fail block.
    old = "dhcp_fail:\nldr r0, =msg_fail; bl print_string\npop {r4-r7,pc}\n.ltorg"
    assert src.count(old) == 1, src.count(old)
    src = src.replace(old, old + "\n" + code)
    # 4. strings after msg_done.
    old = 'msg_done: .asciz "ETH DONE\\n"'
    assert src.count(old) == 1, src.count(old)
    src = src.replace(old, old + "\n" + ARM_HTTP_STRINGS.rstrip("\n"))
    # 5. blobs: per-arch ifdef blocks for the 7 TX frames, after the
    # M33 req blob block (i.e. before mac_addr_m33).
    blobs = []
    datas = {}
    for tag in ("M0", "M33"):
        a = H.HTTP_ARCHES[tag.lower()]
        datas[tag] = http_blobs(tag, a["mac"], a["xid"], a["sport"])
    for tag in ("M0", "M33"):
        sel = "#ifdef RP2350" if tag == "M33" else "#ifndef RP2350"
        blobs.append(sel)
        for name, blob in datas[tag]:
            blobs.append(f"{name}:  /* {tag} {name} {len(blob)}B */")
            blobs.append(blob_lines(blob))
        blobs.append("#endif")
    old = "mac_addr_m33: .byte "
    assert src.count(old) == 1, src.count(old)
    src = src.replace(old, "\n".join(blobs) + "\n" + old)
    return src


def rv32_http_source():
    """Full eth_http_rv32.S: gen_eth_dhcp.rv32_source() + HTTP stage.

    Same string-surgery approach as the ARM side: take the proven RV32
    DORA source verbatim, hook rv_http_run after rv_dhcp_run, insert the
    HTTP funcs before the print helpers, extend strings + blobs.
    Per-arch consts are hardcoded (single-arch build): sport lo,
    cseq+1/cseq+19 BE bytes, SSEQ words from eth_http_common.
    """
    import eth_http_common as H
    a = H.HTTP_ARCHES["rv32"]
    mac, xid, sport = a["mac"], a["xid"], a["sport"]
    cseq = H.cseq_of(xid)
    c1 = [(cseq + 1) >> 24 & 0xFF, (cseq + 1) >> 16 & 0xFF,
          (cseq + 1) >> 8 & 0xFF, (cseq + 1) & 0xFF]
    c19 = [(cseq + 19) >> 24 & 0xFF, (cseq + 19) >> 16 & 0xFF,
           (cseq + 19) >> 8 & 0xFF, (cseq + 19) & 0xFF]
    sport_lo = sport & 0xFF
    blobs = http_blobs("rv32", mac, xid, sport)

    base = gen_eth_dhcp.rv32_source()
    L = []
    A = L.append

    # 1. hook rv_http_run after rv_dhcp_run in _start — but ONLY on the
    # success path. rv_dhcp_run returns normally after ETH DONE; on DHCP
    # failure it prints FAIL and returns too, so gate on re-check? No:
    # the DHCP side already prints FAIL; http_run's waiters would then
    # spin the full poll budget. Simplest correct: rv_dhcp_run returns
    # void, so check the DHCP result via a flag? Instead: http_run runs
    # unconditionally (like ARM: bl dhcp_run; bl http_run) — matches ARM
    # and keeps both arches identical. DHCP failure just cascades to an
    # HTTP timeout, which is the honest signal.
    # NOTE: base is the *rendered* source, not the generator; patch text.
    old = "    jal ra, rv_dhcp_run\n"
    assert base.count(old) == 1, base.count(old)
    base = base.replace(old, old + "    jal ra, rv_http_run\n")

    # 2. HTTP funcs: inserted before rv_print_string definition.
    H_FUNCS = []
    H_A = H_FUNCS.append
    H_A("rv_http_run:")
    H_A("    addi sp, sp, -32")
    H_A("    sw ra, 28(sp)")
    H_A("    sw s0, 24(sp)")
    H_A("    sw s1, 20(sp)")
    H_A("    sw s2, 16(sp)")
    H_A(".Lr20: auipc a0, %pcrel_hi(.Ls12)")
    H_A("    addi a0, a0, %pcrel_lo(.Lr20)")
    H_A("    jal ra, rv_print_string")
    H_A(".Lr21: auipc a0, %pcrel_hi(arp_blob)")
    H_A("    addi a0, a0, %pcrel_lo(.Lr21)")
    H_A("    addi a1, zero, 42")
    H_A("    jal ra, rv_macraw_send")
    H_A("    jal ra, rv_wait_arp           # a0 = 1 match / 0")
    H_A("    bnez a0, rv_h_got_arp")
    H_A(".Lr22: auipc a0, %pcrel_hi(.Ls16)")
    H_A("    addi a0, a0, %pcrel_lo(.Lr22)")
    H_A("    jal ra, rv_print_string")
    H_A("    j rv_http_fail_ret")
    H_A("rv_h_got_arp:")
    H_A(".Lr23: auipc a0, %pcrel_hi(.Ls13)")
    H_A("    addi a0, a0, %pcrel_lo(.Lr23)")
    H_A("    jal ra, rv_print_string")
    H_A(".Lr24: auipc a0, %pcrel_hi(syn_blob)")
    H_A("    addi a0, a0, %pcrel_lo(.Lr24)")
    H_A("    addi a1, zero, 54")
    H_A("    jal ra, rv_macraw_send")
    H_A("    jal ra, rv_wait_synack")
    H_A("    bnez a0, rv_h_got_syn")
    H_A(".Lr25: auipc a0, %pcrel_hi(.Ls17)")
    H_A("    addi a0, a0, %pcrel_lo(.Lr25)")
    H_A("    jal ra, rv_print_string")
    H_A("    j rv_http_fail_ret")
    H_A("rv_h_got_syn:")
    H_A(".Lr26: auipc a0, %pcrel_hi(.Ls14)")
    H_A("    addi a0, a0, %pcrel_lo(.Lr26)")
    H_A("    jal ra, rv_print_string")
    H_A(".Lr27: auipc a0, %pcrel_hi(ack_blob)")
    H_A("    addi a0, a0, %pcrel_lo(.Lr27)")
    H_A("    addi a1, zero, 54")
    H_A("    jal ra, rv_macraw_send")
    H_A(".Lr28: auipc a0, %pcrel_hi(get_blob)")
    H_A("    addi a0, a0, %pcrel_lo(.Lr28)")
    H_A("    addi a1, zero, 72")
    H_A("    jal ra, rv_macraw_send")
    H_A("    jal ra, rv_wait_http")
    H_A("    bnez a0, rv_h_got_http")
    H_A(".Lr29: auipc a0, %pcrel_hi(.Ls18)")
    H_A("    addi a0, a0, %pcrel_lo(.Lr29)")
    H_A("    jal ra, rv_print_string")
    H_A("    j rv_http_fail_ret")
    H_A("rv_h_got_http:")
    H_A(".Lr30: auipc a0, %pcrel_hi(.Ls15)")
    H_A("    addi a0, a0, %pcrel_lo(.Lr30)")
    H_A("    jal ra, rv_print_string")
    H_A(".Lr31: auipc a0, %pcrel_hi(ack2_blob)")
    H_A("    addi a0, a0, %pcrel_lo(.Lr31)")
    H_A("    addi a1, zero, 54")
    H_A("    jal ra, rv_macraw_send")
    H_A(".Lr32: auipc a0, %pcrel_hi(fin_blob)")
    H_A("    addi a0, a0, %pcrel_lo(.Lr32)")
    H_A("    addi a1, zero, 54")
    H_A("    jal ra, rv_macraw_send")
    H_A("    jal ra, rv_wait_fin")
    H_A("    bnez a0, rv_h_got_fin")
    H_A(".Lr33: auipc a0, %pcrel_hi(.Ls19)")
    H_A("    addi a0, a0, %pcrel_lo(.Lr33)")
    H_A("    jal ra, rv_print_string")
    H_A("    j rv_http_fail_ret")
    H_A("rv_h_got_fin:")
    H_A(".Lr34: auipc a0, %pcrel_hi(fack_blob)")
    H_A("    addi a0, a0, %pcrel_lo(.Lr34)")
    H_A("    addi a1, zero, 54")
    H_A("    jal ra, rv_macraw_send")
    H_A(".Lr35: auipc a0, %pcrel_hi(.Ls20)")
    H_A("    addi a0, a0, %pcrel_lo(.Lr35)")
    H_A("    jal ra, rv_print_string")
    H_A("    lw ra, 28(sp)")
    H_A("    lw s0, 24(sp)")
    H_A("    lw s1, 20(sp)")
    H_A("    lw s2, 16(sp)")
    H_A("    addi sp, sp, 32")
    H_A("    ret")
    H_A("rv_http_fail_ret:")
    H_A("    lw ra, 28(sp)")
    H_A("    lw s0, 24(sp)")
    H_A("    lw s1, 20(sp)")
    H_A("    lw s2, 16(sp)")
    H_A("    addi sp, sp, 32")
    H_A("    ret")
    # --- generic waiter bodies: same poll/read/consume skeleton as
    # rv_wait_offer, parameterized by parse callee + fail label prefix.
    # t-reg discipline identical (s0 keeps RECV bit across cs_hi).
    for tag, parse in (("arp", "rv_parse_arp"), ("synack", "rv_parse_synack"),
                       ("http", "rv_parse_http"), ("fin", "rv_parse_fin")):
        H_A(f"rv_wait_{tag}:")
        H_A("    addi sp, sp, -32")
        H_A("    sw ra, 28(sp)")
        H_A("    sw s0, 24(sp)")
        H_A("    sw s1, 20(sp)")
        H_A("    sw s2, 16(sp)")
        H_A("    lui s2, 0x200")
        H_A(f"rv_wo_{tag}_poll:")
        H_A("    jal ra, rv_spi_cs_lo")
        H_A("    addi a0, zero, 0")
        H_A("    jal ra, rv_spi_xfer")
        H_A("    addi a0, zero, 2")
        H_A("    jal ra, rv_spi_xfer")
        H_A("    addi a0, zero, 8")
        H_A("    jal ra, rv_spi_xfer")
        H_A("    addi a0, zero, 0xFF")
        H_A("    jal ra, rv_spi_xfer")
        H_A("    andi s0, a0, 4")
        H_A("    jal ra, rv_spi_cs_hi")
        H_A(f"    bnez s0, rv_wo_{tag}_got")
        H_A("    addi s2, s2, -1")
        H_A(f"    bnez s2, rv_wo_{tag}_poll")
        H_A("    addi a0, zero, 0")
        H_A(f"    j rv_wo_{tag}_done")
        H_A(f"rv_wo_{tag}_got:")
        H_A("    jal ra, rv_spi_cs_lo")
        H_A("    addi a0, zero, 0")
        H_A("    jal ra, rv_spi_xfer")
        H_A("    addi a0, zero, 0")
        H_A("    jal ra, rv_spi_xfer")
        H_A("    addi a0, zero, 0x18")
        H_A("    jal ra, rv_spi_xfer")
        H_A("    addi a0, zero, 0xFF")
        H_A("    jal ra, rv_spi_xfer")
        H_A("    mv s0, a0")
        H_A("    addi a0, zero, 0xFF")
        H_A("    jal ra, rv_spi_xfer")
        H_A("    mv s1, a0")
        H_A("    slli s0, s0, 8")
        H_A("    add s0, s0, s1             # stored len (frame + 2)")
        H_A("    addi s0, s0, -2            # frame len (prefix includes self)")
        H_A("    lui t0, 0x20042")
        H_A("    addi t0, t0, -0x200")
        H_A("    mv s1, t0")
        H_A("    mv s2, s0")
        H_A(f"rv_{tag}_rx_copy:")
        H_A(f"    beqz s2, rv_{tag}_rx_done")
        H_A("    addi a0, zero, 0xFF")
        H_A("    jal ra, rv_spi_xfer")
        H_A("    sb a0, 0(s1)")
        H_A("    addi s1, s1, 1")
        H_A("    addi s2, s2, -1")
        H_A(f"    j rv_{tag}_rx_copy")
        H_A(f"rv_{tag}_rx_done:")
        H_A("    jal ra, rv_spi_cs_hi")
        H_A("    jal ra, rv_spi_cs_lo")
        H_A("    addi a0, zero, 0")
        H_A("    jal ra, rv_spi_xfer")
        H_A("    addi a0, zero, 1")
        H_A("    jal ra, rv_spi_xfer")
        H_A("    addi a0, zero, 0x0C")
        H_A("    jal ra, rv_spi_xfer")
        H_A("    addi a0, zero, 0x40")
        H_A("    jal ra, rv_spi_xfer")
        H_A("    jal ra, rv_spi_cs_hi")
        H_A(f"    jal ra, {parse}")
        H_A(f"    bnez a0, rv_wo_{tag}_done")
        H_A("    lui s2, 0x100")
        H_A(f"    j rv_wo_{tag}_poll")
        H_A(f"rv_wo_{tag}_done:")
        H_A("    lw ra, 28(sp)")
        H_A("    lw s0, 24(sp)")
        H_A("    lw s1, 20(sp)")
        H_A("    lw s2, 16(sp)")
        H_A("    addi sp, sp, 32")
        H_A("    ret")
    # --- parsers (byte-at-a-time against RXBUF, lbu only) ---
    H_A("rv_parse_arp:")
    H_A("    lui t0, 0x20042")
    H_A("    addi t0, t0, -0x200")
    H_A("    lbu t1, 12(t0)")
    H_A("    addi t2, zero, 8")
    H_A("    bne t1, t2, rv_pa_no")
    H_A("    lbu t1, 13(t0)")
    H_A("    addi t2, zero, 6")
    H_A("    bne t1, t2, rv_pa_no")
    H_A("    lbu t1, 20(t0)")
    H_A("    bnez t1, rv_pa_no")
    H_A("    lbu t1, 21(t0)")
    H_A("    addi t2, zero, 2")
    H_A("    bne t1, t2, rv_pa_no")
    H_A("    lbu t1, 28(t0)")
    H_A("    addi t2, zero, 192")
    H_A("    bne t1, t2, rv_pa_no")
    H_A("    lbu t1, 29(t0)")
    H_A("    addi t2, zero, 168")
    H_A("    bne t1, t2, rv_pa_no")
    H_A("    lbu t1, 30(t0)")
    H_A("    addi t2, zero, 4")
    H_A("    bne t1, t2, rv_pa_no")
    H_A("    lbu t1, 31(t0)")
    H_A("    addi t2, zero, 1")
    H_A("    bne t1, t2, rv_pa_no")
    H_A("    addi a0, zero, 1")
    H_A("    ret")
    H_A("rv_pa_no:")
    H_A("    addi a0, zero, 0")
    H_A("    ret")

    def _tcp_head(kind):
        # shared ethertype/proto/src-ip/sport/dport prefix per parser.
        # kind: "n" synack, "h" http, "f" fin. Emits entry label
        # rv_parse_{synack,http,fin} + checks; fail label rv_p{kind}_no.
        _names = {"n": "rv_parse_synack", "h": "rv_parse_http",
                  "f": "rv_parse_fin"}
        H_A(_names[kind] + ":")
        H_A("    addi sp, sp, -16")
        H_A("    sw ra, 12(sp)")
        H_A("    sw s0, 8(sp)")
        H_A("    lui t0, 0x20042")
        H_A("    addi t0, t0, -0x200")
        H_A("    lbu t1, 12(t0)")
        H_A("    addi t2, zero, 8")
        H_A(f"    bne t1, t2, rv_p{kind}_no")
        H_A("    lbu t1, 13(t0)")
        H_A(f"    bnez t1, rv_p{kind}_no")
        H_A("    lbu t1, 23(t0)")
        H_A("    addi t2, zero, 6")
        H_A(f"    bne t1, t2, rv_p{kind}_no")
        for off, val in ((26, 192), (27, 168), (28, 4), (29, 1)):
            H_A(f"    lbu t1, {off}(t0)")
            if val >= 128:
                H_A(f"    xori t2, t1, {val}")
                H_A(f"    bnez t2, rv_p{kind}_no")
            else:
                H_A(f"    addi t2, zero, {val}")
                H_A(f"    bne t1, t2, rv_p{kind}_no")
        H_A("    lbu t1, 34(t0)")
        H_A(f"    bnez t1, rv_p{kind}_no")
        H_A("    lbu t1, 35(t0)")
        H_A("    addi t2, zero, 80")
        H_A(f"    bne t1, t2, rv_p{kind}_no")
        H_A("    lbu t1, 36(t0)")
        H_A("    addi t2, zero, 225")
        H_A(f"    bne t1, t2, rv_p{kind}_no")
        H_A("    lbu t1, 37(t0)")
        H_A(f"    addi t2, zero, {sport_lo}")
        H_A(f"    bne t1, t2, rv_p{kind}_no")

    def _be4_lines(base, delta):
        v = [(cseq + delta) >> 24 & 0xFF, (cseq + delta) >> 16 & 0xFF,
             (cseq + delta) >> 8 & 0xFF, (cseq + delta) & 0xFF]
        out = []
        for i, b in enumerate(v):
            out.append((base + i, b))
        return out

    # parse_synack: seq SSEQ, ack cseq+1, flags 0x12
    _tcp_head("n")
    for off, b in _be4_lines(38, 0):
        pass  # SSEQ is 0x00100000: bytes 0,16,0,0
    H_A("    lbu t1, 38(t0)")
    H_A("    bnez t1, rv_pn_no")
    H_A("    lbu t1, 39(t0)")
    H_A("    addi t2, zero, 16")
    H_A("    bne t1, t2, rv_pn_no")
    H_A("    lbu t1, 40(t0)")
    H_A("    bnez t1, rv_pn_no")
    H_A("    lbu t1, 41(t0)")
    H_A("    bnez t1, rv_pn_no")
    for off, b in _be4_lines(42, 1):
        H_A(f"    lbu t1, {off}(t0)")
        if b >= 128:
            H_A(f"    xori t2, t1, {b}")
            H_A("    bnez t2, rv_pn_no")
        else:
            H_A(f"    addi t2, zero, {b}")
            H_A("    bne t1, t2, rv_pn_no")
    H_A("    lbu t1, 47(t0)")
    H_A("    addi t2, zero, 18")
    H_A("    bne t1, t2, rv_pn_no")
    H_A("    addi a0, zero, 1")
    H_A("    lw ra, 12(sp)")
    H_A("    lw s0, 8(sp)")
    H_A("    addi sp, sp, 16")
    H_A("    ret")
    H_A("rv_pn_no:")
    H_A("    addi a0, zero, 0")
    H_A("    lw ra, 12(sp)")
    H_A("    lw s0, 8(sp)")
    H_A("    addi sp, sp, 16")
    H_A("    ret")
    # parse_http: seq SSEQ+1, ack cseq+19, PSH, prefix, body scan
    _tcp_head("h")
    H_A("    lbu t1, 38(t0)")
    H_A("    bnez t1, rv_ph_no")
    H_A("    lbu t1, 39(t0)")
    H_A("    addi t2, zero, 16")
    H_A("    bne t1, t2, rv_ph_no")
    H_A("    lbu t1, 40(t0)")
    H_A("    bnez t1, rv_ph_no")
    H_A("    lbu t1, 41(t0)")
    H_A("    addi t2, zero, 1")
    H_A("    bne t1, t2, rv_ph_no")
    for off, b in _be4_lines(42, 19):
        H_A(f"    lbu t1, {off}(t0)")
        if b >= 128:
            H_A(f"    xori t2, t1, {b}")
            H_A("    bnez t2, rv_ph_no")
        else:
            H_A(f"    addi t2, zero, {b}")
            H_A("    bne t1, t2, rv_ph_no")
    H_A("    lbu t1, 47(t0)")
    H_A("    andi t2, t1, 8")
    H_A("    beqz t2, rv_ph_no")
    H_A("    addi t0, t0, 54")
    H_A("    mv s0, t0                # save payload ptr (prefix clobbers a0)")
    H_A("    mv a0, t0")
    H_A("    jal ra, rv_ph_prefix")
    H_A("    beqz a0, rv_ph_no")
    H_A("    mv a0, s0                # restore payload ptr for body scan")
    H_A("    jal ra, rv_ph_scan")
    H_A("    beqz a0, rv_ph_no")
    H_A("    addi a0, zero, 1")
    H_A("    lw ra, 12(sp)")
    H_A("    lw s0, 8(sp)")
    H_A("    addi sp, sp, 16")
    H_A("    ret")
    H_A("rv_ph_no:")
    H_A("    addi a0, zero, 0")
    H_A("    lw ra, 12(sp)")
    H_A("    lw s0, 8(sp)")
    H_A("    addi sp, sp, 16")
    H_A("    ret")
    H_A("rv_ph_prefix:")
    H_A("    mv t5, a0")
    for i, b in enumerate(b"HTTP/1.0 200"):
        H_A(f"    lbu t1, {i}(t5)")
        if b >= 128:
            H_A(f"    xori t2, t1, {b}")
            H_A("    bnez t2, rv_ph_px_no")
        else:
            H_A(f"    addi t2, zero, {b}")
            H_A("    bne t1, t2, rv_ph_px_no")
    H_A("    addi a0, zero, 1")
    H_A("    ret")
    H_A("rv_ph_px_no:")
    H_A("    addi a0, zero, 0")
    H_A("    ret")
    H_A("rv_ph_scan:")
    H_A("    mv t5, a0")
    H_A("    addi t6, zero, 64")
    H_A("rv_ph_ss_loop:")
    H_A("    lbu t1, 0(t5)")
    H_A("    addi t2, zero, 104")
    H_A("    bne t1, t2, rv_ph_ss_next")
    for i, b in enumerate(b"ello-eth", start=1):
        H_A(f"    lbu t1, {i}(t5)")
        H_A(f"    addi t2, zero, {b}")
        H_A("    bne t1, t2, rv_ph_ss_next")
    H_A("    addi a0, zero, 1")
    H_A("    ret")
    H_A("rv_ph_ss_next:")
    H_A("    addi t5, t5, 1")
    H_A("    addi t6, t6, -1")
    H_A("    bnez t6, rv_ph_ss_loop")
    H_A("    addi a0, zero, 0")
    H_A("    ret")
    # parse_fin: seq SSEQ+67, ack cseq+19, FIN set
    _tcp_head("f")
    H_A("    lbu t1, 38(t0)")
    H_A("    bnez t1, rv_pf_no")
    H_A("    lbu t1, 39(t0)")
    H_A("    addi t2, zero, 16")
    H_A("    bne t1, t2, rv_pf_no")
    H_A("    lbu t1, 40(t0)")
    H_A("    bnez t1, rv_pf_no")
    H_A("    lbu t1, 41(t0)")
    H_A("    addi t2, zero, 67")
    H_A("    bne t1, t2, rv_pf_no")
    for off, b in _be4_lines(42, 19):
        H_A(f"    lbu t1, {off}(t0)")
        if b >= 128:
            H_A(f"    xori t2, t1, {b}")
            H_A("    bnez t2, rv_pf_no")
        else:
            H_A(f"    addi t2, zero, {b}")
            H_A("    bne t1, t2, rv_pf_no")
    H_A("    lbu t1, 47(t0)")
    H_A("    andi t2, t1, 1")
    H_A("    beqz t2, rv_pf_no")
    H_A("    addi a0, zero, 1")
    H_A("    lw ra, 12(sp)")
    H_A("    lw s0, 8(sp)")
    H_A("    addi sp, sp, 16")
    H_A("    ret")
    H_A("rv_pf_no:")
    H_A("    addi a0, zero, 0")
    H_A("    lw ra, 12(sp)")
    H_A("    lw s0, 8(sp)")
    H_A("    addi sp, sp, 16")
    H_A("    ret")

    funcs = "\n".join(H_FUNCS)
    # insert before rv_print_string definition.
    old = "rv_print_string:                # a0 = ptr"
    assert base.count(old) == 1, base.count(old)
    base = base.replace(old, funcs + "\n" + old)
    # strings .Ls12-20 after .Ls11 (rendered source text, not generator).
    old = '.Ls11: .asciz "ETH FAIL ACK-TIMEOUT\\n"'
    assert base.count(old) == 1, base.count(old)
    base = base.replace(
        old, old + "\n"
        '.Ls12: .asciz "ETH HTTP-START\\n"\n'
        '.Ls13: .asciz "ETH ARP-OK\\n"\n'
        '.Ls14: .asciz "ETH SYNACK-OK\\n"\n'
        '.Ls15: .asciz "ETH HTTP-OK\\n"\n'
        '.Ls20: .asciz "ETH HTTP-DONE\\n"\n'
        '.Ls16: .asciz "ETH FAIL ARP-TIMEOUT\\n"\n'
        '.Ls17: .asciz "ETH FAIL SYN-TIMEOUT\\n"\n'
        '.Ls18: .asciz "ETH FAIL HTTP-TIMEOUT\\n"\n'
        '.Ls19: .asciz "ETH FAIL FIN-TIMEOUT\\n"')
    # auipc refs .Lr20-35 must exist for the new strings. rv_http_run uses
    # .Lr20-35; the base only defines .Lr1-14, so append the refs inline:
    # (already emitted as .LrNN labels in H_FUNCS above — the auipc sites
    # ARE the .LrNN labels, matching the .LsNN string labels. No fixup
    # needed beyond ensuring rvlink sees both, which it does.)
    # blobs after req_blob block (rendered source text). NOTE: the base
    # source has NO .section directive before req_blob — .byte lines after
    # the rodata strings stay in .rodata only if we keep them there. The
    # base emits `.section .rodata` once, then strings, then the two DHCP
    # blobs; appending more .byte lines right after req_blob keeps them
    # in .rodata too (verified: no second .text before them).
    from eth_dhcp_common import (  # noqa: E402 (length anchor only)
        build_discover as _bd, build_request as _br)
    _disc = _bd(mac, xid)
    _req = _br(mac, xid, gen_eth_dhcp.OFFER_IP, gen_eth_dhcp.SERVER_IP)
    raw = []
    for name, blob in blobs:
        raw.append(f"{name}:")
        for i in range(0, len(blob), 12):
            raw.append("    .byte " + ", ".join(f"{b}"
                                                for b in blob[i:i + 12]))
    old = f"req_blob:   # REQUEST {len(_req)}B"
    assert base.count(old) == 1, base.count(old)
    # insert AFTER the last .byte line of req_blob: find the end of the
    # req blob block = start of rv_patch_request code? No — blobs live
    # after ALL code, at file end in .rodata. req_blob is the last thing
    # in .rodata, so appending at end of the rodata section is correct.
    # Simplest robust anchor: append after the final .byte run — i.e.
    # insert before the trailing newline: find last ".byte" line end.
    tail_anchor = base.rstrip("\n")
    assert tail_anchor.endswith(tuple("0123456789")), tail_anchor[-80:]
    base = tail_anchor + "\n" + "\n".join(raw) + "\n"
    return base


def main():
    arm = arm_http_source()
    open(os.path.join(D, "eth_http.S"), "w").write(arm)
    print("wrote", os.path.join(D, "eth_http.S"))
    rv = rv32_http_source()
    open(os.path.join(D, "eth_http_rv32.S"), "w").write(rv)
    print("wrote", os.path.join(D, "eth_http_rv32.S"))


if __name__ == "__main__":
    main()
