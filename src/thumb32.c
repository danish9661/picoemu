/*
 * Thumb-2 32-bit instruction handler for ARMv7-M compatibility
 *
 * Handles instruction groups:
 *   0xE8xx/0xE9xx: Load/Store Multiple T2 (LDMIA/STMIA/LDMDB/STMDB)
 *   0xEAxx/0xEBxx: Data Processing (shifted register)
 *   0xF0xx-0xF7xx: Data Processing (modified/plain immediate) + wide branches
 *   0xF8xx-0xFFxx: Load/Store single (all widths and modes)
 *
 * Also handles BL T1, MSR, MRS, DSB/DMB/ISB (previously in cpu.c).
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "emulator.h"
#include "instructions.h"
#include "nvic.h"
#include "thumb32.h"
#include "devtools.h"

/* External globals from cpu.c */
extern int pc_updated;

/* Flag bits in XPSR */
#define FLAG_N 0x80000000u
#define FLAG_Z 0x40000000u
#define FLAG_C 0x20000000u
#define FLAG_V 0x10000000u

/* Evaluate ARM condition code against current XPSR flags */
int t32_check_cond(uint8_t cond) {
    int N = (cpu.xpsr & FLAG_N) != 0;
    int Z = (cpu.xpsr & FLAG_Z) != 0;
    int C = (cpu.xpsr & FLAG_C) != 0;
    int V = (cpu.xpsr & FLAG_V) != 0;
    switch (cond & 0xF) {
    case 0x0: return  Z;
    case 0x1: return !Z;
    case 0x2: return  C;
    case 0x3: return !C;
    case 0x4: return  N;
    case 0x5: return !N;
    case 0x6: return  V;
    case 0x7: return !V;
    case 0x8: return  C && !Z;
    case 0x9: return !C ||  Z;
    case 0xA: return  N == V;
    case 0xB: return  N != V;
    case 0xC: return !Z && (N == V);
    case 0xD: return  Z || (N != V);
    case 0xE: return 1;  /* AL */
    default:  return 1;
    }
}

/* ========================================================================
 * Helper: Thumb-2 modified 12-bit immediate expansion
 * ======================================================================== */
static uint32_t thumb32_expand_imm(uint32_t imm12) {
    uint32_t b = imm12 & 0xFF;
    switch (imm12 >> 8) {
    case 0: return b;
    case 1: return (b << 16) | b;
    case 2: return (b << 24) | (b << 8);
    case 3: return (b << 24) | (b << 16) | (b << 8) | b;
    default: {
        /* Rotation: bit[7] is always 1 in the unrotated value */
        uint32_t val = 0x80 | (imm12 & 0x7F);
        uint32_t rot = (imm12 >> 7) & 0x1F;
        return (rot == 0) ? val : ((val >> rot) | (val << (32 - rot)));
    }
    }
}

/* ========================================================================
 * Helper: Barrel shifter with carry out
 * type: 0=LSL, 1=LSR, 2=ASR, 3=ROR
 * ======================================================================== */
static uint32_t t32_shift_c(uint32_t val, int type, int n, int *carry_out) {
    if (n == 0) {
        *carry_out = (cpu.xpsr & FLAG_C) ? 1 : 0;
        return val;
    }
    switch (type) {
    case 0: /* LSL */
        if (n >= 32) { *carry_out = (n == 32) ? (val & 1) : 0; return 0; }
        *carry_out = (val >> (32 - n)) & 1;
        return val << n;
    case 1: /* LSR */
        if (n > 32) { *carry_out = 0; return 0; }
        if (n == 32) { *carry_out = val >> 31; return 0; }
        *carry_out = (val >> (n - 1)) & 1;
        return val >> n;
    case 2: /* ASR */
        if (n >= 32) {
            *carry_out = val >> 31;
            return (val & 0x80000000u) ? 0xFFFFFFFFu : 0;
        }
        *carry_out = (val >> (n - 1)) & 1;
        return (uint32_t)((int32_t)val >> n);
    case 3: /* ROR */
        n &= 31;
        if (n == 0) { *carry_out = val >> 31; return val; }
        *carry_out = (val >> (n - 1)) & 1;
        return (val >> n) | (val << (32 - n));
    }
    *carry_out = 0;
    return val;
}

/* ========================================================================
 * Helper: Data processing ALU op (shared by shifted-reg and mod-imm)
 * op: AND=0,BIC=1,ORR=2,ORN=3,EOR=4,ADD=8,ADC=10,SBC=11,SUB=13,RSB=14
 * ======================================================================== */
static void t32_dp_exec(int op, int S, int Rn, int Rd, uint32_t imm32, int shift_c) {
    uint32_t rn  = cpu.r[Rn];
    uint32_t res = 0;
    int      wr  = 1;  /* write result to Rd */

    switch (op) {
    case 0x0: /* AND / TST (Rd=15,S) */
        res = rn & imm32;
        if (Rd == 15 && S) wr = 0;
        if (S) { update_nz_flags(res); if (shift_c) cpu.xpsr |= FLAG_C; else cpu.xpsr &= ~FLAG_C; }
        break;
    case 0x1: /* BIC */
        res = rn & ~imm32;
        if (S) { update_nz_flags(res); if (shift_c) cpu.xpsr |= FLAG_C; else cpu.xpsr &= ~FLAG_C; }
        break;
    case 0x2: /* ORR / MOV (Rn=15) */
        res = (Rn == 15) ? imm32 : (rn | imm32);
        if (S) { update_nz_flags(res); if (shift_c) cpu.xpsr |= FLAG_C; else cpu.xpsr &= ~FLAG_C; }
        break;
    case 0x3: /* ORN / MVN (Rn=15) */
        res = (Rn == 15) ? ~imm32 : (rn | ~imm32);
        if (S) { update_nz_flags(res); if (shift_c) cpu.xpsr |= FLAG_C; else cpu.xpsr &= ~FLAG_C; }
        break;
    case 0x4: /* EOR / TEQ (Rd=15,S) */
        res = rn ^ imm32;
        if (Rd == 15 && S) wr = 0;
        if (S) { update_nz_flags(res); if (shift_c) cpu.xpsr |= FLAG_C; else cpu.xpsr &= ~FLAG_C; }
        break;
    case 0x8: { /* ADD / CMN (Rd=15,S) */
        res = rn + imm32;
        if (Rd == 15 && S) wr = 0;
        if (S) update_add_flags(rn, imm32, res);
        break;
    }
    case 0xA: { /* ADC */
        uint32_t c = (cpu.xpsr & FLAG_C) ? 1u : 0u;
        uint64_t r64 = (uint64_t)rn + imm32 + c;
        res = (uint32_t)r64;
        if (S) {
            update_add_flags(rn, imm32 + c, res);
            cpu.xpsr &= ~FLAG_C;
            if (r64 > 0xFFFFFFFFULL) cpu.xpsr |= FLAG_C;
        }
        break;
    }
    case 0xB: { /* SBC */
        uint32_t c = (cpu.xpsr & FLAG_C) ? 1u : 0u;
        uint32_t b = 1u - c;
        res = rn - imm32 - b;
        if (S) {
            /* Carry = NOT borrow from rn - imm32 - b in full precision.
             * update_sub_flags(rn, imm32, res) is WRONG here: it ignores
             * the borrow-in, so C stays set when rn == imm32 with borrow
             * (broke 64-bit timeout loops like hstx_dvi_start's
             * cmp/sbcs/bcc, which never fired). Mirrors instr_sbcs. */
            cpu.xpsr &= ~(FLAG_N | FLAG_Z | FLAG_C | FLAG_V);
            if (res == 0) cpu.xpsr |= FLAG_Z;
            if (res & 0x80000000u) cpu.xpsr |= FLAG_N;
            uint64_t ures = (uint64_t)rn - (uint64_t)imm32 - (uint64_t)b;
            if ((ures >> 32) == 0) cpu.xpsr |= FLAG_C;
            if (((rn ^ imm32) & (rn ^ res)) & 0x80000000u) cpu.xpsr |= FLAG_V;
        }
        break;
    }
    case 0xD: /* SUB / CMP (Rd=15,S) */
        res = rn - imm32;
        if (Rd == 15 && S) wr = 0;
        if (S) update_sub_flags(rn, imm32, res);
        break;
    case 0xE: /* RSB */
        res = imm32 - rn;
        if (S) update_sub_flags(imm32, rn, res);
        break;
    default:
        /* Unknown ops are typically misrouted instructions from other
         * T32 groups. Treat as no-op rather than HardFault to allow
         * firmware to continue. Real hardware executes the correct
         * handler, not the data-processing path. */
        if (cpu.debug_enabled)
            fprintf(stderr, "[T32] Unknown dp op=0x%X @ PC=0x%08X (no-op)\n", op, cpu.r[15]);
        wr = 0;
        break;
    }
    if (wr && Rd < 16) {
        if (Rd == 15) { cpu.r[15] = res & ~1u; pc_updated = 1; }
        else           { cpu.r[Rd] = res; }
    }
}

/* ========================================================================
 * BL T1 (already handled in cpu.c but also handled here for completeness)
 * upper: 1111 0 S imm10
 * lower: 1 1 J1 1 J2 imm11
 * ======================================================================== */
static void t32_bl(uint32_t pc, uint16_t upper, uint16_t lower) {
    uint32_t S     = (upper >> 10) & 1;
    uint32_t imm10 = upper & 0x3FF;
    uint32_t J1    = (lower >> 13) & 1;
    uint32_t J2    = (lower >> 11) & 1;
    uint32_t imm11 = lower & 0x7FF;
    uint32_t I1    = !(J1 ^ S) & 1;
    uint32_t I2    = !(J2 ^ S) & 1;
    int32_t  offset = (int32_t)((S << 24) | (I1 << 23) | (I2 << 22) | (imm10 << 12) | (imm11 << 1));
    if (S) offset |= (int32_t)0xFF000000;  /* sign-extend from bit 24 */
    cpu.r[14] = (pc + 4) | 1u;
    uint32_t target = (uint32_t)((int32_t)(pc + 4) + offset);
    cpu.r[15] = target;
    pc_updated = 1;
    if (__builtin_expect(callgraph_enabled, 0))
        callgraph_record_call(pc, target);
}

/* ========================================================================
 * Load/Store Multiple T2
 * upper: 1110 1000 W L Rn(4)   (IA: bit8=0)
 *        1110 1001 W L Rn(4)   (DB: bit8=1)
 * lower: 0 P M 0 reglist(12)   (bit15=P=PC, bit14=M=LR)
 * ======================================================================== */
static void t32_ldst_multiple(uint32_t pc, uint16_t upper, uint16_t lower) {
    (void)pc;
    int is_db = (upper >> 8) & 1;
    int L     = (upper >> 4) & 1; /* bit4: 1=LDM, 0=STM (E8BC vs E8AC) */
    int W     = (upper >> 5) & 1;
    int Rn    = upper & 0xF;

    uint32_t reglist = lower;   /* bit15=PC, bit14=LR, bits[13:0] = R13..R0 */
    int      cnt     = __builtin_popcount(reglist & 0xFFFF);
    uint32_t addr    = cpu.r[Rn];

    if (is_db) addr -= (uint32_t)(cnt * 4);
    uint32_t base_end = addr + (uint32_t)(cnt * 4);

    for (int i = 0; i <= 15; i++) {
        if (reglist & (1u << i)) {
            if (L) {
                uint32_t val = mem_read32(addr);
                if (i == 15) { cpu.r[15] = val & ~1u; pc_updated = 1; }
                else          { cpu.r[i] = val; }
            } else {
                uint32_t val = (i == 15) ? (pc + 4) : cpu.r[i];
                mem_write32(addr, val);
            }
            addr += 4;
        }
    }

    if (W && !(L && (reglist & (1u << Rn)))) {
        /* Writeback: IA → updated addr, DB → original - count*4 */
        cpu.r[Rn] = is_db ? (cpu.r[Rn] - (uint32_t)(cnt * 4)) : base_end;
    }
}

