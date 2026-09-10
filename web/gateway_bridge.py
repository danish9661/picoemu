#!/usr/bin/env python3
"""gateway_bridge.py — vnet (unix socket) <-> OpenHW network-gateway (WebSocket).

Lets native picoemu instances use the Go gateway for DHCP/DNS/NAT and
room LAN play without root/TAP:

  bramble a.uf2 -wifi -nodhcp -net -net-peer /tmp/gwroom.sock -mac DE:AD:BE:EF:00:01
  python3 web/gateway_bridge.py --sock /tmp/gwroom.sock --room lab

Protocol: vnet peer framing is 4-byte LE length + raw ETH frame.
Gateway WS takes/emits raw binary ETH frames (text frames like
BOARD_IP:<ip> are logged). Stdlib only.
"""
import argparse
import base64
import hashlib
import os
import select
import socket
import struct
import sys
import time

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def log(msg, flush=True):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=flush)


def ws_connect(url, room):
    # url like ws://host:port/path
    assert url.startswith("ws://"), "only ws:// supported"
    rest = url[5:]
    hostport, _, path = rest.partition("/")
    path = "/" + path
    host, _, port = hostport.partition(":")
    port = int(port or 80)
    if room:
        sep = "&" if "?" in path else "?"
        path = f"{path}{sep}sessionId={room}"
    s = socket.create_connection((host, port), timeout=10)
    key = base64.b64encode(os.urandom(16)).decode()
    req = (f"GET {path} HTTP/1.1\r\nHost: {hostport}\r\n"
           f"Upgrade: websocket\r\nConnection: Upgrade\r\n"
           f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
    s.sendall(req.encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        chunk = s.recv(4096)
        if not chunk:
            raise ConnectionError("WS handshake EOF")
        resp += chunk
    if b" 101 " not in resp.split(b"\r\n", 1)[0]:
        raise ConnectionError(f"WS handshake failed: {resp[:80]!r}")
    log(f"[bridge] WS connected: {url} room={room or '(server-assigned)'}",
          flush=True)
    return s


def ws_send_bin(s, payload):
    hdr = bytes([0x82])
    n = len(payload)
    mask = os.urandom(4)
    if n < 126:
        hdr += bytes([0x80 | n])
    elif n < 65536:
        hdr += bytes([0x80 | 126]) + struct.pack(">H", n)
    else:
        hdr += bytes([0x80 | 127]) + struct.pack(">Q", n)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    s.sendall(hdr + mask + masked)


class WSReader:
    def __init__(self):
        self.buf = b""

    def feed(self, data):
        self.buf += data

    def next_msg(self):
        # returns (opcode, payload) or None if incomplete
        b = self.buf
        if len(b) < 2:
            return None
        op = b[0] & 0x0F
        ln = b[1] & 0x7F
        off = 2
        if ln == 126:
            if len(b) < 4:
                return None
            ln = struct.unpack(">H", b[2:4])[0]
            off = 4
        elif ln == 127:
            if len(b) < 10:
                return None
            ln = struct.unpack(">Q", b[2:10])[0]
            off = 10
        if b[1] & 0x80:  # masked (server must not, but tolerate)
            off += 4
        if len(b) < off + ln:
            return None
        payload = b[off:off + ln]
        if b[1] & 0x80:
            m = b[off - 4:off]
            payload = bytes(x ^ m[i % 4] for i, x in enumerate(payload))
        self.buf = b[off + ln:]
        return op, payload


def open_unix(path):
    # Try client first (emulator already listening); else listen.
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(path)
        log(f"[bridge] unix: connected to {path}", flush=True)
        return s, None
    except OSError:
        pass
    try:
        os.unlink(path)
    except OSError:
        pass
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(path)
    srv.listen(4)
    log(f"[bridge] unix: listening on {path}", flush=True)
    return None, srv


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sock", required=True, help="vnet peer socket path")
    ap.add_argument("--gateway", default="ws://localhost:5090/api/network-gateway")
    ap.add_argument("--room", default="", help="gateway ?sessionId=")
    args = ap.parse_args()

    usock, srv = open_unix(args.sock)
    ws = ws_connect(args.gateway, args.room)
    ws.setblocking(False)

    def ws_reconnect():
        nonlocal ws, wsr
        try:
            ws.close()
        except OSError:
            pass
        import time as _t
        for attempt in range(30):
            try:
                _t.sleep(2)
                ws = ws_connect(args.gateway, args.room)
                ws.setblocking(False)
                wsr = WSReader()
                log("[bridge] WS reconnected", flush=True)
                return
            except OSError as e:
                log(f"[bridge] WS reconnect failed ({e}), retrying")
        log("[bridge] WS reconnect exhausted, exiting")
        sys.exit(1)

    wsr = WSReader()
    ubuf = b""
    n_up = 0
    n_dn = 0
    last_ping = time.time()
    try:
        while True:
            r, _, _ = select.select([srv, ws] if usock is None else [usock, ws], [], [], 1.0)
            if not r:
                # idle: unsolicited PONG keeps the gateway read deadline
                # rolling (gorilla swallows Ping internally, but routes
                # Pong to its handler which extends the deadline)
                if time.time() - last_ping > 25:
                    try:
                        m = os.urandom(4)
                        ws.sendall(bytes([0x8A, 0x80]) + m)
                        last_ping = time.time()
                        log("[bridge] pong sent")
                    except OSError:
                        log("[bridge] ping send failed, reconnecting")
                        ws_reconnect()
                        continue
                if usock is None:
                    continue
            if usock is None:
                # waiting for emulator: still drain gateway -> drop
                r, _, _ = select.select([srv, ws], [], [], 1.0)
                if srv in r:
                    usock, _ = srv.accept()
                    usock.setblocking(False)
                    log("[bridge] unix: emulator connected", flush=True)
                if ws in r:
                    try:
                        chunk = ws.recv(65536)
                    except BlockingIOError:
                        chunk = None
                    if chunk == b"":
                        log("[bridge] WS EOF, reconnecting", flush=True)
                        ws_reconnect()
                        continue
                    if chunk:
                        wsr.feed(chunk)
                        while wsr.next_msg() is not None:
                            n_dn += 1
                continue
            r, _, _ = select.select([usock, ws], [], [], 1.0)
            if usock in r:
                try:
                    chunk = usock.recv(65536)
                except BlockingIOError:
                    chunk = None
                if chunk == b"":
                    log("[bridge] unix EOF, waiting for reconnect",
                          flush=True)
                    try:
                        usock.close()
                    except OSError:
                        pass
                    usock = None
                    ubuf = b""
                    continue
                if chunk:
                    ubuf += chunk
                    while len(ubuf) >= 4:
                        ln = struct.unpack("<I", ubuf[:4])[0]
                        if ln > 9000 or ln < 14:
                            log(f"[bridge] bad vnet len {ln}, resync",
                                  flush=True)
                            ubuf = b""
                            break
                        if len(ubuf) < 4 + ln:
                            break
                        try:
                            ws_send_bin(ws, ubuf[4:4 + ln])
                        except OSError:
                            log("[bridge] WS send failed, reconnecting",
                                  flush=True)
                            ws_reconnect()
                            break
                        n_up += 1
                        if n_up <= 10 or n_up % 50 == 0:
                            log(f"[bridge] vnet->gw frame {n_up}: {ln}B ethertype=0x{ubuf[4+12:4+14].hex()}",
                                  flush=True)
                        ubuf = ubuf[4 + ln:]
            if ws in r:
                try:
                    chunk = ws.recv(65536)
                except BlockingIOError:
                    chunk = None
                if chunk == b"":
                    log("[bridge] WS EOF, reconnecting", flush=True)
                    ws_reconnect()
                    continue
                if chunk:
                    wsr.feed(chunk)
                    while True:
                        m = wsr.next_msg()
                        if m is None:
                            break
                        op, payload = m
                        if op == 0x1:  # text (e.g. BOARD_IP:x)
                            log(f"[bridge] gateway: {payload.decode(errors='replace')}",
                                  flush=True)
                        elif op == 0x2:  # binary ETH
                            if 14 <= len(payload) <= 9000:
                                try:
                                    usock.sendall(struct.pack("<I", len(payload)) + payload)
                                except OSError:
                                    log("[bridge] unix send failed, waiting for reconnect",
                                          flush=True)
                                    try:
                                        usock.close()
                                    except OSError:
                                        pass
                                    usock = None
                                    ubuf = b""
                                    break
                                n_dn += 1
                                if n_dn <= 10 or n_dn % 50 == 0:
                                    log(f"[bridge] gw->vnet frame {n_dn}: {len(payload)}B",
                                          flush=True)
                        elif op == 0x8:  # close
                            log("[bridge] WS close, reconnecting",
                                  flush=True)
                            ws_reconnect()
                            break
                        elif op == 0x9:  # ping -> pong (echo payload)
                            log(f"[bridge] ping ({len(payload)}B), ponging")
                            pong = bytes([0x8A])
                            m = os.urandom(4)
                            n = len(payload)
                            if n < 126:
                                pong += bytes([0x80 | n])
                            elif n < 65536:
                                pong += bytes([0x80 | 126]) + struct.pack(">H", n)
                            else:
                                pong += bytes([0x80 | 127]) + struct.pack(">Q", n)
                            pong += m + bytes(b ^ m[i % 4]
                                              for i, b in enumerate(payload))
                            ws.sendall(pong)
    except KeyboardInterrupt:
        pass
    finally:
        try:
            if srv:
                srv.close()
        except OSError:
            pass


if __name__ == "__main__":
    main()
