#!/usr/bin/env python3
"""vnet peer E2E test for webserver_rv32: speaks the vnet peer protocol
(4-byte LE len + ETH frame) over a Unix socket, runs ARP->SYN->ACK->GET->FIN
against the guest HTTP server (.100:80), verifies checksums + HTTP body.

Usage (two terminals):
  ./build/bramble web/wifi_webserver_rv32.uf2 -clock 125 -wifi \\
      -net -net-peer /tmp/wstest.sock
  python3 test-firmware/ws_peer_test.py /tmp/wstest.sock
Exit 0 + ALL PEER CHECKS PASSED on success.
"""
import socket
import struct
import sys
import time

SOCK = sys.argv[1] if len(sys.argv) > 1 else "/tmp/wstest.sock"
DEV_MAC = bytes([0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE])
DEV_IP = bytes([192, 168, 4, 100])
MY_MAC = bytes([0x02, 0x12, 0x34, 0x56, 0x78, 0x02])
MY_IP = bytes([192, 168, 4, 2])
SPORT = 54321
DPORT = 80


def csum(data):
    s = 0
    for i in range(0, len(data) - 1, 2):
        s += (data[i] << 8) + data[i + 1]
    if len(data) & 1:
        s += data[-1] << 8
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def verify_ip(ip):
    return csum(ip) == 0


def verify_tcp(src, dst, seg):
    pseudo = src + dst + bytes([0, 6]) + struct.pack(">H", len(seg))
    return csum(pseudo + seg) == 0


def eth(dst, src, etype, payload):
    return dst + src + struct.pack(">H", etype) + payload


def arp_req():
    return eth(bytes([0xFF] * 6), MY_MAC, 0x0806,
               struct.pack(">HHBBH", 1, 0x0800, 6, 4, 1) + MY_MAC + MY_IP +
               bytes(6) + DEV_IP)


def tcp_seg(seq, ack, flags, payload=b""):
    seg = struct.pack(">HHIIBBHHH", SPORT, DPORT, seq, ack, 5 << 4, flags,
                      8192, 0, 0)
    tcplen = len(seg) + len(payload)
    pseudo = MY_IP + DEV_IP + bytes([0, 6]) + struct.pack(">H", tcplen)
    cs = csum(pseudo + seg + payload)
    seg = seg[:16] + struct.pack(">H", cs) + seg[18:]
    return seg + payload


def ip_pkt(seg):
    ip = bytearray([0x45, 0]) + struct.pack(">H", 20 + len(seg))
    ip += struct.pack(">HHHH", 0x1234, 0, 64 * 256 + 6, 0) + MY_IP + DEV_IP
    cs = csum(bytes(ip))
    ip[10], ip[11] = (cs >> 8) & 0xFF, cs & 0xFF
    return eth(DEV_MAC, MY_MAC, 0x0800, bytes(ip) + seg)


class Peer:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(0.5)
        self.s.connect(path)
        self.buf = b""

    def send(self, frame):
        self.s.sendall(struct.pack("<I", len(frame)) + frame)

    def recv_all(self, deadline):
        out = []
        while time.time() < deadline:
            try:
                chunk = self.s.recv(65536)
            except socket.timeout:
                continue
            if not chunk:
                break
            self.buf += chunk
            while len(self.buf) >= 4:
                (ln,) = struct.unpack("<I", self.buf[:4])
                if len(self.buf) < 4 + ln:
                    break
                out.append(self.buf[4:4 + ln])
                self.buf = self.buf[4 + ln:]
                if out:
                    return out
        return out

    def wait_for(self, pred, timeout, desc):
        deadline = time.time() + timeout
        while time.time() < deadline:
            for f in self.recv_all(deadline):
                r = pred(f)
                if r is not None:
                    return r
        print(f"TIMEOUT waiting for {desc}")
        return None