/* ========================================================================
 * Load/Store Double
 * upper: 1110 1101 W L Rn   (load/store with imm8, P/U from lower)
 * upper pattern: (upper & 0xFE50) == 0xE840 → STRD, 0xE850 → LDRD
 * lower: Rt(4) Rt2(4) P U W imm8
 * Actually: upper = 1110 1 0 P U 1 W 0 L Rn
 * ======================================================================== */
static void t32_ldrd_strd(uint32_t pc, uint16_t upper, uint16_t lower) {
    (void)pc;
    int P   = (upper >> 8) & 1;
    int U   = (upper >> 7) & 1;
    int W   = (upper >> 5) & 1;
    int L   = (upper >> 4) & 1;
    int Rn  = upper & 0xF;
    int Rt  = (lower >> 12) & 0xF;
    int Rt2 = (lower >> 8) & 0xF;
    int imm8 = (lower & 0xFF) << 2;

    uint32_t base = cpu.r[Rn];
    uint32_t offset_addr = U ? (base + imm8) : (base - imm8);
    uint32_t addr = P ? offset_addr : base;

    if (L) {
        cpu.r[Rt]  = mem_read32(addr);
        cpu.r[Rt2] = mem_read32(addr + 4);
    } else {
        mem_write32(addr,     cpu.r[Rt]);
        mem_write32(addr + 4, cpu.r[Rt2]);
    }
    if (W) cpu.r[Rn] = offset_addr;
}

/* ========================================================================
 * Table Branch
 * upper: 1110 1000 1101 Rn   → TBB/TBH
 * lower: 1111 0000 H Rm
 * ======================================================================== */
static void t32_tbb_tbh(uint32_t pc, uint16_t upper, uint16_t lower) {
    int Rn = upper & 0xF;
    int H  = (lower >> 4) & 1;
    int Rm = lower & 0xF;
    uint32_t base  = (Rn == 15) ? (pc + 4) : cpu.r[Rn];
    uint32_t index = cpu.r[Rm];
    uint32_t addr  = base + (H ? index * 2 : index);
    uint32_t offset;
    if (H) offset = mem_read16(addr) * 2u;
    else   offset = mem_read8(addr) * 2u;
    cpu.r[15] = (pc + 4) + offset;
    pc_updated = 1;
}

/* ========================================================================
 * Data Processing: shifted register
 * upper: 1110 1010 op(4) S Rn(4)
 *        1110 1011 op(4) S Rn(4)   (op has bit3 set)
 * lower: 0 imm3(3) Rd(4) imm2(2) type(2) Rm(4)
 * ======================================================================== */
static void t32_dp_shifted_reg(uint32_t pc, uint16_t upper, uint16_t lower) {
    (void)pc;
    int      op    = (upper >> 5) & 0xF;
    int      S     = (upper >> 4) & 1;
    int      Rn    = upper & 0xF;
    int      imm3  = (lower >> 12) & 7;
    int      Rd    = (lower >> 8) & 0xF;
    int      imm2  = (lower >> 6) & 3;
    int      type  = (lower >> 4) & 3;
    int      Rm    = lower & 0xF;
    int      n     = (imm3 << 2) | imm2;
    int      carry = 0;
    uint32_t shifted;
    if (type == 3 && n == 0) {
        /* RRX (DecodeImmShift: ROR with imm3:imm2 == 0). Carry_in goes to
         * bit31, carry_out is old bit0. S==0 preserves flags (t32_dp_exec
         * only writes flags when S set). pico_double __aeabi_f2d relies on
         * the stale C from `lsls` surviving `mov.w`+ASR into this RRX;
         * without it every float->double promotion (printf %.1f) broke. */
        uint32_t rmv = cpu.r[Rm];
        carry = rmv & 1;
        shifted = ((cpu.xpsr & FLAG_C) ? 0x80000000u : 0) | (rmv >> 1);
    } else {
        if (type != 0 && n == 0) n = 32;  /* LSR#32 / ASR#32 */
        shifted = t32_shift_c(cpu.r[Rm], type, n, &carry);
    }
    t32_dp_exec(op, S, Rn, Rd, shifted, carry);
}

/* ========================================================================
 * Data Processing: modified immediate
 * upper: 1111 0 i op(4) S Rn(4)
 * lower: 0 imm3(3) Rd(4) imm8(8)
 * ======================================================================== */
static void t32_dp_mod_imm(uint32_t pc, uint16_t upper, uint16_t lower) {
    (void)pc;
    uint32_t i     = (upper >> 10) & 1;
    int      op    = (upper >> 5) & 0xF;
    int      S     = (upper >> 4) & 1;
    int      Rn    = upper & 0xF;
    uint32_t imm3  = (lower >> 12) & 7;
    int      Rd    = (lower >> 8) & 0xF;
    uint32_t imm8  = lower & 0xFF;
    uint32_t imm12 = (i << 11) | (imm3 << 8) | imm8;
    uint32_t imm32 = thumb32_expand_imm(imm12);
    /* carry from expansion: MSB of result for rotation case */
    int carry = (imm32 >> 31) & 1;
    t32_dp_exec(op, S, Rn, Rd, imm32, carry);
}

/* ========================================================================
 * Wide branches: Bcc.W T3 and B.W T4
 * Bcc.W T3: upper = 1111 0 S cond(4) imm6, lower = 1 0 J1 0 J2 imm11
 * B.W T4:   upper = 1111 0 S imm10,        lower = 1 0 J1 1 J2 imm11
 * ======================================================================== */
static void t32_branch(uint32_t pc, uint16_t upper, uint16_t lower) {
    uint32_t S    = (upper >> 10) & 1;
    uint32_t J1   = (lower >> 13) & 1;
    uint32_t J2   = (lower >> 11) & 1;
    uint32_t imm11 = lower & 0x7FF;

    int is_bw = (lower >> 12) & 1;  /* bit12=1 → B.W T4, bit12=0 → Bcc.W T3 */

    if (is_bw) {
        /* B.W T4: same offset decode as BL */
        uint32_t imm10 = upper & 0x3FF;
        uint32_t I1    = (!(J1 ^ S)) & 1u;
        uint32_t I2    = (!(J2 ^ S)) & 1u;
        int32_t offset = (int32_t)((S << 24) | (I1 << 23) | (I2 << 22) | (imm10 << 12) | (imm11 << 1));
        if (S) offset |= (int32_t)0xFF000000;
        cpu.r[15] = (uint32_t)((int32_t)(pc + 4) + offset);
        pc_updated = 1;
    } else {
        /* Bcc.W T3: offset from {S,J2,J1,imm6,imm11,0} */
        uint32_t imm6  = upper & 0x3F;
        uint8_t  cond  = (upper >> 6) & 0xF;
        int32_t offset = (int32_t)((S << 20) | (J2 << 19) | (J1 << 18) | (imm6 << 12) | (imm11 << 1));
        if (S) offset |= (int32_t)0xFFE00000;  /* sign extend from bit 20 */
        if (t32_check_cond(cond)) {
            cpu.r[15] = (uint32_t)((int32_t)(pc + 4) + offset);
            pc_updated = 1;
        }
        /* If condition fails, fall through (pc += 4 done by caller) */
    }
}

/* ========================================================================
 * Data Processing: plain binary immediate (MOVW, MOVT, ADDW, SUBW)
 * upper: 1111 0 i 1 op(4) 0 imm4
 * lower: 0 imm3(3) Rd(4) imm8(8)
 * ======================================================================== */
static void t32_dp_plain_imm(uint32_t pc, uint16_t upper, uint16_t lower) {
    (void)pc;
    uint32_t i    = (upper >> 10) & 1;
    uint32_t op   = (upper >> 4) & 0x1F;   /* bits[8:4] of upper */
    uint32_t imm4 = upper & 0xF;
    uint32_t imm3 = (lower >> 12) & 7;
    int      Rd   = (lower >> 8) & 0xF;
    uint32_t imm8 = lower & 0xFF;
    uint32_t Rn   = imm4;  /* For ADD/SUB plain, Rn is in upper[3:0] */

    /* MOVW T3: op[4:0] = 00100, i.e., upper bits[8:4] = 00100 = 0x04 */
    /* upper & 0x01F0 isolates bits[8:4]: */
    uint32_t op5 = (upper >> 4) & 0x1F;  /* bits[8:4] */

    if (op5 == 0x04) {
        /* MOVW T3: imm16 = imm4:i:imm3:imm8 */
        uint32_t imm16 = (imm4 << 12) | (i << 11) | (imm3 << 8) | imm8;
        cpu.r[Rd] = imm16;
        return;
    }
    if (op5 == 0x0C) {
        /* MOVT T1: top halfword = imm4:i:imm3:imm8 */
        uint32_t imm16 = (imm4 << 12) | (i << 11) | (imm3 << 8) | imm8;
        cpu.r[Rd] = (cpu.r[Rd] & 0x0000FFFF) | (imm16 << 16);
        return;
    }

    /* ADDW T4: op5 = 0x00 → ADD with imm12 (zero-extended) */
    uint32_t imm12 = (i << 11) | (imm3 << 8) | imm8;
    if (op5 == 0x00) {
        /* ADD Rd, Rn, #imm12 */
        if (Rn == 15) {
            /* ADR T3 */
            uint32_t base = (pc + 4) & ~3u;
            cpu.r[Rd] = base + imm12;
        } else {
            cpu.r[Rd] = cpu.r[Rn] + imm12;
        }
        return;
    }
    if (op5 == 0x0A) {
        /* SUB Rd, Rn, #imm12 (SUBW T4) */
        if (Rn == 15) {
            /* ADR T2 (subtract) */
            uint32_t base = (pc + 4) & ~3u;
            cpu.r[Rd] = base - imm12;
        } else {
            cpu.r[Rd] = cpu.r[Rn] - imm12;
        }
        return;
    }

    /* Unhandled plain-binary-imm op */
    if (cpu.debug_enabled)
        fprintf(stderr, "[T32] Unhandled plain-imm op5=0x%02X upper=0x%04X @ PC=0x%08X\n",
                op5, upper, pc);
    cpu_exception_entry(EXC_HARDFAULT);
    (void)op;
}

/* ========================================================================
 * Miscellaneous 32-bit: MSR, MRS, DSB/DMB/ISB, UDIV, SDIV, MLA, MLS, etc.
 * ======================================================================== */
