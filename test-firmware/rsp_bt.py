#!/usr/bin/env python3
"""rsp_bt.py — guest backtrace via Bramble's GDB RSP stub + ELF symbols.
Usage: rsp_bt.py <elf> <port> [samples] [interval_ms]
Connects, samples PC/SP/LR + stack return addresses, prints function names.
"""
import socket, struct, subprocess, sys, time

def rsp(conn, pkt):
    def esc(b):
        return b.replace(b'}', b'}\x5d').replace(b'#', b'}\x03').replace(b'$', b'}\x04')
    body = esc(pkt)
    s = sum(body) & 0xFF
    conn.sendall(b'$' + body + b'#%02x' % s)
    if pkt[0:1] in (b'c', b's'):
        return None
    # read + ack
    data = b''
    while True:
        c = conn.recv(1)
        if not c: return None
        if c == b'+': continue
        if c == b'$': break
    while not data.endswith(b'#') or len(data) < 3 or data[-3:-2] != b'#':
        data += conn.recv(4096)
        if data.endswith(b'#'):
            # need 2 checksum bytes
            while len(data) < data.index(b'#') + 3:
                data += conn.recv(4096)
            break
    conn.sendall(b'+')
    payload = data[1:data.index(b'#')]
    return payload

def read_regs(conn):
    p = rsp(conn, b'g')
    regs = struct.unpack('<' + 'I' * (len(p) // 8), bytes.fromhex(p.decode()))
    return regs

def read_mem(conn, addr, ln):
    p = rsp(conn, ('m%x,%x' % (addr, ln)).encode())
    return bytes.fromhex(p.decode())

def load_syms(elf):
    out = subprocess.run(['arm-none-eabi-nm', '-n', elf], capture_output=True, text=True)
    if out.returncode != 0:
        # try PATH lookup
        import shutil
        nm = shutil.which('arm-none-eabi-nm') or 'arm-none-eabi-nm'
        out = subprocess.run([nm, '-n', elf], capture_output=True, text=True)
    syms = []
    for line in out.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] in 'TtWw':
            syms.append((int(parts[0], 16), parts[2]))
    return syms

def symname(syms, addr):
    addr &= ~1
    best = '??'
    for a, n in syms:
        if a <= addr: best = n
        else: break
    return '%s+0x%x' % (best, addr - next(a for a, n in syms if n == best.split('+')[0]))

def main():
    elf, port = sys.argv[1], int(sys.argv[2])
    nsamp = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    ivl = int(sys.argv[4]) if len(sys.argv) > 4 else 1500
    syms = load_syms(elf)
    conn = socket.create_connection(('127.0.0.1', port), timeout=10)
    # drain greeting
    conn.settimeout(2)
    try:
        while True:
            d = conn.recv(4096)
            if not d: break
    except Exception:
        pass
    for i in range(nsamp):
        try:
            regs = read_regs(conn)
            pc, sp, lr = regs[15], regs[13], regs[14]
            print('sample %d: PC=%s SP=0x%08x LR=%s' % (i, symname(syms, pc), sp, symname(syms, lr)))
            # stack return addresses (Thumb, +1)
            try:
                words = struct.unpack('<16I', read_mem(conn, sp, 64))
                rets = sorted(set(w for w in words if 0x10000000 <= (w & ~1) < 0x10100000 and (w & 1)))
                for r in rets[:6]:
                    print('    ret %s' % symname(syms, r))
            except Exception as e:
                print('    stack read failed:', e)
        except Exception as e:
            print('sample failed:', e)
        time.sleep(ivl / 1000.0)

if __name__ == '__main__':
    main()
