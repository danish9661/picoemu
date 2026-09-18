#!/usr/bin/env python3
"""vnet peer E2E test for eth_http (all 3 arches): speaks the vnet peer
protocol (4-byte LE len + ETH frame) over a Unix socket, runs the full
app exchange against the guest HTTP client:

  1. DHCP DORA (reuses dhcp_peer_test: DISCOVER->OFFER->REQUEST->ACK,
     same lease pool as the Go gateway: .2 / server .1)
  2. ARP who-has .1 -> reply (.1 is-at SRV_MAC)
  3. guest SYN .1:80 -> SYN-ACK (sseq=SSEQ)
  4. guest handshake ACK (no reply)
  5. guest GET / -> HTTP 200 hello-eth (PSH)
  6. guest ACK2 + FIN -> FIN-ACK
  7. guest final ACK (no reply)

Exit 0 + ALL HTTP CHECKS PASSED on success. Guest prints ETH HTTP-DONE.

Usage (two terminals):
  ./build/bramble web/eth_http.uf2 -board pico-eth \\
      -net-peer /tmp/httptest.sock
  python3 test-firmware/http_peer_test.py /tmp/httptest.sock

Per-arch identity (sport/cseq) comes from eth_http_common.HTTP_ARCHES;
the responder keys everything off the DHCP chaddr (like handleDHCP.go)
so all three guests can share one room without cross-talk.
"""
import os
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dhcp_peer_test import Peer, dhcp_reply, parse_dhcp, SRV_MAC, SRV_IP, LEASE_IP
from eth_http_common import (HTTP_ARCHES, SSEQ, HTTP_PAYLOAD,
                             parse_tcp_from_guest, is_arp_who_has,
                             arp_reply, tcp_seg_from_srv, ip_pkt_from_srv)

SOCK = sys.argv[1] if len(sys.argv) > 1 else "/tmp/httptest.sock"

ARCH_BY_MAC = {bytes(v["mac"]): (tag, v["sport"]) for tag, v in HTTP_ARCHES.items()}
ARCH_BY_SPORT = {v["sport"]: (tag, bytes(v["mac"])) for tag, v in HTTP_ARCHES.items()}


