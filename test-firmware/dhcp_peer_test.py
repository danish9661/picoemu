#!/usr/bin/env python3
"""vnet peer E2E test for eth_dhcp (all 3 arches): speaks the vnet peer
protocol (4-byte LE len + ETH frame) over a Unix socket, answers the
guest's DHCP DISCOVER with an OFFER and its REQUEST with an ACK (from
eth_dhcp_common, same lease pool as the Go gateway: .2 / server .1),
and expects the guest to print ETH DONE.

Usage (two terminals):
  ./build/bramble web/eth_dhcp.uf2 -board pico-eth \\
      -net -net-peer /tmp/dhcptest.sock
  python3 test-firmware/dhcp_peer_test.py /tmp/dhcptest.sock
Exit 0 + ALL DHCP CHECKS PASSED on success.

The guest MAC/XID differ per arch (M0/M33/RV32); the responder keys the
lease off the client's chaddr (like handleDHCP.go) so all three guests
can share one room without cross-talk.
"""
import socket
import struct
import sys
import time

SOCK = sys.argv[1] if len(sys.argv) > 1 else "/tmp/dhcptest.sock"

SRV_MAC = bytes([0x5A, 0x94, 0xEF, 0xE4, 0x0C, 0xDD])  # gateway MAC (handleDHCP.go)
SRV_IP = bytes([192, 168, 4, 1])
LEASE_IP = bytes([192, 168, 4, 2])


def csum(data):
    s = 0
    for i in range(0, len(data) - 1, 2):
        s += (data[i] << 8) + data[i + 1]
    if len(data) & 1:
        s += data[-1] << 8
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def dhcp_reply(req_bootp, msgtype):
    """Build a DHCP reply BOOTP (342B) mirroring handleDHCP.go options."""
    rep = bytearray(300)
    rep[0] = 2  # BOOTREPLY
    rep[1:4] = req_bootp[1:4]
    rep[4:8] = req_bootp[4:8]  # xid
    rep[10] = 0x80
    rep[16:20] = LEASE_IP
    rep[20:24] = SRV_IP
    rep[28:34] = req_bootp[28:34]
    rep[236:240] = bytes([99, 130, 83, 99])
    o = 240
    for opt in (bytes([53, 1, msgtype]),
                bytes([54, 4]) + SRV_IP,
                bytes([51, 4, 0, 1, 81, 128]),  # 86400s like the gateway
                bytes([1, 4, 255, 255, 255, 0]),
                bytes([3, 4]) + SRV_IP,
                bytes([6, 4, 8, 8, 8, 8]),
                bytes([255])):
        rep[o:o + len(opt)] = opt
        o += len(opt)
    udp = bytearray(8 + 300)
    udp[0], udp[1], udp[2], udp[3] = 0, 67, 0, 68
    udp[4], udp[5] = ((8 + 300) >> 8) & 0xFF, (8 + 300) & 0xFF
    udp[8:] = rep
    pseudo = bytes(SRV_IP) + bytes([255, 255, 255, 255, 0, 17]) + \
        bytes([((8 + 300) >> 8) & 0xFF, (8 + 300) & 0xFF])
    udp[6], udp[7] = 0, 0
    cs = csum(bytes(pseudo) + bytes(udp))
    udp[6], udp[7] = (cs >> 8) & 0xFF, cs & 0xFF
    ip = bytearray(20)
    ip[0], ip[1] = 0x45, 0
    ip[2], ip[3] = ((20 + 8 + 300) >> 8) & 0xFF, (20 + 8 + 300) & 0xFF
    ip[4], ip[5], ip[6], ip[7] = 0x33, 0x44, 0, 0
    ip[8], ip[9] = 64, 17
    ip[12:16] = SRV_IP
    ip[16:20] = bytes([255, 255, 255, 255])
    cs = csum(bytes(ip))
    ip[10], ip[11] = (cs >> 8) & 0xFF, cs & 0xFF
    return bytes([0xFF] * 6) + bytes([0, 0, 0, 0, 0, 0]) + bytes([0x08, 0x00]) \
        + bytes(ip) + bytes(udp)


def parse_dhcp(frame):
    """If frame is a DHCP DISCOVER/REQUEST to us, return (bootp, msgtype)."""
    if len(frame) < 14 + 20 + 8 + 240:
        return None
    if frame[12:14] != b"\x08\x00":
        return None
    ip = frame[14:]
    if ip[9] != 17:
        return None
    bootp = frame[14 + 20 + 8:]
    if bootp[236:240] != bytes([99, 130, 83, 99]) or bootp[0] != 1:
        return None
    msgtype = 0
    i = 240
    while i < len(bootp) and bootp[i] != 255:
        if i + 1 >= len(bootp):
            break
        ln = bootp[i + 1]
        if bootp[i] == 53 and ln >= 1:
            msgtype = bootp[i + 2]
        i += 2 + ln
    if msgtype not in (1, 3):
        return None
    return bootp, msgtype


class Peer:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(0.5)
        self.s.connect(path)
        self.buf = b""

    def send(self, frame):
        self.s.sendall(struct.pack("<I", len(frame)) + frame)

    def wait_for(self, pred, timeout, desc):
        deadline = time.time() + timeout
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
                if ln > 9000 or len(self.buf) < 4 + ln:
                    break
                f = self.buf[4:4 + ln]
                self.buf = self.buf[4 + ln:]
                r = pred(f)
                if r is not None:
                    return r
        print(f"TIMEOUT waiting for {desc}")
        return None


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

    # 1. DISCOVER -> OFFER
    def is_discover(f):
        r = parse_dhcp(f)
        if r and r[1] == 1:
            return r[0]
        return None

    bootp = p.wait_for(is_discover, 90, "DHCP DISCOVER")
    if bootp is None:
        print("FAIL: no DISCOVER")
        return 1
    print(f"PASS: DISCOVER xid={bootp[4:8].hex()} chaddr={bootp[28:34].hex()}")
    # client MAC lives at bootp+28 (chaddr); ETH src of reply = SRV_MAC,
    # dst = broadcast (gateway behavior); ETH src field = SRV_MAC.
    offer = bytearray(dhcp_reply(bootp, 2))
    # dhcp_reply built dst=broadcast already; fix ETH src to SRV_MAC
    offer[6:12] = SRV_MAC
    p.send(bytes(offer))
    print("sent OFFER 192.168.4.2")

    # 2. REQUEST -> ACK
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
    print("ALL DHCP CHECKS PASSED (guest should print ETH DONE)")
    return 0


sys.exit(main())