static int t32_misc(uint32_t pc, uint16_t upper, uint16_t lower) {
    /* MSR T1: upper=0xF380|Rn, lower=0x88xx */
    if ((upper & 0xFFF0) == 0xF380 && (lower & 0xFF00) == 0x8800) {
        uint8_t rn   = upper & 0xF;
        uint8_t sysm = lower & 0xFF;
        instr_msr_32(rn, sysm);
        return 1;
    }
    /* MRS T1: upper=0xF3EF, lower=0x8Rss */
    if ((upper & 0xFFFF) == 0xF3EF && (lower & 0xF000) == 0x8000) {
        uint8_t rd   = (lower >> 8) & 0xF;
        uint8_t sysm = lower & 0xFF;
        instr_mrs_32(rd, sysm);
        return 1;
    }
    /* DSB/DMB/ISB: upper=0xF3BF, lower=0x8Fxx */
    if ((upper & 0xFFFF) == 0xF3BF && (lower & 0xFF00) == 0x8F00) {
        /* Memory barriers are NOPs in emulator */
        return 1;
    }
    /* SDIV T1: upper = 1111 1011 1001 Rn, lower = 1111 Rd 1111 Rm */
    if ((upper & 0xFFF0) == 0xFB90 && (lower & 0xF0F0) == 0xF0F0) {
        int Rd = (lower >> 8) & 0xF;
        int Rn = upper & 0xF;
        int Rm = lower & 0xF;
        int32_t divisor = (int32_t)cpu.r[Rm];
        if (divisor == 0) cpu.r[Rd] = 0;
        else cpu.r[Rd] = (uint32_t)((int32_t)cpu.r[Rn] / divisor);
        return 1;
    }
    /* UDIV T1: upper = 1111 1011 1011 Rn, lower = 1111 Rd 1111 Rm */
    if ((upper & 0xFFF0) == 0xFBB0 && (lower & 0xF0F0) == 0xF0F0) {
        int Rd = (lower >> 8) & 0xF;
        int Rn = upper & 0xF;
        int Rm = lower & 0xF;
        uint32_t divisor = cpu.r[Rm];
        cpu.r[Rd] = (divisor == 0) ? 0 : (cpu.r[Rn] / divisor);
        return 1;
    }
    /* MUL T2: upper = 1111 1011 0000 Rn, lower = 1111 Rd 0000 Rm */
    if ((upper & 0xFFF0) == 0xFB00 && (lower & 0xF0F0) == 0xF000) {
        int Rd = (lower >> 8) & 0xF;
        int Rn = upper & 0xF;
        int Rm = lower & 0xF;
        cpu.r[Rd] = cpu.r[Rn] * cpu.r[Rm];
        return 1;
    }
    /* MLA T1: upper = 1111 1011 0000 Rn, lower = Ra Rd 0000 Rm (Ra != 1111) */
    if ((upper & 0xFFF0) == 0xFB00 && (lower & 0x00F0) == 0x0000) {
        int Rd = (lower >> 8) & 0xF;
        int Ra = (lower >> 12) & 0xF;
        int Rn = upper & 0xF;
        int Rm = lower & 0xF;
        if (Ra != 15) { cpu.r[Rd] = cpu.r[Rn] * cpu.r[Rm] + cpu.r[Ra]; return 1; }
    }
    /* MLS T1: upper = 1111 1011 0000 Rn, lower = Ra Rd 0001 Rm */
    if ((upper & 0xFFF0) == 0xFB00 && (lower & 0x00F0) == 0x0010) {
        int Rd = (lower >> 8) & 0xF;
        int Ra = (lower >> 12) & 0xF;
        int Rn = upper & 0xF;
        int Rm = lower & 0xF;
        cpu.r[Rd] = cpu.r[Ra] - cpu.r[Rn] * cpu.r[Rm];
        return 1;
    }
    /* SMULL T1: upper = 1111 1011 1000 Rn, lower = RdLo RdHi 0000 Rm */
    if ((upper & 0xFFF0) == 0xFB80 && (lower & 0x00F0) == 0x0000) {
        int RdLo = (lower >> 12) & 0xF;
        int RdHi = (lower >> 8) & 0xF;
        int Rn   = upper & 0xF;
        int Rm   = lower & 0xF;
        int64_t result = (int64_t)(int32_t)cpu.r[Rn] * (int64_t)(int32_t)cpu.r[Rm];
        cpu.r[RdLo] = (uint32_t)result;
        cpu.r[RdHi] = (uint32_t)(result >> 32);
        return 1;
    }
    /* UMULL T1: upper = 1111 1011 1010 Rn, lower = RdLo RdHi 0000 Rm */
    if ((upper & 0xFFF0) == 0xFBA0 && (lower & 0x00F0) == 0x0000) {
        int RdLo = (lower >> 12) & 0xF;
        int RdHi = (lower >> 8) & 0xF;
        int Rn   = upper & 0xF;
        int Rm   = lower & 0xF;
        uint64_t result = (uint64_t)cpu.r[Rn] * (uint64_t)cpu.r[Rm];
        cpu.r[RdLo] = (uint32_t)result;
        cpu.r[RdHi] = (uint32_t)(result >> 32);
        return 1;
    }
    /* UMLAL T1: upper = 1111 1011 1110 Rn, lower = RdLo RdHi 0000 Rm */
    if ((upper & 0xFFF0) == 0xFBE0 && (lower & 0x00F0) == 0x0000) {
        int RdLo = (lower >> 12) & 0xF;
        int RdHi = (lower >> 8) & 0xF;
        int Rn   = upper & 0xF;
        int Rm   = lower & 0xF;
        uint64_t acc    = ((uint64_t)cpu.r[RdHi] << 32) | cpu.r[RdLo];
        uint64_t result = acc + (uint64_t)cpu.r[Rn] * (uint64_t)cpu.r[Rm];
        cpu.r[RdLo] = (uint32_t)result;
        cpu.r[RdHi] = (uint32_t)(result >> 32);
        return 1;
    }
    /* SMLAL T1: upper = 1111 1011 1100 Rn, lower = RdLo RdHi 0000 Rm */
    if ((upper & 0xFFF0) == 0xFBC0 && (lower & 0x00F0) == 0x0000) {
        int RdLo = (lower >> 12) & 0xF;
        int RdHi = (lower >> 8) & 0xF;
        int Rn   = upper & 0xF;
        int Rm   = lower & 0xF;
        int64_t  acc    = (int64_t)(((uint64_t)cpu.r[RdHi] << 32) | cpu.r[RdLo]);
        int64_t  result = acc + (int64_t)(int32_t)cpu.r[Rn] * (int64_t)(int32_t)cpu.r[Rm];
        cpu.r[RdLo] = (uint32_t)result;
        cpu.r[RdHi] = (uint32_t)((uint64_t)result >> 32);
        return 1;
    }
    /* SMMULR/SMMLAR/SMMUL/SMMLA T1: upper = 1111 1011 0101 Rn,
     * lower = Ra Rd op2 Rm (op2: 0001 round, 0000 truncate; Ra=15 means
     * no accumulate). Pico_double ddiv/dsqrt iteration needs these;
     * previously fell into LDR.W-T2 and silently loaded garbage.
     * Encodings verified against arm-none-eabi-as -mcpu=cortex-m33. */
    if ((upper & 0xFFF0) == 0xFB50 && ((lower & 0x00F0) == 0x0010 ||
                                       (lower & 0x00F0) == 0x0000)) {
        int Rn = upper & 0xF;
        int Ra = (lower >> 12) & 0xF;
        int Rd = (lower >> 8) & 0xF;
        int Rm = lower & 0xF;
        int round = ((lower >> 4) & 1) != 0; /* op2 bit0: 1 round, 0 trunc */
        int64_t prod = (int64_t)(int32_t)cpu.r[Rn] * (int64_t)(int32_t)cpu.r[Rm];
        uint32_t hi = (uint32_t)(prod >> 32);
        if (Rd != 15) {
            if (round) {
                uint32_t rounded = (uint32_t)(((uint64_t)prod + 0x80000000ULL) >> 32);
                cpu.r[Rd] = (Ra == 15) ? rounded : rounded + cpu.r[Ra];
            } else {
                cpu.r[Rd] = (Ra == 15) ? hi : hi + cpu.r[Ra];
            }
        }
        return 1;
    }
    /* SMUAD/SMUSD T1: upper = 1111 1011 0110 Rn, lower = 1111 Rd 000M Rm. */
    if ((upper & 0xFFF0) == 0xFB60 && (lower & 0xF0E0) == 0xF000) {
        int op = (lower >> 4) & 1; /* 0=UAD, 1=USD */
        int Rn = upper & 0xF;
        int Rd = (lower >> 8) & 0xF;
        int Rm = lower & 0xF;
        int32_t a = (int32_t)cpu.r[Rn], b = (int32_t)cpu.r[Rm];
        int32_t p1 = (int16_t)(a & 0xFFFF) * (int16_t)(b & 0xFFFF);
        int32_t p2 = (int16_t)((a >> 16) & 0xFFFF) * (int16_t)((b >> 16) & 0xFFFF);
        if (Rd != 15) cpu.r[Rd] = (uint32_t)(op ? (p1 - p2) : (p1 + p2));
        return 1;
    }
    /* SMMLSR T1: upper = 1111 1011 0110 Rn, lower = Ra Rd 0001 Rm
     * (Ra==15 would be SMUSD, handled above). */
    if ((upper & 0xFFF0) == 0xFB60 && (lower & 0x00F0) == 0x0010 &&
        ((lower >> 12) & 0xF) != 15) {
        int Rn = upper & 0xF;
        int Ra = (lower >> 12) & 0xF;
        int Rd = (lower >> 8) & 0xF;
        int Rm = lower & 0xF;
        int64_t prod = (int64_t)(int32_t)cpu.r[Rn] * (int64_t)(int32_t)cpu.r[Rm];
        uint32_t hi = (uint32_t)(prod >> 32);
        if (Rd != 15) cpu.r[Rd] = hi - cpu.r[Ra];
        return 1;
    }
    /* CLZ T1: upper = 1111 1010 1011 Rm, lower = 1111 Rd 1000 Rm.
     * The mask must exclude the variable Rd field (0xF0F0, not 0xF0FF),
     * or any nonzero Rd (e.g. clz r3, r2 in littleos_pico2 division)
     * falls through to a bogus load/store decode. */
    if ((upper & 0xFFF0) == 0xFAB0 && (lower & 0xF0F0) == 0xF080) {
        int Rd = (lower >> 8) & 0xF;
        int Rm = lower & 0xF;
        cpu.r[Rd] = cpu.r[Rm] ? __builtin_clz(cpu.r[Rm]) : 32;
        return 1;
    }
    /* RBIT T1: upper = 1111 1010 1001 Rm, lower = 1111 Rd 1010 Rm
     * (mask excludes variable Rd, see CLZ above). */
    if ((upper & 0xFFF0) == 0xFA90 && (lower & 0xF0F0) == 0xF0A0) {
        int Rd = (lower >> 8) & 0xF;
        int Rm = lower & 0xF;
        uint32_t v = cpu.r[Rm], r = 0;
        for (int b = 0; b < 32; b++) { r = (r << 1) | (v & 1); v >>= 1; }
        cpu.r[Rd] = r;
        return 1;
    }
    /* UBFX T1: upper = 1111 0011 1100 Rn, lower = 0 imm3 Rd imm2 widthm1 */
    if ((upper & 0xFFF0) == 0xF3C0 && (lower & 0x8000) == 0x0000) {
        int Rd      = (lower >> 8) & 0xF;
        int Rn      = upper & 0xF;
        int lsbit   = ((lower >> 12) & 7) << 2 | ((lower >> 6) & 3);
        int widthm1 = lower & 0x1F;
        uint32_t mask = (widthm1 == 31) ? 0xFFFFFFFF : ((1u << (widthm1 + 1)) - 1);
        cpu.r[Rd] = (cpu.r[Rn] >> lsbit) & mask;
        return 1;
    }
    /* SBFX T1: upper = 1111 0011 0100 Rn */
    if ((upper & 0xFFF0) == 0xF340 && (lower & 0x8000) == 0x0000) {
        int Rd      = (lower >> 8) & 0xF;
        int Rn      = upper & 0xF;
        int lsbit   = ((lower >> 12) & 7) << 2 | ((lower >> 6) & 3);
        int widthm1 = lower & 0x1F;
        uint32_t extracted = (cpu.r[Rn] >> lsbit) & ((widthm1 == 31) ? 0xFFFFFFFF : ((1u << (widthm1+1))-1));
        /* Sign-extend from bit widthm1 */
        if (extracted >> widthm1) extracted |= ~((1u << (widthm1 + 1)) - 1);
        cpu.r[Rd] = extracted;
        return 1;
    }
    /* USAT/SSAT T1: upper = 1111 0011 U0 sh Rm (U=1 USAT F38x,
     * U=0 SSAT F30x-F33x), lower = imm3 Rd imm2 sat[4:0].
     * shift: sh=0 LSL amount, sh=1 ASR amount (0 means 32).
     * sat field is 0-31 for USAT, 1-32 for SSAT. Encodings verified
     * against arm-none-eabi-as -mcpu=cortex-m33. Was unhandled ->
     * HardFault in pico_double double2fix64_z (Sage print of floats). */
    if (((upper & 0xFFC0) == 0xF300 || (upper & 0xFFC0) == 0xF380) &&
        (lower & 0x8000) == 0x0000) {
        int is_usat = (upper >> 7) & 1;
        int sh      = (upper >> 5) & 1;
        int Rm      = upper & 0xF;
        int Rd      = (lower >> 8) & 0xF;
        int amount  = (((lower >> 12) & 7) << 2) | ((lower >> 6) & 3);
        int sat     = lower & 0x1F;
        uint32_t v  = cpu.r[Rm];
        int32_t s;
        if (sh) s = (amount == 0) ? ((v & 0x80000000u) ? -1 : 0)
                                  : (int32_t)v >> amount;
        else    s = (int32_t)(amount ? (v << amount) : v);
        int satn = is_usat ? sat : sat + 1;
        int32_t res;
        int q = 0;
        if (is_usat) {
            uint32_t hi = (satn >= 32) ? 0xFFFFFFFFu : ((1u << satn) - 1);
            if (s < 0) { res = 0; q = 1; }
            else if ((uint32_t)s > hi) { res = (int32_t)hi; q = 1; }
            else res = s;
        } else {
            int32_t lo = (satn >= 32) ? (int32_t)0x80000000 : -(int32_t)(1u << (satn - 1));
            int32_t hi = (satn >= 32) ? (int32_t)0x7FFFFFFF : (int32_t)((1u << (satn - 1)) - 1);
            if (s < lo) { res = lo; q = 1; }
            else if (s > hi) { res = hi; q = 1; }
            else res = s;
        }
        if (Rd != 15) cpu.r[Rd] = (uint32_t)res;
        if (q) cpu.xpsr |= (1u << 27); /* Q sticky saturation flag */
        return 1;
    }
    /* BFI T1 / BFC T1: upper = 1111 0011 0110 Rn, lower = 0 imm3 Rd imm2 0 msbit */
    if ((upper & 0xFFF0) == 0xF360 && (lower & 0x8020) == 0x0000) {
        int Rd     = (lower >> 8) & 0xF;
        int Rn     = upper & 0xF;
        int lsbit  = ((lower >> 12) & 7) << 2 | ((lower >> 6) & 3);
        int msbit  = lower & 0x1F;
        if (msbit >= lsbit) {
            uint32_t width = msbit - lsbit + 1;
            uint32_t mask  = ((1u << width) - 1) << lsbit;
            uint32_t src   = (Rn == 15) ? 0 : cpu.r[Rn];
            cpu.r[Rd] = (cpu.r[Rd] & ~mask) | ((src << lsbit) & mask);
        }
        return 1;
    }
    /* SXTB T2: upper = 1111 1010 0100 1111, lower = 1111 Rd 10RR Rm */
    if ((upper & 0xFFFF) == 0xFA4F && (lower & 0xF0C0) == 0xF080) {
        int Rd  = (lower >> 8) & 0xF;
        int Rm  = lower & 0xF;
        int rot = ((lower >> 4) & 3) * 8;
        uint32_t val = cpu.r[Rm];
        if (rot) val = (val >> rot) | (val << (32 - rot));
        cpu.r[Rd] = (uint32_t)(int32_t)(int8_t)(val & 0xFF);
        return 1;
    }
    /* SXTH T2: upper = 1111 1010 0000 1111 */
    if ((upper & 0xFFFF) == 0xFA0F && (lower & 0xF0C0) == 0xF080) {
        int Rd  = (lower >> 8) & 0xF;
        int Rm  = lower & 0xF;
        int rot = ((lower >> 4) & 3) * 8;
        uint32_t val = cpu.r[Rm];
        if (rot) val = (val >> rot) | (val << (32 - rot));
        cpu.r[Rd] = (uint32_t)(int32_t)(int16_t)(val & 0xFFFF);
        return 1;
    }
    /* UXTB T2: upper = 1111 1010 0101 1111 */
    if ((upper & 0xFFFF) == 0xFA5F && (lower & 0xF0C0) == 0xF080) {
        int Rd  = (lower >> 8) & 0xF;
        int Rm  = lower & 0xF;
        int rot = ((lower >> 4) & 3) * 8;
        uint32_t val = cpu.r[Rm];
        if (rot) val = (val >> rot) | (val << (32 - rot));
        cpu.r[Rd] = val & 0xFF;
        return 1;
    }
    /* UXTH T2: upper = 1111 1010 0001 1111 */
    if ((upper & 0xFFFF) == 0xFA1F && (lower & 0xF0C0) == 0xF080) {
        int Rd  = (lower >> 8) & 0xF;
        int Rm  = lower & 0xF;
        int rot = ((lower >> 4) & 3) * 8;
        uint32_t val = cpu.r[Rm];
        if (rot) val = (val >> rot) | (val << (32 - rot));
        cpu.r[Rd] = val & 0xFFFF;
        return 1;
    }
    /* SXTAB/SXTAH/UXTAB/UXTAH T1 (extend + add): upper = 1111_1010_op_Rn
     *   op: 0100=SXTAB 0000=SXTAH 0101=UXTAB 0001=UXTAH
     * Rd = Rn + Zero/SignExtend(ROR(Rm, rot)). M33 compilers emit these
     * (e.g. lwIP pbuf_header); without them the pattern falls into the
     * T2 load/store path and can even alias Rt=15 (wild PC load).
     * Placed after the exact Rn=15 plain SXT/UXT checks above. */
    if (((upper & 0xFFF0) == 0xFA40 || (upper & 0xFFF0) == 0xFA00 ||
         (upper & 0xFFF0) == 0xFA50 || (upper & 0xFFF0) == 0xFA10) &&
        (lower & 0xF0C0) == 0xF080) {
        int Rn  = upper & 0xF;
        int Rd  = (lower >> 8) & 0xF;
        int rot = ((lower >> 4) & 3) * 8;
        int Rm  = lower & 0xF;
        uint32_t val = cpu.r[Rm];
        if (rot) val = (val >> rot) | (val << (32 - rot));
        int op = (upper >> 4) & 0xF;
        uint32_t ext;
        if (op == 0x4)      ext = (uint32_t)(int32_t)(int8_t)(val & 0xFF);
        else if (op == 0x0) ext = (uint32_t)(int32_t)(int16_t)(val & 0xFFFF);
        else if (op == 0x5) ext = val & 0xFF;
        else                ext = val & 0xFFFF;
        if (Rd == 15) { cpu.r[15] = cpu.r[Rn] + ext; pc_updated = 1; }
        else cpu.r[Rd] = cpu.r[Rn] + ext;
        return 1;
    }
    /* REV T2: upper = 1111 1010 1001 Rm, lower = 1111 Rd 1000 Rm
     * (mask excludes variable Rd, see CLZ above). */
    if ((upper & 0xFFF0) == 0xFA90 && (lower & 0xF0F0) == 0xF080) {
        int Rd = (lower >> 8) & 0xF;
        int Rm = lower & 0xF;
        uint32_t v = cpu.r[Rm];
        cpu.r[Rd] = ((v&0xFF)<<24)|((v&0xFF00)<<8)|((v&0xFF0000)>>8)|((v>>24)&0xFF);
        return 1;
    }
    /* LSL/LSR/ASR/ROR (register) T2: upper = 1111 1010 op(2) S Rn,
     * lower = 1111 Rd 0000 Rm (e.g. FA02 F101 = lsl.w r1, r2, r1).
     * SDK clock code uses lsl.w to build its SELECTED wait mask; falling
     * through to ldst_single left Rd unchanged and hung the wait forever.
     * Placed after the exact SXT/UXT checks above (0xFA0F/0xFA1F...). */
    if ((upper & 0xFF80) == 0xFA00 && (lower & 0xF0F0) == 0xF000) {
        int op = (upper >> 5) & 0x3;  /* 0=LSL 1=LSR 2=ASR 3=ROR */
        int S  = (upper >> 4) & 0x1;
        int Rn = upper & 0xF;
        int Rd = (lower >> 8) & 0xF;
        int Rm = lower & 0xF;
        if (Rd != 15) {
            int n = cpu.r[Rm] & 0xFF;
            int carry = 0;
            uint32_t res = t32_shift_c(cpu.r[Rn], op, n, &carry);
            cpu.r[Rd] = res;
            if (S) {
                update_nz_flags(res);
                if (carry) cpu.xpsr |= FLAG_C; else cpu.xpsr &= ~FLAG_C;
            }
        }
        return 1;
    }
    /* BLX register is 16-bit (handled elsewhere) */
    /* NOP/IT/YIELD etc. 32-bit hints: upper = 0xF3AF, lower = 0x8000..80xx */
    if ((upper & 0xFFFF) == 0xF3AF && (lower & 0xFF00) == 0x8000) {
        return 1;  /* NOP, YIELD, WFE, WFI, SEV hints */
    }

    (void)pc;
    return 0;
}

