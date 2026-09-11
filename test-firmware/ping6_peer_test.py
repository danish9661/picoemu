#!/usr/bin/env python3
"""vnet peer E2E test for ping6_rv32: NS->NA and Echo->Echo-Reply against
the guest (ULA fd00:4::64), with ICMPv6 checksum verification.

Usage:
  ./build/bramble web/wifi_ping6_rv32.uf2 -clock 125 -wifi \\
      -net -net-peer /tmp/p6test.sock
  python3 test-firmware/ping6_peer_test.py /tmp/p6test.sock
Exit 0 + ALL PING6 CHECKS PASSED on success.
"""
import socket
import struct
import sys
import time

SOCK = sys.argv[1] if len(sys.argv) > 1 else "/tmp/p6test.sock"
DEV_MAC = bytes([0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE])
DEV_IP = bytes.fromhex("fd000004" + "00000000" + "00000000" + "00000064")
MY_MAC = bytes([0x02, 0x12, 0x34, 0x56, 0x78, 0x02])
MY_IP = bytes.fromhex("fe800000" + "00000000" + "00000000" + "00000002")
SN_MAC = bytes([0x33, 0x33, 0xFF, 0x00, 0x00, 0x64])
SN_IP = bytes.fromhex("ff020000" + "00000000" + "00000001" + "ff000064")


def csum(data):
    s = 0
    for i in range(0, len(data) - 1, 2):
        s += (data[i] << 8) + data[i + 1]
    if len(data) & 1:
        s += data[-1] << 8
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def icmp6_cksum(src, dst, body):
    pseudo = src + dst + struct.pack(">I", len(body)) + bytes([0, 0, 0, 58])
    return csum(pseudo + body)


def eth(dst_mac, payload6):
    return dst_mac + MY_MAC + struct.pack(">H", 0x86DD) + payload6


def ip6(src, dst, body):
    h = bytes([0x60, 0, 0, 0]) + struct.pack(">H", len(body)) + bytes([58, 64])
    return h + src + dst + body


def with_cksum(src, dst, body):
    cs = icmp6_cksum(src, dst, body)
    return body[:2] + struct.pack(">H", cs) + body[4:]


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
                if len(self.buf) < 4 + ln:
                    break
                f = self.buf[4:4 + ln]
                self.buf = self.buf[4 + ln:]
                r = pred(f)
                if r is not None:
                    return r
        print(f"TIMEOUT waiting for {desc}")
        return None


def parse_icmp6(f):
    if len(f) < 14 + 40 + 8:
        return None
    if f[12:14] != b"\x86\xdd":
        return None
    ip = f[14:]
    if (ip[0] >> 4) != 6 or ip[6] != 58:
        return None
    body = ip[40:40 + ((ip[4] << 8) + ip[5])]
    if icmp6_cksum(ip[8:24], ip[24:40], body) != 0:
        print("WARN: bad ICMPv6 checksum")
        return None
    return {"src": ip[8:24], "dst": ip[24:40], "type": body[0],
            "code": body[1], "body": body}


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

    # 1. Neighbor Solicitation for fd00:4::64
    ns = bytes([135, 0, 0, 0, 0, 0, 0, 0]) + DEV_IP
    ns += bytes([1, 1]) + MY_MAC
    p.send(eth(SN_MAC, ip6(MY_IP, SN_IP, with_cksum(MY_IP, SN_IP, ns))))
    print("sent NS")

    def is_na(f):
        t = parse_icmp6(f)
        if t and t["type"] == 136 and t["body"][4] & 0x60 == 0x60:
            if t["body"][8:24] == DEV_IP and t["body"][24:26] == b"\x02\x01" \
                    and t["body"][26:32] == DEV_MAC:
                return t
        return None

    t = p.wait_for(is_na, 60, "NA")
    if t is None:
        print("FAIL: no NA")
        return 1
    print("PASS: NA for fd00:4::64 S+O from", t["src"].hex())

    # 2. Echo Request (16B data)
    data = bytes(range(0xA0, 0xB0))
    echo = struct.pack(">BBHHH", 128, 0, 0, 0x1234, 1) + data
    p.send(eth(DEV_MAC, ip6(MY_IP, DEV_IP, with_cksum(MY_IP, DEV_IP, echo))))
    print("sent Echo Request")

    def is_reply(f):
        t = parse_icmp6(f)
        if t and t["type"] == 129 and t["body"][4:8] == b"\x12\x34\x00\x01" \
                and t["body"][8:24] == data:
            return t
        return None

    t = p.wait_for(is_reply, 60, "Echo Reply")
    if t is None:
        print("FAIL: no Echo Reply")
        return 1
    print(f"PASS: Echo Reply id=0x1234 seq=1 {len(t['body']) - 8}B echoed")
    print("ALL PING6 CHECKS PASSED")
    return 0


sys.exit(main())
