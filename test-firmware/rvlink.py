"""Minimal RISC-V static linker for bare-metal Bramble demos.
Usage: rvlink.py in.o out.bin [--base 0x10000000]
Lays .text at base, .rodata after (align 4). Handles R_RISCV_JAL/BRANCH/
HI20/LO12/PCREL_HI20/PCREL_LO12; ignores ALIGN/RELAX; fails on anything else.
No .data/.bss support (demos must be rodata+text only).
"""
import struct, sys

R_JAL, R_BRANCH = 17, 16
R_HI20, R_LO12_I, R_LO12_S = 26, 27, 28
R_PCREL_HI20, R_PCREL_LO12_I = 23, 24
R_ALIGN, R_RELAX = 51, 42

def u32(b, o=0):
    return struct.unpack('<I', b[o:o+4])[0]

def main():
    path, outp = sys.argv[1], sys.argv[2]
    base = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x10000000
    d = open(path, 'rb').read()
    assert d[:4] == b'\x7fELF' and d[4] == 1 and struct.unpack("<H", d[18:20])[0] == 0xF3
    e_shoff = u32(d, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack('<HHH', d[0x2E:0x34])
    secs = {}
    order = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        name, typ, flags, addr, off, size, link, info, al, es = struct.unpack('<IIIIIIIIII', d[o:o+40])
        secs[i] = dict(name=name, type=typ, off=off, size=size, es=es, link=link, info=info)
        order.append(i)
    shoff = e_shoff + e_shstrndx * e_shentsize
    s_off, s_size = u32(d, shoff+16), u32(d, shoff+20)
    shstr = d[s_off:s_off+s_size]
    for i in order:
        nm = shstr[secs[i]['name']:].split(b'\0')[0]
        secs[i]['nm'] = nm
    byname = {s['nm']: s for s in secs.values()}
    text, rodata = byname.get(b'.text'), byname.get(b'.rodata')
    assert text is not None, "no .text"
    for s in secs.values():
        if s['nm'] in (b'.data', b'.bss', b'.sbss') and s['size'] > 0:
            raise SystemExit(f"unsupported section {s['nm']} (demos must be text+rodata only)")
    # symtab
    syms = []
    for s in secs.values():
        if s['type'] == 2:
            for j in range(s['size'] // 16):
                e = d[s['off']+j*16:s['off']+(j+1)*16]
                st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack('<IIIBBH', e)
                syms.append((st_shndx, st_value))
    # layout
    tbase = base
    rbase = (tbase + text['size'] + 3) & ~3
    secbase = {}
    for i, s in secs.items():
        if s['nm'] == b'.text':
            secbase[i] = tbase
        elif s['nm'] == b'.rodata':
            secbase[i] = rbase
    img = bytearray(d[text['off']:text['off']+text['size']])
    if rodata is not None:
        img += b'\0' * (rbase - (tbase + text['size']))
        img += d[rodata['off']:rodata['off']+rodata['size']]
    def S(symidx, add):
        shndx, val = syms[symidx]
        if shndx == 0:
            raise SystemExit(f"undefined symbol idx {symidx}")
        return secbase[shndx] + val + add if shndx in secbase else val + add
    himap = {}
    # pass 1: HI20 targets
    rels = []
    for s in secs.values():
        if (s['type'] == 4 or s['type'] == 7) and s['es'] in (8, 12):
            for j in range(s['size'] // s['es']):
                e = d[s['off']+j*s['es']:s['off']+(j+1)*s['es']]
                if s['es'] == 8:
                    r_off, r_info = struct.unpack('<II', e)
                    add = 0
                else:
                    r_off, r_info, add = struct.unpack('<IIi', e)
                rels.append((r_off, r_info >> 8, r_info & 0xFF, add))
    for r_off, sym, typ, add in rels:
        if typ in (R_HI20, R_PCREL_HI20):
            # find containing section to get P
            sec = text if r_off < text['size'] else None
            assert sec is not None
            P = tbase + r_off
            T = S(sym, add)
            if typ == R_PCREL_HI20:
                # auipc/addi pair: value is PC-relative
                himap[P] = T
                v = ((T - P + 0x800) >> 12) & 0xFFFFF
            else:
                v = ((T + 0x800) >> 12) & 0xFFFFF
            w = u32(img, r_off)
            w = (w & 0xFFF) | (v << 12)
            img[r_off:r_off+4] = struct.pack('<I', w)
    # pass 2: everything else
    for r_off, sym, typ, add in rels:
        if typ in (R_HI20, R_PCREL_HI20, R_ALIGN, R_RELAX):
            continue
        sec = text if r_off < text['size'] else None
        assert sec is not None, f"reloc outside .text @{r_off:#x}"
        P = tbase + r_off
        w = u32(img, r_off)
        if typ == R_JAL:
            v = S(sym, add) - P
            assert -(1 << 20) <= v < (1 << 20) and v % 2 == 0, f"JAL range {v:#x}"
            v &= 0x1FFFFF
            enc = ((v >> 20) & 1) << 31 | ((v >> 1) & 0x3FF) << 21 | ((v >> 11) & 1) << 20 | ((v >> 12) & 0xFF) << 12
            w = (w & 0xFFF) | enc
        elif typ == R_BRANCH:
            v = S(sym, add) - P
            assert -(1 << 12) <= v < (1 << 12) and v % 2 == 0, f"BRANCH range {v:#x}"
            v &= 0x1FFF
            enc = ((v >> 12) & 1) << 31 | ((v >> 5) & 0x3F) << 25 | ((v >> 1) & 0xF) << 8 | ((v >> 11) & 1) << 7
            w = (w & 0x01FFF07F) | enc
        elif typ in (R_LO12_I, R_PCREL_LO12_I):
            if typ == R_PCREL_LO12_I:
                shndx, val = syms[sym]
                hisite = (secbase[shndx] + val) if shndx in secbase else val
                assert hisite in himap, f"LO12 without paired HI20 @{r_off:#x}"
                T = himap[hisite]
                # low part pairs with the auipc site (not this insn)
                hi = ((T - hisite + 0x800) >> 12) & 0xFFFFF
                v = (T - hisite - (hi << 12)) & 0xFFF
            else:
                T = S(sym, add)
                v = (T - P) & 0xFFF
            w = (w & 0xFFFFF) | (v << 20)
        elif typ == R_LO12_S:
            T = S(sym, add)
            v = (T - P) & 0xFFF
            w = (w & 0x1FFF07F) | ((v & 0xFE0) << 20) | ((v & 0x1F) << 7)
        elif typ == 1:  # R_RISCV_32 absolute
            w = S(sym, add) & 0xFFFFFFFF
        else:
            raise SystemExit(f"unsupported reloc type {typ} @{r_off:#x}")
        img[r_off:r_off+4] = struct.pack('<I', w)
    open(outp, 'wb').write(bytes(img))
    print(f"linked {len(img)} bytes, entry {tbase:#x}")

main()