/* ========================================================================
 * Load/Store single (32-bit): T2/T3/T4 encodings
 * upper pattern analysis:
 *   bit[8]=1 (T3): unsigned imm12 offset
 *     0xF8C0|Rn: STR.W   0xF8D0|Rn: LDR.W
 *     0xF880|Rn: STRB.W  0xF890|Rn: LDRB.W
 *     0xF8A0|Rn: STRH.W  0xF8B0|Rn: LDRH.W (also LDR.W T3 with Rn=15 = PC-relative)
 *     0xF990|Rn: LDRSB.W 0xF9B0|Rn: LDRSH.W
 *   bit[8]=0 (T4/T2): signed imm8 pre/post or register offset
 * ======================================================================== */
static void t32_ldst_single(uint32_t pc, uint16_t upper, uint16_t lower) {
    int Rn   = upper & 0xF;
    int size = (upper >> 5) & 3;   /* 00=byte, 01=hword, 10=word */
    int sign = (upper >> 8) & 1;   /* for F9xx: signed extension */
    int L    = (upper >> 4) & 1;   /* 1=load, 0=store */
    int Rt   = (lower >> 12) & 0xF;

    /* Distinguish T3 (12-bit unsigned imm, upper[7]=1 for same-size group,
     * more precisely: for 0xF8xx upper[8]=1 means T3, for 0xF9xx always T1/T3) */
    /* Simplification: use upper[11:8] to determine form */
    int upper_hi = (upper >> 4) & 0xF;  /* bits[11:8] */

    if (upper_hi & 0x8) {
        /* T3: 12-bit unsigned offset. lower = Rt(4):imm12(12) */
        int imm12 = lower & 0xFFF;
        uint32_t base = (Rn == 15) ? ((pc + 4) & ~3u) : cpu.r[Rn];

        /* Decode by upper[7:4] */
        int bits74 = (upper >> 4) & 0xF;
        /* And sign extension bit from upper[8] or upper group: */
        /* For F8xx: no sign extension (unsigned) */
        /* For F9xx: sign extension */
        int is_signed = ((upper >> 8) & 0xF) == 9;  /* upper[11:8]=1001 = 0xF9 group */

        switch (bits74) {
        case 0x8: /* STRB */
            mem_write8(base + imm12, cpu.r[Rt] & 0xFF); break;
        case 0x9: /* LDRB */
            cpu.r[Rt] = is_signed ? (uint32_t)(int32_t)(int8_t)mem_read8(base + imm12)
                                  : (uint32_t)mem_read8(base + imm12);
            break;
        case 0xA: /* STRH */
            mem_write16(base + imm12, cpu.r[Rt] & 0xFFFF); break;
        case 0xB: /* LDRH */
            cpu.r[Rt] = is_signed ? (uint32_t)(int32_t)(int16_t)mem_read16(base + imm12)
                                  : (uint32_t)mem_read16(base + imm12);
            break;
        case 0xC: /* STR.W */
            mem_write32(base + imm12, cpu.r[Rt]); break;
        case 0xD: /* LDR.W */
            /* Loads to PC interwork (bit0 selects Thumb state); the M
             * profile stays in Thumb, so clear it (an unmasked odd PC
             * misaligns the next fetch and sends execution wild). */
            cpu.r[Rt] = (Rt == 15) ? (mem_read32(base + imm12) & ~1u)
                                   : mem_read32(base + imm12);
            if (Rt == 15) pc_updated = 1;
            break;
        default:
            goto unhandled_ldst;
        }
        return;
    }

    /* T4/T2: 8-bit signed offset with P/U/W, or register offset.
     * T4 is selected by lower bit11; additionally, Rn==15 with a zero
     * offset field (e.g. F85F F000 = ldr.w pc, [pc], a jump-table
     * veneer) is a PC-relative literal: route it to T4 so the base is
     * Align(PC,4), not PC+Rn-contents (which sent littleos_pico2 to
     * 0x5F200040). Genuine [PC,Rm] forms have nonzero offset bits and
     * stay on the T2 path. */
    if ((lower & 0x0800) || (Rn == 15 && (lower & 0x0FFF) == 0)) {
        /* T4: lower = Rt(4) : 1 : P : U : W : imm8 */
        int P    = (lower >> 10) & 1;
        int U    = (lower >> 9) & 1;
        int W    = (lower >> 8) & 1;
        int imm8 = lower & 0xFF;
        uint32_t base = (Rn == 15) ? ((pc + 4) & ~3u) : cpu.r[Rn];
        int32_t  off  = U ? (int32_t)imm8 : -(int32_t)imm8;
        uint32_t offset_addr = (uint32_t)((int32_t)base + off);
        uint32_t addr = P ? offset_addr : base;

        int bits74 = (upper >> 4) & 0xF;
        int is_signed = ((upper >> 8) & 0xF) == 9;
        switch (bits74) {
        case 0x8: case 0x0:
            if (L) cpu.r[Rt] = mem_read8(addr); else mem_write8(addr, cpu.r[Rt]&0xFF); break;
        case 0x9: case 0x1:
            cpu.r[Rt] = is_signed ? (uint32_t)(int32_t)(int8_t)mem_read8(addr)
                                  : (uint32_t)mem_read8(addr); break;
        case 0xA: case 0x2:
            if (L) cpu.r[Rt] = mem_read16(addr); else mem_write16(addr, cpu.r[Rt]&0xFFFF); break;
        case 0xB: case 0x3:
            cpu.r[Rt] = is_signed ? (uint32_t)(int32_t)(int16_t)mem_read16(addr)
                                  : (uint32_t)mem_read16(addr); break;
        case 0xC: case 0x4:
            if (L) cpu.r[Rt] = mem_read32(addr); else mem_write32(addr, cpu.r[Rt]); break;
        case 0xD: case 0x5:
            if (L) { cpu.r[Rt] = (Rt == 15) ? (mem_read32(addr) & ~1u)
                                            : mem_read32(addr);
                     if (Rt==15) pc_updated=1; }
            else   { mem_write32(addr, cpu.r[Rt]); }
            break;
        default: goto unhandled_ldst;
        }
        if (W) cpu.r[Rn] = offset_addr;
        return;
    }

    /* T2: register offset. lower = Rt(4) : 000000 : imm2(2) : Rm(4).
     * Rn==15 uses Align(PC,4) as base (same literal convention). */
    {
        int imm2 = (lower >> 4) & 3;
        int Rm   = lower & 0xF;
        uint32_t offset = cpu.r[Rm] << imm2;
        uint32_t addr   = ((Rn == 15) ? ((pc + 4) & ~3u) : cpu.r[Rn]) + offset;
        int bits74 = (upper >> 4) & 0xF;
        int is_signed = ((upper >> 8) & 0xF) == 9;

        switch (bits74) {
        case 0x8: case 0x0:
            if (L) cpu.r[Rt] = mem_read8(addr); else mem_write8(addr, cpu.r[Rt]&0xFF); break;
        case 0x9: case 0x1:
            cpu.r[Rt] = is_signed ? (uint32_t)(int32_t)(int8_t)mem_read8(addr)
                                  : (uint32_t)mem_read8(addr); break;
        case 0xA: case 0x2:
            if (L) cpu.r[Rt] = mem_read16(addr); else mem_write16(addr, cpu.r[Rt]&0xFFFF); break;
        case 0xB: case 0x3:
            cpu.r[Rt] = is_signed ? (uint32_t)(int32_t)(int16_t)mem_read16(addr)
                                  : (uint32_t)mem_read16(addr); break;
        case 0xC: case 0x4:
            if (L) cpu.r[Rt] = mem_read32(addr); else mem_write32(addr, cpu.r[Rt]); break;
        case 0xD: case 0x5:
            if (L) { cpu.r[Rt] = (Rt == 15) ? (mem_read32(addr) & ~1u)
                                            : mem_read32(addr);
                     if (Rt==15) pc_updated=1; }
            else   { mem_write32(addr, cpu.r[Rt]); }
            break;
        case 0x7: /* LDRSH.W T2 (signed halfword, register offset): upper[15:8]=0xF9, bits74=7 */
            /* 0xF97x Rn: LDRSH.W Rt, [Rn, Rm{, LSL #imm2}] */
            cpu.r[Rt] = (uint32_t)(int32_t)(int16_t)mem_read16(addr);
            break;
        default: goto unhandled_ldst;
        }
        return;
    }

unhandled_ldst:
    if (cpu.debug_enabled)
        fprintf(stderr, "[T32] Unhandled ldst upper=0x%04X lower=0x%04X @ PC=0x%08X (no-op)\n",
                upper, lower, pc);
    (void)sign; (void)size; (void)L;
}