def main():
    for _ in range(100):
        try:
            p = Peer(SOCK)
            break
        except OSError:
            time.sleep(0.2)
    else:
        print("FAIL: cannot connect to", SOCK)
        return 1
    print("connected to", SOCK)

    # ---- 1. DORA (identical to dhcp_peer_test) ----
    def is_discover(f):
        r = parse_dhcp(f)
        if r and r[1] == 1:
            return r[0]
        return None

    bootp = p.wait_for(is_discover, 90, "DHCP DISCOVER")
    if bootp is None:
        print("FAIL: no DISCOVER")
        return 1
    gmac = bytes(bootp[28:34])
    print(f"PASS: DISCOVER xid={bootp[4:8].hex()} chaddr={gmac.hex()}")
    if gmac not in ARCH_BY_MAC:
        print(f"FAIL: unknown chaddr {gmac.hex()}")
        return 1
    tag, sport = ARCH_BY_MAC[gmac]
    print(f"PASS: arch={tag} sport={sport:#06x}")
    offer = bytearray(dhcp_reply(bootp, 2))
    offer[6:12] = SRV_MAC
    p.send(bytes(offer))
    print("sent OFFER 192.168.4.2")

    def is_request(f):
        r = parse_dhcp(f)
        if r and r[1] == 3:
            return r[0]
        return None

    bootp2 = p.wait_for(is_request, 90, "DHCP REQUEST")
    if bootp2 is None:
        print("FAIL: no REQUEST")
        return 1
    print(f"PASS: REQUEST xid={bootp2[4:8].hex()}")
    ack = bytearray(dhcp_reply(bootp2, 5))
    ack[6:12] = SRV_MAC
    p.send(bytes(ack))
    print("sent ACK 192.168.4.2")

    # ---- 2. ARP who-has .1 ----
    def is_arp(f):
        r = is_arp_who_has(f, gmac)
        if r:
            return r
        return None

    r = p.wait_for(is_arp, 90, "ARP who-has .1")
    if r is None:
        print("FAIL: no ARP")
        return 1
    print("PASS: ARP who-has 192.168.4.1")
    p.send(arp_reply(gmac, LEASE_IP))
    print("sent ARP reply (.1 is-at gateway MAC)")

    # ---- 3. SYN -> SYN-ACK ----
    def is_syn(f):
        t = parse_tcp_from_guest(f, sport)
        if t and t["flags"] == 0x02 and t["ack"] == 0 and t["payload"] == b"":
            return t
        return None

    t = p.wait_for(is_syn, 90, "SYN :80")
    if t is None:
        print("FAIL: no SYN")
        return 1
    cseq = t["seq"]
    print(f"PASS: SYN seq={cseq:#010x} sport={sport:#06x}")
    p.send(ip_pkt_from_srv(gmac, tcp_seg_from_srv(sport, SSEQ, (cseq + 1) & 0xFFFFFFFF, 0x12)))
    print(f"sent SYN-ACK seq={SSEQ:#010x} ack={(cseq + 1) & 0xFFFFFFFF:#010x}")

    # ---- 4. handshake ACK (no reply) ----
    def is_hack(f):
        t = parse_tcp_from_guest(f, sport)
        if (t and t["flags"] == 0x10 and t["seq"] == (cseq + 1) & 0xFFFFFFFF
                and t["ack"] == (SSEQ + 1) & 0xFFFFFFFF and t["payload"] == b""):
            return t
        return None

    t = p.wait_for(is_hack, 90, "handshake ACK")
    if t is None:
        print("FAIL: no handshake ACK")
        return 1
    print("PASS: handshake ACK")

    # ---- 5. GET -> HTTP 200 hello-eth ----
    def is_get(f):
        t = parse_tcp_from_guest(f, sport)
        if (t and (t["flags"] & 0x18) == 0x18 and t["seq"] == (cseq + 1) & 0xFFFFFFFF
                and t["ack"] == (SSEQ + 1) & 0xFFFFFFFF
                and t["payload"] == b"GET / HTTP/1.0\r\n\r\n"):
            return t
        return None

    t = p.wait_for(is_get, 90, "GET /")
    if t is None:
        print("FAIL: no GET")
        return 1
    print(f"PASS: GET / ({len(t['payload'])}B)")
    p.send(ip_pkt_from_srv(
        gmac, tcp_seg_from_srv(sport, (SSEQ + 1) & 0xFFFFFFFF,
                               (cseq + 19) & 0xFFFFFFFF, 0x18, HTTP_PAYLOAD)))
    print(f"sent HTTP 200 hello-eth ({len(HTTP_PAYLOAD)}B payload)")

    # ---- 6. ACK2 + FIN -> FIN-ACK ----
    def is_ack2(f):
        t = parse_tcp_from_guest(f, sport)
        if (t and t["flags"] == 0x10 and t["seq"] == (cseq + 19) & 0xFFFFFFFF
                and t["ack"] == (SSEQ + 67) & 0xFFFFFFFF and t["payload"] == b""):
            return t
        return None

    t = p.wait_for(is_ack2, 90, "ACK of HTTP data")
    if t is None:
        print("FAIL: no ACK2")
        return 1
    print("PASS: ACK of HTTP data")

    def is_fin(f):
        t = parse_tcp_from_guest(f, sport)
        if (t and (t["flags"] & 0x11) == 0x11 and t["seq"] == (cseq + 19) & 0xFFFFFFFF
                and t["ack"] == (SSEQ + 67) & 0xFFFFFFFF):
            return t
        return None

    t = p.wait_for(is_fin, 90, "FIN")
    if t is None:
        print("FAIL: no FIN")
        return 1
    print("PASS: FIN")
    p.send(ip_pkt_from_srv(
        gmac, tcp_seg_from_srv(sport, (SSEQ + 67) & 0xFFFFFFFF,
                               (cseq + 19) & 0xFFFFFFFF, 0x11)))
    print("sent FIN-ACK")

    # ---- 7. final ACK (no reply) ----
    def is_fack(f):
        t = parse_tcp_from_guest(f, sport)
        if (t and t["flags"] == 0x10 and t["seq"] == (cseq + 20) & 0xFFFFFFFF
                and t["ack"] == (SSEQ + 68) & 0xFFFFFFFF and t["payload"] == b""):
            return t
        return None

    t = p.wait_for(is_fack, 90, "final ACK")
    if t is None:
        print("FAIL: no final ACK")
        return 1
    print("PASS: final ACK")
    print("ALL HTTP CHECKS PASSED (guest should print ETH HTTP-DONE)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
