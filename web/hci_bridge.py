#!/usr/bin/env python3
"""hci_bridge.py — forward guest HCI (H4) between bramble and a host
Bluetooth controller over TCP (Bumble virtual controller or
`bumble-hci-bridge` -> physical adapter).

  ./build/bramble <fw.uf2> -wifi -bt-hci /tmp/bthci.sock [...]
  python3 web/hci_bridge.py --sock /tmp/bthci.sock [--tcp 127.0.0.1:9544]

Unix side: [u32 LE len][type+payload] (bramble listens, bridge connects).
TCP side: raw H4 stream (1-byte type + header-parsed payload), matching
Bumble's tcp-server/tcp-client transports and the gateway ble-gateway
convention (Bumble on localhost:9544).
"""
import argparse
import select
import socket
import struct
import sys
import time
from datetime import datetime


def ts():
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]

TNAMES = {1: "CMD", 2: "ACL", 3: "SCO", 4: "EVT", 5: "ISO"}

# Canned return params (after status) for short-circuited commands that
# have them. LE_Read_Buffer_Size_V2: ACL 27B x4, ISO 27B x4.
# LE_Encrypt: 16B "encrypted" data (zeros — bring-up only; real SMP
# pairing needs a controller that computes this).
CANNED_PARAMS = {
    0x2060: bytes([0x1B, 0x00, 0x04, 0x1B, 0x00, 0x04]),
    0x2017: bytes(16),
}


def h4_desc(pkt):
    """Short decode: CMD opcode / EVT code / ACL handle."""
    t = pkt[0]
    try:
        if t == 1 and len(pkt) >= 4:
            return f"CMD {pkt[1]:02x}{pkt[2]:02x}+{pkt[3]}"
        if t == 4 and len(pkt) >= 3:
            return f"EVT {pkt[1]:02x}+{pkt[2]}"
        if t in (2, 3) and len(pkt) >= 5:
            h = pkt[1] | (pkt[2] << 8)
            ln = pkt[3] | (pkt[4] << 8)
            return f"{TNAMES[t]} h={h:#x} {ln}B"
    except IndexError:
        pass
    return TNAMES.get(t, "?")


def h4_len(buf):
    """Total H4 packet length in buf, or 0 if incomplete/unknown."""
    if len(buf) < 1:
        return 0
    t = buf[0]
    if t == 1 and len(buf) >= 4:
        return 4 + buf[3]
    if t == 4 and len(buf) >= 3:
        return 3 + buf[2]
    if t == 2 and len(buf) >= 5:
        return 5 + (buf[3] | (buf[4] << 8))
    if t == 3 and len(buf) >= 4:
        return 4 + buf[3]
    if t == 5 and len(buf) >= 5:
        return 5 + (buf[3] | ((buf[4] & 0x3F) << 8))
    if t in (1, 2, 3, 4, 5):
        return 0  # incomplete header
    return -1  # unknown type: desync


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sock", default="/tmp/bthci.sock")
    ap.add_argument("--tcp", default="127.0.0.1:9544")
    ap.add_argument("--short-circuit", default="fc01,0c63,0c6d,2017",
                    help="comma-separated opcodes (hex) answered locally with CC(status 0): chip-specific (Broadcom Set_BD_ADDR) or commands minimal controllers lack (Set_Event_Mask_Page_2, Write_LE_Host_Support, LE_Encrypt)")
    args = ap.parse_args()
    host, port = args.tcp.rsplit(":", 1)
    short_circuit = set()
    for tok in args.short_circuit.split(","):
        tok = tok.strip().lower().lstrip("0x")
        if tok:
            try:
                short_circuit.add(int(tok, 16))
            except ValueError:
                pass

    u = None
    for _ in range(100):
        try:
            u = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            u.connect(args.sock)
            break
        except OSError:
            time.sleep(0.2)
    if u is None:
        print("FAIL: cannot connect to", args.sock)
        return 1
    u.settimeout(0.5)
    print(f"[hci_bridge] unix: connected to {args.sock}", flush=True)

    t = socket.create_connection((host, int(port)), timeout=10)
    t.settimeout(0.5)
    print(f"[hci_bridge] tcp: connected to {args.tcp}", flush=True)

    ubuf, tbuf = b"", b""
    n_up = n_dn = 0
    u.setblocking(False)
    t.setblocking(False)
    while True:
        # Wait for either side (no polling delay in either direction).
        try:
            r, _, _ = select.select([u, t], [], [], 5.0)
        except (OSError, ValueError):
            print("[hci_bridge] select error")
            return 0
        if u in r:
            # unix -> tcp (guest commands/ACL to controller)
            try:
                d = u.recv(65536)
            except BlockingIOError:
                d = None
            if d is not None:
                if not d:
                    print("[hci_bridge] unix EOF")
                    return 0
                ubuf += d
            while len(ubuf) >= 4:
                (ln,) = struct.unpack("<I", ubuf[:4])
                if ln < 1 or ln > 4096:
                    print(f"[{ts()}] [hci_bridge] WARN: bad unix len {ln}, dropping {len(ubuf)}B")
                    ubuf = b""
                    break
                if len(ubuf) < 4 + ln:
                    break
                pkt = ubuf[4:4 + ln]
                ubuf = ubuf[4 + ln:]
                # Short-circuit chip-specific commands the controller may
                # not implement (e.g. Broadcom Set_BD_ADDR 0xFC01): answer
                # Command Complete (status 0) locally, like Bumble's own
                # hci_bridge --short-circuit.
                if len(pkt) >= 4 and pkt[0] == 1:
                    op = pkt[1] | (pkt[2] << 8)
                    if op in short_circuit or op in CANNED_PARAMS:
                        params = CANNED_PARAMS.get(op, b"")
                        cc = (bytes([0x04, 0x0E, 4 + len(params), 0x01,
                                     pkt[1], pkt[2], 0x00]) + params)
                        u.sendall(struct.pack("<I", len(cc)) + cc)
                        n_dn += 1
                        print(f"[{ts()}] [hci_bridge] short-circuit CMD {op:04x} -> CC", flush=True)
                        continue
                try:
                    t.sendall(pkt)
                except (OSError, ConnectionError):
                    print("[hci_bridge] tcp write failed")
                    return 0
                n_up += 1
                if n_up <= 8 or n_up % 50 == 0:
                    print(f"[{ts()}] [hci_bridge] up {h4_desc(pkt)} ({ln}B #{n_up})", flush=True)
        if t in r:
            # tcp -> unix (controller events/ACL to guest)
            try:
                d = t.recv(65536)
            except BlockingIOError:
                d = None
            if d is not None:
                if not d:
                    print("[hci_bridge] tcp EOF")
                    return 0
                tbuf += d
            while True:
                ln = h4_len(tbuf)
                if ln == 0:
                    break
                if ln < 0:
                    print(f"[hci_bridge] WARN: unknown H4 type 0x{tbuf[0]:02x}, dropping 1B")
                    tbuf = tbuf[1:]
                    continue
                pkt = tbuf[:ln]
                tbuf = tbuf[ln:]
                try:
                    u.sendall(struct.pack("<I", ln) + pkt)
                except (OSError, ConnectionError):
                    print("[hci_bridge] unix write failed")
                    return 0
                n_dn += 1
                if n_dn <= 8 or n_dn % 50 == 0:
                    print(f"[{ts()}] [hci_bridge] dn {h4_desc(pkt)} ({ln}B #{n_dn})", flush=True)


sys.exit(main())