/* ========================================================================
 * VFP single-precision (M33 FPU, fpv5-sp-d16)
 *
 * littleOS Pico 2 builds with -mfloat-abi=softfp: float arithmetic uses
 * VFP S-regs while doubles use softfloat helpers. The old blanket NOP
 * left every float computation on stale register garbage (supervisor
 * "Memory usage high: 1800%", "Temperature: 1800C" from %f prints).
 *
 * Register numbering (verified mechanically against arm-none-eabi-as):
 *   Sd[4:1] = lower[15:12], Sd[0] = upper[6]      (i.e. Sd = Vd:D)
 *   Sn[4:1] = upper[3:0],   Sn[0] = lower[7]      (i.e. Sn = Vn:N)
 *   Sm[4:1] = lower[3:0],   Sm[0] = lower[5]      (i.e. Sm = Vm:M)
 *   Dd/Dm (64-bit moves only) = lower[15:12] / lower[3:0]
 * S[d] layout: D[d] = S[2d] (low) | S[2d+1]<<32. Only FPSCR NZCV modeled.
 * 32-bit VFP insns honor IT predication via the generic cpu_step path.
 * NOTE: VFP state is not saved across dual-core context switches
 * (bind/unbind); all current workloads run VFP on one core only.
 * ======================================================================== */
static inline uint32_t vfp_s_get(int s) { return cpu.vfp_s[s & 31]; }
static inline void vfp_s_set(int s, uint32_t v) { cpu.vfp_s[s & 31] = v; }
static inline float vfp_f_get(int s) {
    float f; uint32_t v = vfp_s_get(s); memcpy(&f, &v, 4); return f;
}
static inline void vfp_f_set(int s, float f) {
    uint32_t v; memcpy(&v, &f, 4); vfp_s_set(s, v);
}
static inline uint64_t vfp_d_get(int d) {
    d &= 15;
    return (uint64_t)vfp_s_get(2 * d) | ((uint64_t)vfp_s_get(2 * d + 1) << 32);
}
static inline void vfp_d_set(int d, uint64_t v) {
    d &= 15;
    vfp_s_set(2 * d, (uint32_t)v);
    vfp_s_set(2 * d + 1, (uint32_t)(v >> 32));
}
static inline int vfp_Sd(uint16_t upper, uint16_t lower) {
    return (((lower >> 12) & 0xF) << 1) | ((upper >> 6) & 1);
}
static inline int vfp_Sn(uint16_t upper, uint16_t lower) {
    return ((upper & 0xF) << 1) | ((lower >> 7) & 1);
}
static inline int vfp_Sm(uint16_t upper, uint16_t lower) {
    (void)upper; return ((lower & 0xF) << 1) | ((lower >> 5) & 1);
}

/* VFPExpandImm for 32-bit VMOV immediate (imm8 = abcdefgh). */
static uint32_t vfp_expand_imm(uint8_t imm8) {
    uint32_t a = ((uint32_t)imm8 >> 7) & 1u;
    uint32_t b = ((uint32_t)imm8 >> 6) & 1u;
    uint32_t not_b = b ^ 1u;
    uint32_t exp = (not_b << 7) | (b << 6) | (b << 5) | (b << 4) |
                   (b << 3) | (b << 2) | (((uint32_t)imm8 >> 5) & 1u) << 1 |
                   (((uint32_t)imm8 >> 4) & 1u);
    uint32_t frac = ((uint32_t)imm8 & 0xFu) << 19;
    return (a << 31) | (exp << 23) | frac;
}

/* float -> int32, round toward zero, NaN -> 0, saturate (ARM VCVT rule). */
static int32_t vfp_f2i(float f) {
    if (isnan(f)) return 0;
    if (f >= 2147483648.0f) return INT32_MAX;
    if (f <= -2147483648.0f) return INT32_MIN;
    return (int32_t)f;
}

/* float -> uint32, round toward zero, NaN -> 0, saturate (ARM VCVT rule). */
static uint32_t vfp_f2u(float f) {
    if (isnan(f)) return 0;
    if (f <= 0.0f) return 0;
    if (f >= 4294967296.0f) return UINT32_MAX;
    if (f < 2147483648.0f) return (uint32_t)(int32_t)f;
    return (uint32_t)((int32_t)(f - 2147483648.0f)) + 0x80000000u;
}

/* double -> int32/uint32 for DCP RDIC/RDUC (round mode: 0 trunc, 1 near). */
static int32_t dcp_d2i(double f, int rnd) {
    double r = rnd ? nearbyint(f) : trunc(f);
    if (isnan(r)) return 0;
    if (r >= 2147483648.0) return INT32_MAX;
    if (r <= -2147483648.0) return INT32_MIN;
    return (int32_t)r;
}

static uint32_t dcp_d2u(double f, int rnd) {
    double r = rnd ? nearbyint(f) : trunc(f);
    if (isnan(r)) return 0;
    if (r <= 0.0) return 0;
    if (r >= 4294967296.0) return UINT32_MAX;
    if (r < 2147483648.0) return (uint32_t)(int32_t)r;
    return (uint32_t)((int32_t)(r - 2147483648.0)) + 0x80000000u;
}

/* VCMP(E).F32: NZCV into FPSCR (V always 0). ARM rule: C is set for
 * equal and unordered ONLY, not greater-than (unlike SUB borrow!).
 * GT must yield all-clear or BHI/BLS invert (littleOS shell crash).
 * APSR is untouched (a following VMRS makes flags visible). */
static void vfp_cmp(float a, float b) {
    uint32_t nzcv;
    if (isnan(a) || isnan(b))      nzcv = FLAG_C;
    else if (a == b)               nzcv = FLAG_Z | FLAG_C;
    else if (a < b)                nzcv = FLAG_N;
    else                           nzcv = 0;
    cpu.vfp_fpscr = nzcv;
}

/* ========================================================================
 * RP2350 DCP double-precision coprocessor (deferred-compute model)
 *
 * Firmware routes ALL double math through pico_double's DCP assembly
 * (dcp_*_m canned sequences: WX/WY operand writes, CDP micro-ops,
 * RD/RCMP result reads). The engine is multi-cycle stateful HW; we
 * model it functionally: snapshot operands on writes, compute exact
 * IEEE-754 results (host double) at terminal reads. Intermediate
 * micro-ops (mantissa shuffles, NRxx/NORM) and their integer fixups are
 * straight-line with no control flow on DCP state (verified against the
 * canned sequences), so ignoring them is behavior-preserving.
 * Engagement (PCMP) always reads not-engaged: ops complete instantly,
 * so save/restore (exact 64-bit round-trip via PXMD/PYMD/REFD and
 * WXMD/WYMD/WEFD) never triggers spuriously. Nested DCP use across an
 * interrupt would clobber state (same limitation as VFP dual-core).
 * Field layouts verified mechanically against arm-none-eabi-as:
 *   MCRR: hw1=0xEC40|Rt2, hw2=Rt<<12|0x400|opc1<<4|CRm
 *   MRRC: hw1=0xEC50|Rt2, hw2=Rt<<12|0x400|opc1<<4|CRm
 *   MRC:  hw1=0xEE10,     hw2=Rt<<12|0x400|(opc2<<5)|CRm
 *   CDP:  hw1=0xEE00|(opc1<<4), hw2=0x0400|(opc2<<5)|CRm (CRd=CRn=c0)
 *   MRC2: hw1=0xFE10,     hw2=Rt<<12|0x400|(opc2<<5)|CRm
 *   MRRC2: hw1=0xFC50|Rt2, hw2=Rt<<12|0x400|opc1<<4|CRm
 * MRC/CDP share EE10+0x04xx space, split by lower bit4 (MRC=1, CDP=0).
 * ======================================================================== */
