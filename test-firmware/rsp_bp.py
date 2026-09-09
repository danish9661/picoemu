#!/usr/bin/env python3
"""rsp_bp.py — break, read regs+stack, resume. Usage: rsp_bp.py <port> <hexaddr>"""
import socket, struct, sys

def xfer(conn, payload: bytes) -> bytes:
    s = sum(payload) & 0xFF
    conn.sendall(b'$' + payload + b'#%02x' % s)
    while True:
        ch = conn.recv(1)
        if not ch:
            raise ConnectionError('eof')
        if ch == b'+':
            continue
        if ch == b'$':
            break
        if ch == b'T':  # async stop notification
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
    conn.recv(2)
    conn.sendall(b'+')
    return body

port, addr = int(sys.argv[1]), sys.argv[2]
conn = socket.create_connection(('127.0.0.1', port), timeout=10)
conn.settimeout(30)
print('break:', xfer(conn, ('Z0,%s,2' % addr).encode()))
xfer(conn, b'c')
print('waiting for breakpoint hit...')
# wait for async T05 stop notification
conn.settimeout(120)
while True:
    ch = conn.recv(1)
    if ch == b'+':
        continue
    if ch == b'$':
        break
body = b''
while True:
    ch = conn.recv(1)
    if ch == b'#':
        break
    body += ch
conn.recv(2)
conn.sendall(b'+')
print('stopped:', body[:20])
print('regs:')
raw = xfer(conn, b'g')
regs = struct.unpack('<%dI' % (len(raw) // 8), bytes.fromhex(raw.decode()))
for i, v in enumerate(regs[:16]):
    print('  r%d=0x%08x' % (i, v) if i < 13 else '  %s=0x%08x' % (['sp', 'lr', 'pc'][i - 13], v))
sp = regs[13]
raw = xfer(conn, ('m%x,64' % sp).encode())
data = bytes.fromhex(raw.decode())
print('stack:')
for off in range(0, 64, 16):
    print('  %08x: %s' % (sp + off, ' '.join('%02x' % b for b in data[off:off+16])))
xfer(conn, b'c')
