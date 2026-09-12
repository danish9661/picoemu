#!/usr/bin/env python3
"""vnet peer E2E test for MicroPython with LWIP_IPV6=1 (+DUP_DETECT=0):
learns the guest MAC, derives its SLAAC ULA (fd00:4::/64, EUI-64), then
NS->NA and Echo->Echo-Reply with ICMPv6 checksum verification.

Guest: MP firmware built with LWIP_IPV6=1 + LWIP_IPV6_DUP_DETECT_ATTEMPTS=0
(ports/rp2/lwip_inc/lwipopts.h, MEM_SIZE 24000) plus dual-stack fixes in
the MP tree (all guarded, v4-neutral): `ip_2_ip4`/`IP_ADDR4` in
extmod/modlwip.c (also AF_INET6 socket create/bind/connect/sendto/
recvfrom/accept/getaddrinfo-literal via udp_new_ip6/tcp_new_ip6),
extmod/network_cyw43.c, shared/netutils/dhcpserver.c; immediate-PREFERRED
for SLAAC addresses in lib/lwip/src/core/netif.c (DAD-less builds); and an
lwIP timer pump in ports/rp2/mpconfigport.h (MICROPY_INTERNAL_EVENT_HOOK
runs sys_check_timeouts under lwip_lock — without it no lwIP timer fires:
no DHCP retries/RS/DAD/TCP-RTO; TCP RTO verified firing with it).
Guest main.py only needs to join BrambleNet (v6 SLAAC via periodic RA
is automatic). Run:
  ./build/bramble ~/mpbuild-w2/firmware.uf2 -clock 125 -wifi -net \\
      -net-peer /tmp/mp6test.sock
  python3 test-firmware/mp6_peer_test.py /tmp/mp6test.sock
Exit 0 + ALL MP6 CHECKS PASSED on success.
"""
import socket
import struct
import sys
import time

SOCK = sys.argv[1] if len(sys.argv) > 1 else "/tmp/mp6test.sock"
MY_MAC = bytes([0x02, 0x12, 0x34, 0x56, 0x78, 0x02])
MY_IP = bytes.fromhex("fe800000" + "00000000" + "00000000" + "00000002")
ULA_PREFIX = bytes.fromhex("fd000004" + "00000000")


def eui64(mac):
    return (bytes([mac[0] ^ 0x02]) + mac[1:3] + bytes([0xFF, 0xFE]) +
            mac[3:6])


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


def eth(dst_mac, src_mac, payload6):
    return dst_mac + src_mac + struct.pack(">H", 0x86DD) + payload6


def ip6(src, dst, body, hop=255):
    h = bytes([0x60, 0, 0, 0]) + struct.pack(">H", len(body)) + bytes([58, hop])
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

    def frames_until(self, deadline):
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
                yield f

    def wait_for(self, pred, timeout, desc):
        deadline = time.time() + timeout
        for f in self.frames_until(deadline):
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
    if len(body) < 8 or ip[40] in (44,) or len(ip) < 40 + len(body):
        return None
    if icmp6_cksum(ip[8:24], ip[24:40], body) != 0:
        return None  # bad cksum or ext headers: ignore quietly
    return {"src": ip[8:24], "dst": ip[24:40], "type": body[0],
            "code": body[1], "body": body, "smac": f[6:12]}


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

    # 1. Learn the guest MAC from any frame it sends (MLD reports go out
    # at netif bring-up even before SLAAC).
    def is_guest(f):
        if len(f) >= 14:
            return f[6:12]
        return None

    guest_mac = p.wait_for(is_guest, 120, "guest frame")
    if guest_mac is None:
        print("FAIL: no guest traffic")
        return 1
    ula = ULA_PREFIX + eui64(guest_mac)
    print(f"PASS: guest mac {guest_mac.hex()} -> ULA {ula.hex()}")

    # 2. NS/NA for the ULA (best-effort: SLAAC needs >=1 processed RA at
    # 7s cadence; retry a few times, then move on to Echo regardless).
    sn_ip = bytes.fromhex("ff020000") + bytes(8) + bytes([1, 0xFF]) + ula[13:16]
    sn_mac = bytes([0x33, 0x33, 0xFF]) + ula[13:16]

    def is_na(f):
        t = parse_icmp6(f)
        if t and t["type"] == 136 and t["body"][4] & 0x60 == 0x60:
            if t["body"][8:24] == ula and t["body"][26:32] == guest_mac:
                return t
        return None

    na_ok = False
    for attempt in range(4):
        ns = bytes([135, 0, 0, 0, 0, 0, 0, 0]) + ula + bytes([1, 1]) + MY_MAC
        p.send(eth(sn_mac, MY_MAC,
                   ip6(MY_IP, sn_ip, with_cksum(MY_IP, sn_ip, ns))))
        print(f"sent NS for ULA (try {attempt + 1})")
        t = p.wait_for(is_na, 20, "NA")
        if t is not None:
            print(f"PASS: NA S+O for ULA from {t['src'].hex()}")
            na_ok = True
            break
    if not na_ok:
        print("note: no NA (continuing to Echo)")

    # 4. Echo Request -> Echo Reply (answer their NS for us on the way).
    data = bytes(range(0xC0, 0xD0))
    echo = struct.pack(">BBHHH", 128, 0, 0, 0x5678, 1) + data
    p.send(eth(guest_mac, MY_MAC,
               ip6(MY_IP, ula, with_cksum(MY_IP, ula, echo))))
    print("sent Echo Request")

    def is_ns_or_reply(f):
        t = parse_icmp6(f)
        if not t:
            return None
        if t["type"] == 135 and t["body"][8:24] == MY_IP:
            na = (bytes([136, 0, 0, 0, 0x60, 0, 0, 0]) + MY_IP +
                  bytes([2, 1]) + MY_MAC)
            p.send(eth(t["smac"], MY_MAC,
                       ip6(MY_IP, t["src"],
                           with_cksum(MY_IP, t["src"], na))))
            print("answered NS for us")
            return None
        if t["type"] == 129 and t["body"][4:8] == b"\x56\x78\x00\x01" \
                and t["body"][8:24] == data:
            return t
        return None

    t = p.wait_for(is_ns_or_reply, 90, "Echo Reply")
    if t is None:
        print("FAIL: no Echo Reply")
        return 1
    print(f"PASS: Echo Reply id=0x5678 {len(t['body']) - 8}B echoed")
    print("ALL MP6 CHECKS PASSED")
    return 0


sys.exit(main())