static inline double dcp_as_double(uint64_t u) {
    double d; memcpy(&d, &u, 8); return d;
}
static inline uint64_t dcp_as_bits(double d) {
    uint64_t u; memcpy(&u, &d, 8); return u;
}
static inline float dcp_as_float(uint32_t u) {
    float f; memcpy(&f, &u, 4); return f;
}
static inline uint32_t dcp_float_bits(float f) {
    uint32_t u; memcpy(&u, &f, 4); return u;
}

/* DCP RCMP NZCV: SUB-like ordered flags; unordered sets V (not Z/C).
 * Verified against dcmplt/dcmpeq/dcmpun/ite-hi users: unordered must give
 * HI=0 (C=0,Z=0) yet V=1 for the bvs fixups and dcmpun's bit28 extract. */
static uint32_t dcp_cmp_flags(double a, double b) {
    if (isnan(a) || isnan(b)) return FLAG_V;
    if (a == b) return FLAG_Z | FLAG_C;
    if (a < b)  return FLAG_N;
    return FLAG_C;
}

/* Best-effort 64-bit mantissa state (hidden bit explicit) for RXMS-style
 * reads. Nothing branches on these in any canned sequence; exact layout
 * is unobservable behaviorally. */
static uint64_t dcp_mantissa(double d) {
    uint64_t u = dcp_as_bits(d);
    uint64_t exp = (u >> 52) & 0x7FFu;
    uint64_t man = u & 0xFFFFFFFFFFFFFULL;
    if (exp != 0 && exp != 0x7FFu) man |= 1ULL << 52;
    return man;
}

/* Execute a DCP instruction. Returns 1 if DCP-shaped (handled or explicit
 * NOP), 0 otherwise (caller falls through to VFP/misc/ldst). Covers the
 * MCRR/MRRC/CDP/MRC shapes; MRC2/MRRC2 handled by t32_dcp2. */
static int t32_dcp(uint32_t pc, uint16_t upper, uint16_t lower) {
    (void)pc;
    /* MCRR writes: hw1=0xEC40|Rt2, hw2=Rt<<12|0x400|opc1<<4|CRm */
    if ((upper & 0xFFF0) == 0xEC40 && (lower & 0x0F00) == 0x0400) {
        int Rt = (lower >> 12) & 0xF, Rt2 = upper & 0xF;
        int opc1 = (lower >> 4) & 0xF, crm = lower & 0xF;
        uint64_t v = (uint64_t)cpu.r[Rt] | ((uint64_t)cpu.r[Rt2] << 32);
        switch (opc1) {
        case 0: /* WXMD/WYMD/WEFD: exact state write (save/restore) */
            if (crm == 0) cpu.dcp_x = v;
            else if (crm == 1) cpu.dcp_y = v;
            else if (crm == 2) cpu.dcp_ef = v;
            else return 0;
            return 1;
        case 1: /* WXUP/WYUP/WXYU */
            if (crm == 0) { cpu.dcp_x = v; cpu.dcp_from_int = 0; }
            else if (crm == 1) { cpu.dcp_y = v; cpu.dcp_from_int = 0; }
            else if (crm == 2) {
                cpu.dcp_x = dcp_as_bits((double)dcp_as_float((uint32_t)v));
                cpu.dcp_y = dcp_as_bits((double)dcp_as_float((uint32_t)(v >> 32)));
                cpu.dcp_from_int = 0;
            } else return 0;
            return 1;
        case 2: /* WXMS */
        case 3: /* WXMO */
        case 4: /* WXDD */
        case 5: /* WXDQ */
            if (crm != 0) return 0;
            return 1; /* intermediate state: ignored (see header) */
        case 6: /* WXUC */
            if (crm != 0) return 0;
            cpu.dcp_x = dcp_as_bits((double)(uint32_t)cpu.r[Rt]);
            cpu.dcp_from_int = 1;
            return 1;
        case 7: /* WXIC */
            if (crm != 0) return 0;
            cpu.dcp_x = dcp_as_bits((double)(int32_t)cpu.r[Rt]);
            cpu.dcp_from_int = 1;
            return 1;
        case 8: /* WXDC */
            if (crm != 0) return 0;
            cpu.dcp_x = v;
            cpu.dcp_from_int = 0;
            return 1;
        case 9: /* WXFC */
            if (crm != 2) return 0;
            cpu.dcp_x = dcp_as_bits((double)dcp_as_float((uint32_t)v));
            cpu.dcp_from_int = 0;
            return 1;
        case 10: /* WXFM */
        case 11: /* WXFD */
        case 12: /* WXFQ */
            if (crm != 0 && crm != 2) return 0;
            return 1; /* float-fma paths: unused by linked code */
        default:
            return 0;
        }
    }
    /* MRRC reads: hw1=0xEC50|Rt2, hw2=Rt<<12|0x400|opc1<<4|CRm */
    if ((upper & 0xFFF0) == 0xEC50 && (lower & 0x0F00) == 0x0400) {
        int Rt = (lower >> 12) & 0xF, Rt2 = upper & 0xF;
        int opc1 = (lower >> 4) & 0xF, crm = lower & 0xF;
        double X = dcp_as_double(cpu.dcp_x), Y = dcp_as_double(cpu.dcp_y);
        if (opc1 == 0) {
            uint64_t v = 0;
            if (crm == 8) v = cpu.dcp_x;
            else if (crm == 9) v = cpu.dcp_y;
            else if (crm == 10) v = cpu.dcp_ef;
            else return 0;
            if (Rt != 15) cpu.r[Rt] = (uint32_t)v;
            if (Rt2 != 15) cpu.r[Rt2] = (uint32_t)(v >> 32);
            return 1;
        }
        if (crm == 4 || crm == 5) {
            /* RXMS/RYMS: mantissa state (best-effort; no control flow
             * depends on it in any canned sequence). */
            uint64_t m = dcp_mantissa(crm == 4 ? X : Y);
            if (Rt != 15) cpu.r[Rt] = (uint32_t)m;
            if (Rt2 != 15) cpu.r[Rt2] = (uint32_t)(m >> 32);
            return 1;
        }
        if (crm == 1) {
            /* RXYH/RYMR/RXMQ halves (best-effort, see above). */
            uint32_t lo = 0, hi = 0;
            if (opc1 == 1) { lo = (uint32_t)(cpu.dcp_x >> 32); hi = (uint32_t)(cpu.dcp_y >> 32); }
            else if (opc1 == 2) { lo = (uint32_t)(cpu.dcp_y >> 32); hi = (uint32_t)cpu.dcp_y; }
            else if (opc1 == 4) { lo = (uint32_t)(cpu.dcp_x >> 32); hi = (uint32_t)cpu.dcp_x; }
            else return 0;
            if (Rt != 15) cpu.r[Rt] = lo;
            if (Rt2 != 15) cpu.r[Rt2] = hi;
            return 1;
        }
        if (crm == 0) {
            /* RDDA/RDDS/RDDM/RDDD/RDDQ/RDDG result reads. */
            double r = 0;
            int ok = 1;
            switch (opc1) {
            case 1: r = X + Y; break;
            case 3: r = cpu.dcp_from_int ? X : X - Y; break;
            case 5: r = X * Y; break;
            case 7: r = X / Y; break;
            case 9: r = sqrt(X); break;
            case 11: r = X; break;
            default: ok = 0; break;
            }
            if (!ok) return 0;
            uint64_t v = dcp_as_bits(r);
            if (Rt != 15) cpu.r[Rt] = (uint32_t)v;
            if (Rt2 != 15) cpu.r[Rt2] = (uint32_t)(v >> 32);
            return 1;
        }
        return 0;
    }
    /* MRC reads: upper==0xEE10, hw2=Rt<<12|0x400|(opc2<<5)|CRm */
    if (upper == 0xEE10 && (lower & 0x0F00) == 0x0400) {
        int Rt = (lower >> 12) & 0xF;
        int opc2 = (lower >> 5) & 7, crm = lower & 0xF;
        double X = dcp_as_double(cpu.dcp_x);
        if (crm == 0 && opc2 == 0) { /* RXVD: always ready */
            if (Rt != 15) cpu.r[Rt] = 1;
            return 1;
        }
        if (crm == 0 && opc2 == 1) { /* RCMP */
            uint32_t fl = dcp_cmp_flags(X, dcp_as_double(cpu.dcp_y));
            if (Rt == 15) cpu.xpsr = (cpu.xpsr & ~0xF0000000u) | fl;
            else cpu.r[Rt] = fl;
            return 1;
        }
        if (crm == 2 && opc2 <= 5) { /* RDFA..RDFG: float result */
            if (Rt != 15) cpu.r[Rt] = dcp_float_bits((float)X);
            return 1;
        }
        if (crm == 3 && opc2 <= 1) { /* RDIC/RDUC */
            uint32_t iv = opc2 ? dcp_d2u(X, cpu.dcp_rmode) : (uint32_t)dcp_d2i(X, cpu.dcp_rmode);
            if (Rt != 15) cpu.r[Rt] = iv;
            return 1;
        }
        return 0;
    }
    /* CDP ops: hw1=0xEE00|(opc1<<4), hw2=0x0400|(opc2<<5)|CRm */
    if ((upper & 0xFF00) == 0xEE00 && (lower & 0x0F00) == 0x0400) {
        int opc1 = (upper >> 4) & 0xF;
        int opc2 = (lower >> 5) & 7, crm = lower & 0xF;
        if (opc1 == 8 && crm == 0 && opc2 == 2) cpu.dcp_rmode = 0; /* NTDC */
        else if (opc1 == 8 && crm == 0 && opc2 == 3) cpu.dcp_rmode = 1; /* NRDC */
        /* ADD0/ADD1/SUB1/SQR0/NORM/NRDF/NRDD/NTDC/NRDC/INIT: no-ops here;
         * operands already snapshotted, results computed at reads. */
        return 1;
    }
    return 0;
}

/* DCP group 0x1F (MRC2/MRRC2). Returns 1 if handled, 0 otherwise. */
static int t32_dcp2(uint32_t pc, uint16_t upper, uint16_t lower) {
    (void)pc;
    /* MRC2: upper==0xFE10, hw2=Rt<<12|0x400|(opc2<<5)|CRm */
    if (upper == 0xFE10 && (lower & 0x0F00) == 0x0400) {
        int Rt = (lower >> 12) & 0xF;
        int opc2 = (lower >> 5) & 7, crm = lower & 0xF;
        if (crm == 0 && opc2 == 1) { /* PCMP: never engaged */
            if (Rt == 15) cpu.xpsr &= ~FLAG_N;
            else cpu.r[Rt] = 0;
            return 1;
        }
        return 0;
    }
    /* MRRC2: hw1=0xFC50|Rt2, hw2=Rt<<12|0x400|opc1<<4|CRm */
    if ((upper & 0xFFF0) == 0xFC50 && (lower & 0x0F00) == 0x0400) {
        int Rt = (lower >> 12) & 0xF, Rt2 = upper & 0xF;
        int opc1 = (lower >> 4) & 0xF, crm = lower & 0xF;
        uint64_t v = 0;
        if (opc1 == 0 && crm == 8) v = cpu.dcp_x;         /* PXMD */
        else if (opc1 == 0 && crm == 9) v = cpu.dcp_y;    /* PYMD */
        else if (opc1 == 0 && crm == 10) v = cpu.dcp_ef;  /* PEFD */
        else return 0;
        if (Rt != 15) cpu.r[Rt] = (uint32_t)v;
        if (Rt2 != 15) cpu.r[Rt2] = (uint32_t)(v >> 32);
        return 1;
    }
    return 0;
}

/* Execute a VFP instruction in group 0x1D (EC/ED/EE/EF uppers).
 * Returns 1 if the shape is VFP (handled or explicit NOP), 0 otherwise
 * (caller falls through to TT/exclusives/LDRD/etc.). */
