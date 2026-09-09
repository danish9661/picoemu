#!/usr/bin/env python3
"""rsp_read.py — minimal RSP memory/register reader for Bramble gdb stub.
Usage: rsp_read.py <port> regs | mem <hexaddr> <hexlen>
"""
import socket, sys

def xfer(conn, payload: bytes) -> bytes:
    s = sum(payload) & 0xFF
    conn.sendall(b'$' + payload + b'#%02x' % s)
    buf = b''
    while True:
        ch = conn.recv(1)
        if not ch:
            raise ConnectionError('eof')
        if ch == b'+':
            continue
        if ch == b'$':
            break
        # async stop notification (T05...) — ack and keep waiting
        if ch == b'T':
            while ch != b'#':
                ch = conn.recv(1)
            conn.recv(2)
            conn.sendall(b'+')
            continue
    body = b''
    while True:
        ch = conn.recv(1)
        if ch == b'#':
            break
        body += ch
    conn.recv(2)  # checksum
    conn.sendall(b'+')
    return body

def main():
    port = int(sys.argv[1])
    conn = socket.create_connection(('127.0.0.1', port), timeout=10)
    conn.settimeout(10)
    if sys.argv[2] == 'regs':
        import struct
        raw = xfer(conn, b'g')
        regs = struct.unpack('<%dI' % (len(raw) // 8), bytes.fromhex(raw.decode()))
        names = ['r%d' % i for i in range(13)] + ['sp', 'lr', 'pc']
        for i, v in enumerate(regs[:16]):
            print('%s=0x%08x' % (names[i], v))
    elif sys.argv[2] == 'mem':
        addr, ln = int(sys.argv[3], 16), int(sys.argv[4], 16)
        raw = xfer(conn, ('m%x,%x' % (addr, ln)).encode())
        data = bytes.fromhex(raw.decode())
        import struct
        for off in range(0, len(data), 16):
            chunk = data[off:off+16]
            print('%08x: %s' % (addr + off, ' '.join('%02x' % b for b in chunk)))

if __name__ == '__main__':
    main()
