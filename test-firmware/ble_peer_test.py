#!/usr/bin/env python3
"""vnet peer E2E test for ble_adv_rv32: speaks the vnet peer protocol
(4-byte LE len + ETH frame) over a Unix socket, verifies the guest's BLE
ADV announcements (ethertype 0x88B5) carry our MAC + BRV32 payload, injects
a foreign ADV, and expects beacons to repeat (~2s cadence).

Usage (two terminals):
  ./build/bramble web/wifi_ble_adv_rv32.uf2 -clock 125 -wifi \\
      -net -net-peer /tmp/bletest.sock
  python3 test-firmware/ble_peer_test.py /tmp/bletest.sock
Exit 0 + ALL PEER CHECKS PASSED on success.
 guest SCAN-OK (report synthesis) needs the guest UART: run two
bramble instances peered together (see docs/NETWORKING.md BLE row).
"""
import socket
import struct
import sys
import time

SOCK = sys.argv[1] if len(sys.argv) > 1 else "/tmp/bletest.sock"
DEV_MAC = bytes([0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE])
PEER_MAC = bytes([0x11, 0x22, 0x33, 0x44, 0x55, 0x66])


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
                if ln > 9000 or len(self.buf) < 4 + ln:
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


def is_adv(f):
    if len(f) >= 14 + 38 and f[12:14] == b"\x88\xb5":
        return f
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

    # 1. First ADV announce (enable-time; may arrive via pre-accept backlog).
    f = p.wait_for(is_adv, 60, "ADV announce")
    if f is None:
        print("FAIL: no ADV announce")
        return 1
    if f[14:20] != DEV_MAC:
        print("FAIL: ADV BDADDR mismatch:", f[14:20].hex())
        return 1
    alen = f[20]
    adv = f[21:21 + alen]
    if b"BRV32" not in adv:
        print("FAIL: ADV payload missing BRV32:", adv.hex())
        return 1
    print(f"PASS: ADV from {f[14:20].hex()} len={alen} payload-ok")

    # 2. Beacon repeats (~2s cadence): expect a second ADV.
    f2 = p.wait_for(is_adv, 15, "ADV beacon repeat")
    if f2 is None:
        print("FAIL: no beacon repeat")
        return 1
    print("PASS: beacon repeats")

    # 3. Inject a foreign ADV (guest synthesizes LE Advertising Report;
    # visible on guest UART as SCAN-OK, not on the wire).
    payload = bytes([2, 1, 6, 5, 9]) + b"PEER1"
    inj = (bytes([0xFF] * 6) + DEV_MAC + b"\x88\xb5" + PEER_MAC +
           bytes([len(payload)]) + payload + bytes(31 - len(payload)))
    p.send(inj)
    print("sent foreign ADV (check guest UART for SCAN-OK)")
    print("ALL PEER CHECKS PASSED")
    return 0


sys.exit(main())