static int t32_vfp(uint32_t pc, uint16_t upper, uint16_t lower) {
    int sub = (lower >> 8) & 0xF;
    int is_vfp_ls = (sub == 0xA || sub == 0xB) &&
        ((upper & 0x0F00) == 0x0C00 || (upper & 0x0F00) == 0x0D00);
    /* EC pair-move VMOV FIRST: it overlaps EC+101x multi shapes, but its
     * U[7:4] of 0100/0101 never collides with multi's 1001/1010/1011/001x.
     * Direction lives in U[4] (EC55: Rt,Rt2 <- Dm; EC45: Dm <- Rt,Rt2),
     * so match (upper&0x0FE0)==0x0C40 to cover both (0x0C50 missed EC45
     * and silently dropped `vmov Dm, Rt, Rt2`). */
    if ((upper & 0x0FE0) == 0x0C40 && (lower & 0x0FF0) == 0x0B10) {
        int dir = (upper >> 4) & 1; /* 1: Rt,Rt2 <- Dm, 0: Dm <- Rt,Rt2 */
        int Rt = (lower >> 12) & 0xF, Rt2 = upper & 0xF, Dm = lower & 0xF;
        if (dir) {
            uint64_t v = vfp_d_get(Dm);
            if (Rt != 15) cpu.r[Rt] = (uint32_t)v;
            if (Rt2 != 15) cpu.r[Rt2] = (uint32_t)(v >> 32);
        } else {
            vfp_d_set(Dm, (uint64_t)cpu.r[Rt] | ((uint64_t)cpu.r[Rt2] << 32));
        }
        return 1;
    }
    /* VLDM/VSTM multi (incl. VPUSH/VPOP): EC+101x always multi;
     * ED+101x with W==1 is STMDB-form multi. (ED+101x+W==0 is single,
     * handled below.) */
    if (is_vfp_ls &&
        (((upper & 0x0F00) == 0x0C00) ||
         (((upper >> 5) & 1) && ((upper & 0x0F00) == 0x0D00)))) {
        int U = (upper >> 7) & 1, P = (upper >> 8) & 1;
        int Ld = (upper >> 4) & 1, Rn = upper & 0xF;
        int is64 = (sub == 0xB);
        int start_s = is64 ? (((lower >> 12) & 0xF) * 2)
                           : ((((lower >> 12) & 0xF) << 1) | ((upper >> 6) & 1));
        int count = lower & 0xFF; /* words */
        if (count == 0) return 1;
        uint32_t base = cpu.r[Rn];
        uint32_t start;
        if (!P && U)       start = base;                    /* IA */
        else if (P && U)   start = base + 4;                /* IB */
        else if (!P && !U) start = base - 4u * (uint32_t)count + 4; /* DA */
        else               start = base - 4u * (uint32_t)count;     /* DB */
        for (int i = 0; i < count; i++) {
            uint32_t a = start + 4u * (uint32_t)i;
            if (Ld) vfp_s_set(start_s + i, mem_read32(a));
            else    mem_write32(a, vfp_s_get(start_s + i));
        }
        if (Rn != 15)
            cpu.r[Rn] = U ? base + 4u * (uint32_t)count
                          : base - 4u * (uint32_t)count;
        return 1;
    }
    /* ED single (W==0): VLDR/VSTR */
    if ((upper & 0x0F00) == 0x0D00) {
        if (sub != 0xA && sub != 0xB) return 0;
        int is64 = (sub == 0xB);
        int U = (upper >> 7) & 1, W = (upper >> 5) & 1;
        int Ld = (upper >> 4) & 1, Rn = upper & 0xF;
        int P = (upper >> 8) & 1;
        int32_t off = (int32_t)(lower & 0xFF) * 4;
        if (!U) off = -off;
        uint32_t base = (Rn == 15) ? ((pc + 4) & ~3u) : cpu.r[Rn];
        uint32_t addr = P ? (uint32_t)((int32_t)base + off) : base;
        if (is64) {
            int Dd = (lower >> 12) & 0xF;
            if (Ld) {
                uint32_t lo = mem_read32(addr);
                uint32_t hi = mem_read32(addr + 4);
                vfp_d_set(Dd, (uint64_t)lo | ((uint64_t)hi << 32));
            } else {
                uint64_t v = vfp_d_get(Dd);
                mem_write32(addr, (uint32_t)v);
                mem_write32(addr + 4, (uint32_t)(v >> 32));
            }
        } else {
            int Sd = vfp_Sd(upper, lower);
            if (Ld) vfp_s_set(Sd, mem_read32(addr));
            else    mem_write32(addr, vfp_s_get(Sd));
        }
        if (W && Rn != 15)
            cpu.r[Rn] = P ? addr : (uint32_t)((int32_t)base + off);
        return 1;
    }

    /* EE group */
    if ((upper & 0x0F00) != 0x0E00) return 0;
    {
        int u74 = (upper >> 4) & 0xF;   /* includes D bit */
        int Sd = vfp_Sd(upper, lower);

        /* VMOV.F32 immediate: U[7:4]==1011 (D-masked), L[7:4]==0000 */
        if ((u74 & ~0x4) == 0xB && ((lower >> 4) & 0xF) == 0x0 &&
            ((lower >> 8) & 0xF) == 0xA) {
            uint8_t imm8 = (uint8_t)(((upper & 0xF) << 4) | (lower & 0xF));
            vfp_s_set(Sd, vfp_expand_imm(imm8));
            return 1;
        }

        /* VMOV between GP reg and S reg: U[7:4] 0000/0001, L[11:8]==1010 */
        if (u74 <= 1 && ((lower >> 8) & 0xF) == 0xA) {
            int dir = (upper >> 4) & 1; /* 1: Rt <- Sn, 0: Sn <- Rt */
            int Rt = (lower >> 12) & 0xF, Sn = vfp_Sn(upper, lower);
            if (dir) { if (Rt != 15) cpu.r[Rt] = vfp_s_get(Sn); }
            else     { vfp_s_set(Sn, cpu.r[Rt]); }
            return 1;
        }

        /* 3-operand data processing: opc = {U[7],U[5],U[4]}, N = L[6].
         * (The old (u74&~4)==3 gate only matched VADD/VSUB and silently
         * NOP'd VMUL/VDIV/VMLA/VFMA.) */
        {
            int opc3 = (((upper >> 7) & 1) << 2) | ((upper >> 4) & 3);
            if (((lower >> 8) & 0xF) == 0xA && (opc3 <= 4 || opc3 == 6)) {
            int opc = opc3;
            int N = (lower >> 6) & 1;
            int Sn = vfp_Sn(upper, lower), Sm = vfp_Sm(upper, lower);
            float n = vfp_f_get(Sn), m = vfp_f_get(Sm), d = vfp_f_get(Sd), r = 0;
            switch (opc) {
            case 0: r = N ? d - n * m : d + n * m; break;          /* VMLS/VMLA */
            case 1: r = N ? -(d - n * m) : -(d + n * m); break;    /* VNMLS/VNMLA */
            case 2: r = n * m; if (N) r = -r; break;               /* VMUL/VNMUL */
            case 3: r = N ? n - m : n + m; break;                  /* VSUB/VADD */
            case 4: r = n / m; break;                              /* VDIV */
            case 6: { /* VFMA/VFMS: fused (single rounding via double) */
                double dd = (double)d + (N ? -1.0 : 1.0) * (double)n * (double)m;
                r = (float)dd;
                break;
            }
            default:
                if (cpu.debug_enabled)
                    fprintf(stderr, "[T32] VFP unhandled 3-op opc=%d @ PC=0x%08X\n",
                            opc, pc);
                return 1;
            }
            vfp_f_set(Sd, r);
            return 1;
            }
        }

        /* VMRS/VMSR: U = EEF1/EEE1, L[11:0] == 0xA10. Checked BEFORE the
         * 1-register group below: EEF1 FA10 also matches that group's
         * gate and would execute as VNEG (vmrs never ran, so VCMP-set
         * flags never reached APSR). Exact match collides with nothing:
         * 1-reg ops use L[7:0] 0x41/0x40/0xC0/0xC1/0xE7/0xC7/0x67/0x47. */
        if (((upper == 0xEEF1) || (upper == 0xEEE1)) &&
            (lower & 0x0FFF) == 0x0A10) {
            int Rt = (lower >> 12) & 0xF;
            if (upper == 0xEEF1) { /* VMRS */
                if (Rt == 15) cpu.xpsr = (cpu.xpsr & ~0xF0000000u) |
                                                 (cpu.vfp_fpscr & 0xF0000000u);
                else cpu.r[Rt] = cpu.vfp_fpscr;
            } else {               /* VMSR */
                cpu.vfp_fpscr = (cpu.vfp_fpscr & ~0xF8000000u) |
                                (cpu.r[Rt] & 0xF8000000u);
            }
            return 1;
        }

        /* 1-register ops + VCMP + VCVT-int: U[7:4] 1011/1111 (D-masked) */
        if ((u74 & ~0x4) == 0xB && ((lower >> 8) & 0xF) == 0xA) {
            /* VCVT between float and int: U[3] == 1 */
            if (upper & 0x0008) {
                int to_fp = ((upper >> 2) & 1) == 0; /* U[2]: 0 int->fp */
                if (to_fp) {
                    int sgned = (lower >> 7) & 1;    /* L[7]: 1 signed */
                    int Sm = vfp_Sm(upper, lower);
                    uint32_t iv = vfp_s_get(Sm);     /* integer bit pattern */
                    vfp_f_set(Sd, sgned ? (float)(int32_t)iv : (float)iv);
                } else {
                    int sgned = upper & 1;           /* U[0]: 1 signed */
                    int Sm = vfp_Sm(upper, lower);
                    float f = vfp_f_get(Sm);
                    uint32_t iv = sgned ? (uint32_t)vfp_f2i(f) : vfp_f2u(f);
                    vfp_s_set(Sd, iv);
                }
                return 1;
            }
            /* VCMP(E): U[2] == 1, E = L[7] */
            if (upper & 0x0004) {
                int Sm = vfp_Sm(upper, lower);
                vfp_cmp(vfp_f_get(Sd), vfp_f_get(Sm));
                return 1;
            }
            /* MOV/NEG/ABS/SQRT by {U[0], L[7]} */
            {
                int Sm = vfp_Sm(upper, lower);
                float m = vfp_f_get(Sm), r = m;
                int sel = (((upper & 1) << 1) | ((lower >> 7) & 1));
                if (sel == 2) r = -m;                /* VNEG */
                else if (sel == 1) r = fabsf(m);     /* VABS */
                else if (sel == 3) r = sqrtf(m);     /* VSQRT */
                vfp_f_set(Sd, r);
                return 1;
            }
        }
    }
    return 0;
}

/* VSEL (group 0x1F, FE uppers): cond2 = U[5:4] (00 EQ, 01 VS, 10 GE, 11 GT).
 * Sd = cond ? Sn : Sm. Returns 1 if VSEL-shaped, 0 otherwise. */
static int t32_vsel(uint32_t pc, uint16_t upper, uint16_t lower) {
    (void)pc;
    if ((upper & 0xFF00) != 0xFE00) return 0;
    if (((lower >> 8) & 0xF) != 0xA) return 0;
    int c2 = (upper >> 4) & 3;
    int cond = (c2 == 0) ? 0x0 : (c2 == 1) ? 0x6 : (c2 == 2) ? 0xA : 0xC;
    int Sd = vfp_Sd(upper, lower);
    int Sn = vfp_Sn(upper, lower);
    int Sm = vfp_Sm(upper, lower);
    vfp_s_set(Sd, t32_check_cond((uint8_t)cond) ? vfp_s_get(Sn) : vfp_s_get(Sm));
    return 1;
}

/* ========================================================================
 * Main 32-bit Thumb-2 dispatcher
 * Called with current pc (address of upper halfword), upper and lower halfwords.
 * Returns 1 if instruction was handled, 0 if unknown (caller triggers HardFault).
 * ======================================================================== */