def parse_tcp(f):
    if len(f) < 14 + 20 + 20:
        return None
    if f[12:14] != b"\x08\x00":
        return None
    ip = f[14:]
    if ip[9] != 6:
        return None
    ihl = (ip[0] & 0xF) * 4
    if not verify_ip(ip[:ihl]):
        print("WARN: bad IP checksum")
        return None
    iplen = (ip[2] << 8) + ip[3]
    seg = ip[ihl:ihl + (iplen - ihl)]  # exclude ETH pad (RFC: TCP len from IP total)
    if len(seg) < 20:
        return None
    if not verify_tcp(ip[12:16], ip[16:20], seg):
        print("WARN: bad TCP checksum")
        return None
    sport, dport, seq, ack, off, flags = struct.unpack(">HHIIBB", seg[:14])
    hlen = (off >> 4) * 4
    return {"sport": sport, "dport": dport, "seq": seq, "ack": ack,
            "flags": flags, "payload": seg[hlen:]}


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

    # 1. ARP
    p.send(arp_req())
    print("sent ARP who-has .100")

    def is_arp_reply(f):
        if len(f) >= 42 and f[12:14] == b"\x08\x06" and f[20:22] == b"\x00\x02":
            if f[28:32] == DEV_IP and f[32:38] == MY_MAC:
                return f
        return None

    r = p.wait_for(is_arp_reply, 60, "ARP reply")
    if r is None:
        print("FAIL: no ARP reply")
        return 1
    print("PASS: ARP reply from", r[22:28].hex())

    # 2. SYN
    cseq = 0x11223344
    p.send(ip_pkt(tcp_seg(cseq, 0, 0x02)))
    print("sent SYN")

    def is_synack(f):
        t = parse_tcp(f)
        if t and t["flags"] == 0x12 and t["ack"] == (cseq + 1) & 0xFFFFFFFF:
            return t
        return None

    t = p.wait_for(is_synack, 60, "SYN-ACK")
    if t is None:
        print("FAIL: no SYN-ACK")
        return 1
    print(f"PASS: SYN-ACK seq={t['seq']:#x} ack={t['ack']:#x}")
    sseq = t["seq"]

    # 3. ACK
    p.send(ip_pkt(tcp_seg((cseq + 1) & 0xFFFFFFFF, (sseq + 1) & 0xFFFFFFFF,
                          0x10)))
    print("sent handshake ACK")

    # 4. GET
    get = b"GET / HTTP/1.0\r\n\r\n"
    cseq1 = (cseq + 1) & 0xFFFFFFFF
    p.send(ip_pkt(tcp_seg(cseq1, (sseq + 1) & 0xFFFFFFFF, 0x18, get)))
    print("sent GET")
    cseq2 = (cseq1 + len(get)) & 0xFFFFFFFF

    def is_http(f):
        t = parse_tcp(f)
        if t and (t["flags"] & 0x08) and t["payload"].startswith(b"HTTP/1.0 200"):
            return t
        return None

    t = p.wait_for(is_http, 60, "HTTP response")
    if t is None:
        print("FAIL: no HTTP response")
        return 1
    ok = b"hello-rv32" in t["payload"]
    print(f"PASS: HTTP {len(t['payload'])}B body-ok={ok} ack={t['ack']:#x}")
    if not ok:
        print("FAIL: body missing:", t["payload"][:120])
        return 1
    snext = (sseq + 1 + len(t["payload"])) & 0xFFFFFFFF
    if t["seq"] != ((sseq + 1) & 0xFFFFFFFF) or t["ack"] != cseq2:
        print(f"FAIL: http seq/ack mismatch seq={t['seq']:#x} ack={t['ack']:#x}")
        return 1

    # 5. ACK the data, then FIN
    p.send(ip_pkt(tcp_seg(cseq2, snext, 0x10)))
    p.send(ip_pkt(tcp_seg(cseq2, snext, 0x11)))
    print("sent ACK + FIN")

    def is_finack(f):
        t = parse_tcp(f)
        if t and (t["flags"] & 0x01):
            return t
        return None

    t = p.wait_for(is_finack, 60, "FIN-ACK")
    if t is None:
        print("FAIL: no FIN-ACK")
        return 1
    print(f"PASS: FIN-ACK seq={t['seq']:#x} ack={t['ack']:#x}")
    if t["seq"] != snext or t["ack"] != ((cseq2 + 1) & 0xFFFFFFFF):
        print("FAIL: fin seq/ack mismatch")
        return 1
    # final ACK
    p.send(ip_pkt(tcp_seg((cseq2 + 1) & 0xFFFFFFFF,
                          (t["seq"] + 1) & 0xFFFFFFFF, 0x10)))
    print("sent final ACK")
    print("ALL PEER CHECKS PASSED")
    return 0


sys.exit(main())