int thumb32_step(uint32_t pc, uint16_t upper, uint16_t lower) {
    /* Update PC to skip this 32-bit instruction (caller may override) */
    cpu.r[15] = pc + 4;
    pc_updated = 1;

    uint8_t top5 = upper >> 11;  /* bits[15:11] of upper */

    /* ------------------------------------------------------------------ */
    /* Group 11101: E8xx-EFxx                                              */
    /* ------------------------------------------------------------------ */
    if (top5 == 0x1D) {
        /* DCP double-coprocessorbundle first (MCRR/MRRC/CDP/MRC shapes are
         * disjoint from VFP's by lower[11:8]: 0x4 vs 0xA/B/F). */
        if (t32_dcp(pc, upper, lower)) return 1;
        /* VFP single-precision (M33 FPU): real execution via t32_vfp;
         * unrecognized coprocessor shapes stay NOP (previous behavior:
         * the blanket skip below also covered MVE etc.). Without the
         * blanket, VFP fell into shifted-register DP and corrupted
         * registers (littleos_pico2: r4 clobbered across runtime_init). */
        if ((upper & 0xFC00) == 0xEC00) {
            t32_vfp(pc, upper, lower);
            return 1;
        }
        /* ARMv8-M TT (Test Target, TrustZone): upper = 0xE840|Rn,
         * lower = 0xF2Rd0 (e.g. E842 F200 = tt r2, r2, used by the
         * RP2350 SDK ROM table trampoline). All emulated memory is
         * Secure/privileged with no MPU, so the response is 0
         * (secure) — must be handled here, not as shifted-register DP. */
        if ((upper & 0xFFF0) == 0xE840 && (lower & 0xFF0F) == 0xF200) {
            int tt_rd = (lower >> 8) & 0xF;
            if (tt_rd != 15) cpu.r[tt_rd] = 0;
            return 1;
        }
        /* ARMv8-M load-acquire/store-release exclusive (M33 only; the SDK
         * uses LDAEXB/STREXB for spinlocks/mutexes). Misdecoding these as
         * LDRD/STRD or shifted-register DP clobbered PC (Rd=15 phantom)
         * and hung littleos_pico2 in a ROM walk. Single-core simplification:
         * LDAEXx = plain load, STREXx = plain store reporting success (0).
         * upper = 0xE8C0|Rn (STREX) or 0xE8D0|Rn (LDAEX);
         * lower = Rt:1111:01sz:Rd for STREX, Rt:1111:11sz:1111 for LDAEX
         * (sz: 00=byte 01=half 10=word). */
        if (((upper & 0xFFF0) == 0xE8C0 || (upper & 0xFFF0) == 0xE8D0) &&
            (lower & 0x0F00) == 0x0F00 &&
            (((upper & 0x0010) != 0) == ((lower & 0x0080) != 0))) {
            int is_load = (upper & 0x0010) != 0;
            int sz = (lower >> 4) & 0x3;
            int Rn = upper & 0xF;
            int Rt = (lower >> 12) & 0xF;
            int Rd = lower & 0xF;
            uint32_t addr = cpu.r[Rn];
            if (is_load) {
                uint32_t v = 0;
                if (sz == 0) v = mem_read8(addr);
                else if (sz == 1) v = mem_read16(addr);
                else if (sz == 2) v = mem_read32(addr);
                if (Rt != 15) cpu.r[Rt] = v;
                arm_excl_set(get_active_core(), addr);
            } else {
                int ok = arm_excl_store(get_active_core(), addr);
                if (ok) {
                    if (sz == 0) mem_write8(addr, cpu.r[Rt] & 0xFF);
                    else if (sz == 1) mem_write16(addr, cpu.r[Rt] & 0xFFFF);
                    else if (sz == 2) mem_write32(addr, cpu.r[Rt]);
                }
                if (Rd != 15) cpu.r[Rd] = ok ? 0 : 1;
            }
            return 1;
        }
        /* Load-acquire / store-release byte/halfword/word (M33): STLB,
         * STLH, LDAB, LDAH (and LDAPR word form). Upper = 0xE8C0|Rn
         * (store) or 0xE8D0|Rn (load); lower = (Rt<<12)|0xF8F (byte),
         * 0xF9F (half) or 0xFAF (word). Without this, STLB fell into
         * LDRD/STRD decoding and sprayed PC+4 across RAM as a
         * double-word store (littleos_pico2 spinlock-array corruption).
         * Barriers are NOPs; single-core-correct plain accesses. */
        if (((upper & 0xFFF0) == 0xE8C0 || (upper & 0xFFF0) == 0xE8D0) &&
            (lower & 0x0F00) == 0x0F00 && (lower & 0x000F) == 0x000F) {
            int sub = (lower >> 4) & 0xF;
            if (sub >= 0x8 && sub <= 0xA) {
                int is_ldab = (upper & 0x0010) != 0;
                int sz2 = sub - 0x8;
                int Rn2 = upper & 0xF;
                int Rt2 = (lower >> 12) & 0xF;
                uint32_t addr2 = cpu.r[Rn2];
                if (is_ldab) {
                    uint32_t v = 0;
                    if (sz2 == 0) v = mem_read8(addr2);
                    else if (sz2 == 1) v = mem_read16(addr2);
                    else v = mem_read32(addr2);
                    if (Rt2 != 15) cpu.r[Rt2] = v;
                } else {
                    if (sz2 == 0) mem_write8(addr2, cpu.r[Rt2] & 0xFF);
                    else if (sz2 == 1) mem_write16(addr2, cpu.r[Rt2] & 0xFFFF);
                    else mem_write32(addr2, cpu.r[Rt2]);
                }
                return 1;
            }
        }
        /* Table branch TBB/TBH: upper = 0xE8D0|Rn, lower = 0xF00H|Rm
         * (H=0 TBB, H=1 TBH; E8DF F001 / E8DF F011 verified against as).
         * This must precede the LDRD/STRD dispatch below: TBB/TBH uppers
         * have bit6 set, so the old bit6 routing sent every table-branch
         * to LDRD/STRD (littleos_pico2 switch jumped to 0x002C0149).
         * Lower top nibble 0xF is unique here (LDRD/STRD never use Rt=15;
         * LDREX/STREX live at 0xE84x/0xE85x, handled next). */
        if ((upper & 0xFFF0) == 0xE8D0 && (lower & 0xF000) == 0xF000) {
            t32_tbb_tbh(pc, upper, lower);
            return 1;
        }
        /* LDREX (word): upper = 0xE850|Rn, lower = (Rt<<12)|0xF00
         * (E853 2000 = ldrex r2, [r3]). Sets the exclusive reservation;
         * Rt==15 is UNPREDICTABLE and excluded (that shape belongs to LDMIA). */
        if ((upper & 0xFFF0) == 0xE850 && (lower & 0xF000) != 0xF000 &&
            (lower & 0x0FFF) == 0x0F00) {
            int ldx_rt = (lower >> 12) & 0xF;
            uint32_t ldx_addr = cpu.r[upper & 0xF];
            if (ldx_rt != 15) cpu.r[ldx_rt] = mem_read32(ldx_addr);
            arm_excl_set(get_active_core(), ldx_addr);
            return 1;
        }
        /* STREX (word): upper = 0xE840|Rn, lower = (Rt<<12)|(Rd<<8)
         * (E846 5400 = strex r4, r5, [r6]). Succeeds only with a live
         * reservation; otherwise reports failure (Rd=1) without storing.
         * Guards exclude TT (lower 0xF2xx, also checked first above) and
         * UNPREDICTABLE Rd==15 shapes. */
        if ((upper & 0xFFF0) == 0xE840 && (lower & 0xF000) != 0xF000 &&
            (lower & 0x00FF) == 0x0000 && ((lower >> 8) & 0xF) != 0xF) {
            int st_rt = (lower >> 12) & 0xF;
            int st_rd = (lower >> 8) & 0xF;
            uint32_t st_addr = cpu.r[upper & 0xF];
            int st_ok = arm_excl_store(get_active_core(), st_addr);
            if (st_ok) mem_write32(st_addr, cpu.r[st_rt]);
            if (st_rd != 15) cpu.r[st_rd] = st_ok ? 0 : 1;
            return 1;
        }
        uint8_t bits_10_9 = (upper >> 9) & 3;
        if (bits_10_9 == 0) {
            /* Load/Store Multiple or Double */
            /* Distinguish: LDRD/STRD (bit[6]=1 in this subgroup) vs LDM/STM */
            /* For LDRD/STRD T1: upper[6]=1 (but let's use upper bits more carefully) */
            /* upper[9:8]: 00=STMIA/LDMIA T2, 01=LDM/STM again, 10=? */
            /* Simpler: upper[8]=is_DB, upper[7]=L */
            /* Check for LDRD/STRD: upper[6]=1 and upper[7:5] pattern */
            if (upper & 0x0040) {
                /* LDRD/STRD: upper[6]=1 (TBB/TBH/LDREX/STREX are decoded
                 * above and never reach here) */
                t32_ldrd_strd(pc, upper, lower);
            } else {
                /* LDM/STM T2 */
                t32_ldst_multiple(pc, upper, lower);
            }
            return 1;
        }
        if (bits_10_9 == 1) {
            /* 0xEA/0xEB (bit10=0, bit9=1): data-processing with
             * shifted register (AND.W/ORR.W/BIC.W/ADD.W/...). Must NOT
             * be decoded as LDRD/STRD (that misdecode made firmware
             * BICS.W loops spin forever: LDRD never sets flags). */
            t32_dp_shifted_reg(pc, upper, lower);
            return 1;
        }
        /* bits_10_9 == 2 or 3: Data processing shifted register */
        t32_dp_shifted_reg(pc, upper, lower);
        return 1;
    }

    /* ------------------------------------------------------------------ */
    /* Group 11110: F0xx-F7xx                                              */
    /* ------------------------------------------------------------------ */
    if (top5 == 0x1E) {
        /* Check for VFP/NEON instructions (M33 FPU) */
        if ((upper & 0xEF00) == 0xEE00 || (upper & 0xEF00) == 0xED00) {
            /* VFP/NEON stubs: skip and return 1 to avoid HardFault */
            if (cpu.debug_enabled)
                fprintf(stderr, "[T32] FPU instruction stub PC=0x%08X upper=0x%04X lower=0x%04X\n", pc, upper, lower);
            return 1;
        }

        /* Check MSR/MRS/barriers first: they have lower bit[15]=1 and would
         * otherwise be misidentified as branches (e.g. MSR F380 8808). */
        if (t32_misc(pc, upper, lower)) return 1;
        /* BL T1: lower bits[15,14,12] = 1,1,1 */
        if ((lower & 0xD000) == 0xD000) {
            t32_bl(pc, upper, lower);
            return 1;
        }
        /* Branch (wide): lower bit[15]=1 */
        if (lower & 0x8000) {
            t32_branch(pc, upper, lower);
            return 1;
        }
        /* Data processing (lower bit[15]=0) */
        if (upper & 0x0200) {
            /* Plain binary immediate (bit9=1): MOVW, MOVT, ADDW, SUBW */
            t32_dp_plain_imm(pc, upper, lower);
        } else {
            /* Modified immediate (bit9=0) */
            t32_dp_mod_imm(pc, upper, lower);
        }
        return 1;
    }

    /* ------------------------------------------------------------------ */
    /* Group 11111: F8xx-FFxx                                              */
    /* ------------------------------------------------------------------ */
    if (top5 == 0x1F) {
        /* DCP MRC2/MRRC2 first (FE10/FC50 shapes are disjoint from VSEL's
         * FE+0x0Axx and from misc/ldst by full-pattern match). */
        if (t32_dcp2(pc, upper, lower)) return 1;
        /* VSEL.F32 (M33 FPU, FE uppers): select on APSR flags. Must
         * precede misc/ldst: as LDR.W it would corrupt memory. */
        if (t32_vsel(pc, upper, lower)) return 1;
        /* Check misc 32-bit first (SDIV, UDIV, MUL, CLZ etc.) */
        if (t32_misc(pc, upper, lower)) return 1;
        /* Load/Store single */
        t32_ldst_single(pc, upper, lower);
        return 1;
    }

    return 0;  /* Unknown */
}
