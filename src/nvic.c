#include <stdio.h>
#include <string.h>
#include "nvic.h"
#include "emulator.h"
#include "corepool.h"
#include "devtools.h"

/* CPUID value (M0+ default, M33 overlay changes this) */
uint32_t nvic_cpuid_value = 0x410CC601;  /* Cortex-M0+ default */

/* Per-core NVIC state (RP2040 has independent NVIC per core) */
nvic_state_t nvic_states[2] = {{0}};

/* Per-core SysTick state */
systick_state_t systick_states[2] = {{0}};

/* Track last IRQ signal for duplicate detection */
static uint32_t last_irq_signal = 0xFFFFFFFF;
static uint32_t irq_signal_count = 0;

/* Helper: get current core's NVIC state */
static inline nvic_state_t *nvic_cur(void) {
    return &nvic_states[get_active_core()];
}

/* Helper: get current core's SysTick state */
static inline systick_state_t *systick_cur(void) {
    return &systick_states[get_active_core()];
}

/* Initialize NVIC */
void nvic_init(void) {
    nvic_reset();
    systick_reset();
}

/* M33 MPU + SAU state (single instance: the emulator runs one world;
 * per-core banking would only matter for TrustZone-aware RTOS ports). */
mpu_state_t mpu_state;
sau_state_t sau_state;

/* Reset NVIC to power-on defaults (both cores) */
void nvic_reset(void) {
    for (int c = 0; c < 2; c++) {
        memset(&nvic_states[c], 0, sizeof(nvic_state_t));
        nvic_states[c].enable[0] = 0x0;
        nvic_states[c].pending[0] = 0x0;
        nvic_states[c].enable[1] = 0x0;
        nvic_states[c].pending[1] = 0x0;
        nvic_states[c].iabr[0] = 0x0;
        nvic_states[c].iabr[1] = 0x0;
        nvic_states[c].active_exceptions = 0x0;
        nvic_states[c].active_exceptions_hi = 0x0;
        nvic_states[c].shpr1 = 0;
        nvic_states[c].shpr2 = 0;
        nvic_states[c].shpr3 = 0;
        nvic_states[c].pendsv_pending = 0;
        for (int i = 0; i < NUM_EXTERNAL_IRQS_M33; i++) {
            nvic_states[c].priority[i] = 0;
        }
    }
    memset(&mpu_state, 0, sizeof(mpu_state));
    memset(&sau_state, 0, sizeof(sau_state));

    for (int c = 0; c < 2; c++) {
        memset(&systick_states[c], 0, sizeof(systick_state_t));
    }

    last_irq_signal = 0xFFFFFFFF;
    irq_signal_count = 0;
}

/* Initialize SysTick */
void systick_init(void) {
    systick_reset();
}

/* Reset SysTick to power-on defaults (both cores) */
void systick_reset(void) {
    for (int c = 0; c < 2; c++) {
        memset(&systick_states[c], 0, sizeof(systick_state_t));
    }
}

/* Tick SysTick timer - called once per CPU step for active core.
 * O(1) arithmetic: handles multi-cycle steps without looping.
 *
 * The original per-cycle loop behavior:
 *   1. if cvr > 0: cvr--
 *   2. if cvr == 0: set COUNTFLAG, reload, pend interrupt
 *
 * This means a cycle that decrements cvr to 0 ALSO fires the event
 * in the same cycle, and the reload value is what the next cycle sees.
 */
void systick_tick(uint32_t cycles) {
    systick_state_t *st = systick_cur();
    if (!(st->csr & 1)) return; /* Not enabled */

    uint32_t reload = st->rvr & 0x00FFFFFF;
    uint32_t remaining = cycles;

    while (remaining > 0) {
        if (st->cvr == 0) {
            /* Counter already at zero from a previous tick — reload and fire.
             * This consumes one cycle (the cycle that "sees" zero and reloads).
             * With reload==0, real silicon does not re-fire every cycle
             * (Arduino enables CSR before programming RVR); fire once until
             * RVR is rewritten, so main isn't starved before its RVR store. */
            uint32_t reload0 = st->rvr & 0x00FFFFFF;
            st->cvr = reload0;
            if (reload0 == 0) {
                if (!st->zero_fired) {
                    st->zero_fired = 1;
                    st->csr |= (1u << 16); /* COUNTFLAG */
                    if (st->csr & 2) {
                        st->pending = 1;
                        corepool_wake_cores();
                    }
                }
                return;
            }
            st->csr |= (1u << 16); /* COUNTFLAG */
            if (st->csr & 2) {
                st->pending = 1;
                corepool_wake_cores();
            }
            remaining--;
            continue;
        }

        if (remaining < st->cvr) {
            /* Won't reach zero this batch */
            st->cvr -= remaining;
            return;
        }

        /* Will reach zero: consume cvr cycles to hit 0 */
        remaining -= st->cvr;
        st->cvr = 0;

        /* The cycle that caused cvr to reach 0 also fires the event */
        st->csr |= (1u << 16); /* COUNTFLAG */
        st->cvr = reload;
        if (st->csr & 2) {
            st->pending = 1;
            corepool_wake_cores();
        }
        if (reload == 0) return;

        /* Fast-skip full periods if remaining > reload */
        if (remaining > reload) {
            uint32_t full_periods = (remaining - 1) / reload;
            remaining -= full_periods * reload;
            if (st->csr & 2) {
                st->pending = 1;
                corepool_wake_cores();
            }
        }
    }
}

void systick_tick_for_core(int core_id, uint32_t cycles) {
    if (core_id < 0 || core_id >= NUM_CORES) {
        return;
    }

    int saved_core = get_active_core();
    set_active_core(core_id);
    systick_tick(cycles);
    set_active_core(saved_core);
}

/**
 * Get effective priority of an exception vector number.
 * Uses active core's NVIC state.
 */
uint8_t nvic_get_exception_priority(uint32_t vector_num) {
    nvic_state_t *ns = nvic_cur();
    switch (vector_num) {
        case EXC_RESET:
        case EXC_NMI:
        case EXC_HARDFAULT:
            return 0; /* Fixed highest priority */
        case EXC_SVCALL:
            return (ns->shpr2 >> 24) & 0xC0;
        case EXC_PENDSV:
            return (ns->shpr3 >> 16) & 0xC0;
        case EXC_SYSTICK:
            return (ns->shpr3 >> 24) & 0xC0;
        default:
            if (vector_num >= 16 && (vector_num - 16) < NUM_EXTERNAL_IRQS_M33) {
                return ns->priority[vector_num - 16] & 0xC0;
            }
            return 0xFF;
    }
}

/* Enable an IRQ on current core (set in ISER) */
void nvic_enable_irq(uint32_t irq) {
    if (irq < NUM_EXTERNAL_IRQS_M33) {
        nvic_state_t *ns = nvic_cur();
        nvic_bit_set(ns->enable, irq);
        if (cpu.debug_enabled)
            printf("[NVIC] Core %d: Enabled IRQ %u (enable mask=0x%X)\n",
                   get_active_core(), irq, ns->enable[0]);
    }
}

/* Disable an IRQ on current core (set in ICER) */
void nvic_disable_irq(uint32_t irq) {
    if (irq < NUM_EXTERNAL_IRQS_M33) {
        nvic_state_t *ns = nvic_cur();
        nvic_bit_clear(ns->enable, irq);
        if (cpu.debug_enabled)
            printf("[NVIC] Core %d: Disabled IRQ %u (enable mask=0x%X)\n",
                   get_active_core(), irq, ns->enable[0]);
    }
}

/* Mark an IRQ as pending on current core (firmware ISPR write) */
void nvic_set_pending(uint32_t irq) {
    if (irq < NUM_EXTERNAL_IRQS_M33) {
        nvic_state_t *ns = nvic_cur();
        nvic_bit_set(ns->pending, irq);
        corepool_wake_cores();
        if (cpu.debug_enabled)
            printf("[NVIC] Core %d: Set pending IRQ %u (pending=0x%X, enable=0x%X)\n",
                   get_active_core(), irq, ns->pending[0], ns->enable[0]);
    }
}

/* Clear pending bit on current core (set in ICPR) */
void nvic_clear_pending(uint32_t irq) {
    if (irq < NUM_EXTERNAL_IRQS_M33) {
        nvic_state_t *ns = nvic_cur();
        nvic_bit_clear(ns->pending, irq);
        if (cpu.debug_enabled)
            printf("[NVIC] Core %d: Cleared pending IRQ %u (pending now=0x%X)\n",
                   get_active_core(), irq, ns->pending[0]);
    }
}

/* Set priority for an IRQ on current core */
void nvic_set_priority(uint32_t irq, uint8_t priority) {
    if (irq < NUM_EXTERNAL_IRQS_M33) {
        nvic_state_t *ns = nvic_cur();
        ns->priority[irq] = priority & 0xC0;
        /* Recompute fast-path flag */
        ns->priorities_nondefault = 0;
        for (uint32_t i = 0; i < NUM_EXTERNAL_IRQS_M33; i++) {
            if (ns->priority[i] != 0) { ns->priorities_nondefault = 1; break; }
        }
    }
}

/**
 * Get the highest priority pending IRQ for the active core.
 * Optimized: fast-path uses CTZ when all priorities are default (0).
 */
uint32_t nvic_get_pending_irq(void) {
    nvic_state_t *ns = nvic_cur();
    /* Two words: word 0 = IRQs 0-31, word 1 = IRQs 32-63. Scan word 0
     * first so the lowest enabled+pending IRQ wins at equal priority. */
    uint32_t words[2] = {
        ns->pending[0] & ns->enable[0],
        ns->pending[1] & ns->enable[1],
    };

    if ((words[0] | words[1]) == 0) {
        return 0xFFFFFFFF;
    }

    /* Fast path: if no custom priorities set, lowest IRQ number wins */
    if (ns->priorities_nondefault == 0) {
        if (words[0]) return (uint32_t)__builtin_ctz(words[0]);
        return 32u + (uint32_t)__builtin_ctz(words[1]);
    }

    /* Slow path: scan for highest priority (lowest value) */
    uint32_t highest_priority_irq = 0xFFFFFFFF;
    uint8_t highest_priority_value = 0xFF;
    for (int w = 0; w < 2; w++) {
        uint32_t bits = words[w];
        while (bits) {
            uint32_t bit = (uint32_t)__builtin_ctz(bits);
            uint32_t irq = (uint32_t)(w * 32) + bit;
            uint8_t prio = ns->priority[irq] & 0xC0;

            if (prio < highest_priority_value ||
                (prio == highest_priority_value && irq < highest_priority_irq)) {
                highest_priority_value = prio;
                highest_priority_irq = irq;
            }
            bits &= bits - 1; /* Clear lowest set bit */
        }
    }

    return highest_priority_irq;
}

/* Read NVIC register (current core's view) */
uint32_t nvic_read_register(uint32_t addr) {
    nvic_state_t *ns = nvic_cur();
    systick_state_t *st = systick_cur();

    /* PPB STIR (0xE000EF00, M33 only): software-trigger an IRQ
     * 0-511. The RP2350 SDK / Arduino core uses it for user IRQs.
     * Read: RAZ. Write handled in nvic_write_register. */
    if (addr == NVIC_STIR)
        return 0;

    switch (addr) {
        /* SysTick registers */
        case SYST_CSR:
            {
                uint32_t val = st->csr;
                /* COUNTFLAG (bit 16) is cleared on read */
                st->csr &= ~(1u << 16);
                return val;
            }
        case SYST_RVR:
            return st->rvr & 0x00FFFFFF;
        case SYST_CVR:
            return st->cvr & 0x00FFFFFF;
        case SYST_CALIB:
            return 0xC0002710;

        /* NVIC registers. ISER/ICER/ISPR/ICPR/IABR have per-group
         * aliases (M33: one word per 32 IRQs; RP2350 SDK irq_set_enabled
         * writes ISER[irq>>5] = 0xE000E100+((irq>>5)<<2): TIMER/GPIO21
         * land in ISER0, UART/SPI/I2C (32+) in ISER1). IPR has one byte
         * per IRQ (M33 numbering: 0xE000E400+N). */
        case NVIC_ISER:
        case NVIC_ISER + 4:
            {
                uint32_t grp = (addr - NVIC_ISER) / 4;
                if (grp < 2) return ns->enable[grp];
                return 0;
            }

        case NVIC_ICER:
        case NVIC_ICER + 4:
            {
                uint32_t grp = (addr - NVIC_ICER) / 4;
                if (grp < 2) return ns->enable[grp];
                return 0;
            }

        case NVIC_ISPR:
        case NVIC_ISPR + 4:
            {
                uint32_t grp = (addr - NVIC_ISPR) / 4;
                if (grp < 2) return ns->pending[grp];
                return 0;
            }

        case NVIC_ICPR:
        case NVIC_ICPR + 4:
            {
                uint32_t grp = (addr - NVIC_ICPR) / 4;
                if (grp < 2) return ns->pending[grp];
                return 0;
            }

        case NVIC_IABR:
            return ns->iabr[0];
        case NVIC_IABR + 4:
            return ns->iabr[1];

        case NVIC_IPR:
        case NVIC_IPR + 4:
        case NVIC_IPR + 8:
        case NVIC_IPR + 12:
        case NVIC_IPR + 16:
        case NVIC_IPR + 20:
        case NVIC_IPR + 24:
        case NVIC_IPR + 28:
        case NVIC_IPR + 32:
        case NVIC_IPR + 36:
        case NVIC_IPR + 40:
        case NVIC_IPR + 44:
        case NVIC_IPR + 48:
        case NVIC_IPR + 52:
        case NVIC_IPR + 56:
        case NVIC_IPR + 60:
            {
                /* M33: one priority byte per IRQ at 0xE000E400+N.
                 * 16 words cover IRQs 0-63 (RP2350 uses 0-51). */
                uint32_t offset = (addr - NVIC_IPR) / 4;
                if (offset < 16) {
                    uint32_t result = 0;
                    for (int i = 0; i < 4; i++) {
                        uint32_t irq_idx = offset * 4 + i;
                        if (irq_idx < NUM_EXTERNAL_IRQS_M33) {
                            result |= ((uint32_t)(ns->priority[irq_idx] & 0xC0u)) << (i * 8);
                        }
                    }
                    return result;
                }
            }
            return 0;

        /* SCB registers */
        case SCB_ICSR:
            {
                uint32_t pending_irq = nvic_get_pending_irq();
                uint32_t val = 0;
                if (pending_irq != 0xFFFFFFFF) {
                    val |= ((pending_irq + 16) << ICSR_VECTPENDING_SHIFT);
                    val |= ICSR_ISRPENDING;
                }
                if (st->pending)
                    val |= ICSR_PENDSTSET;
                if (ns->pendsv_pending)
                    val |= ICSR_PENDSVSET;
                return val;
            }

        case SCB_VTOR:
            return cpu.vtor;

        case SCB_BASE:  /* 0xE000ED00 - CPUID */
            return nvic_cpuid_value;

        /* NOTE: SCB+8 (0xE000ED08 = VTOR) is already handled by the
         * SCB_VTOR case above (same address). The SDK's
         * irq_get_vtable_handler does LDR r3,[0xE000ED00,#8] to fetch
         * VTOR — that path was verified returning cpu.vtor. */

        case SCB_AIRCR:
            return 0x05FA0000;

        case SCB_SCR:
            return 0;

        case SCB_CCR:
            return (1u << 9);

        case SCB_SHPR1:
            return ns->shpr1;

        case SCB_SHPR2:
            return ns->shpr2;

        case SCB_SHPR3:
            return ns->shpr3;

        case SCB_SHCSR: {
            /* M33: fault enables/pended. M0+: RAZ. */
            if (!membus_rp2350_mode) return 0;
            return ns->shcsr;
        }

        case SCB_CFSR:
            if (!membus_rp2350_mode) return 0;
            return ns->cfsr;

        case SCB_HFSR:
            if (!membus_rp2350_mode) return 0;
            return ns->hfsr;

        case SCB_MMFAR:
            if (!membus_rp2350_mode) return 0;
            return ns->mmfar;

        case SCB_BFAR:
            if (!membus_rp2350_mode) return 0;
            return ns->bfar;

        case MPU_TYPE:
            /* DREGION=8, IREGION=0 (unified), SEPARATE=0. */
            if (!membus_rp2350_mode) return 0;
            return (MPU_TYPE_DREGION << 8);
        case MPU_CTRL:
            if (!membus_rp2350_mode) return 0;
            return mpu_state.ctrl;
        case MPU_RNR:
            if (!membus_rp2350_mode) return 0;
            return mpu_state.rnr & 7u;
        case MPU_RBAR:
        case MPU_RBAR_A1:
        case MPU_RBAR_A2:
        case MPU_RBAR_A3: {
            if (!membus_rp2350_mode) return 0;
            int r = (addr == MPU_RBAR) ? (int)(mpu_state.rnr & 7u)
                : (addr == MPU_RBAR_A1) ? 1 : (addr == MPU_RBAR_A2) ? 2 : 3;
            return mpu_state.rbar[r];
        }
        case MPU_RLAR:
        case MPU_RLAR_A1:
        case MPU_RLAR_A2:
        case MPU_RLAR_A3: {
            if (!membus_rp2350_mode) return 0;
            int r = (addr == MPU_RLAR) ? (int)(mpu_state.rnr & 7u)
                : (addr == MPU_RLAR_A1) ? 1 : (addr == MPU_RLAR_A2) ? 2 : 3;
            return mpu_state.rlar[r];
        }
        case MPU_MAIR0:
            if (!membus_rp2350_mode) return 0;
            return mpu_state.mair[0];
        case MPU_MAIR1:
            if (!membus_rp2350_mode) return 0;
            return mpu_state.mair[1];

        case SAU_CTRL:
            if (!membus_rp2350_mode) return 0;
            return sau_state.ctrl & (SAU_CTRL_ENABLE | SAU_CTRL_ALLNS);
        case SAU_TYPE:
            if (!membus_rp2350_mode) return 0;
            return SAU_TYPE_SREGION; /* 8 regions */
        case SAU_RNR:
            if (!membus_rp2350_mode) return 0;
            return sau_state.rnr & 7u;
        case SAU_RBAR:
            if (!membus_rp2350_mode) return 0;
            return sau_state.rbar[sau_state.rnr & 7u];
        case SAU_RLAR:
            if (!membus_rp2350_mode) return 0;
            return sau_state.rlar[sau_state.rnr & 7u];

        default:
            return 0;
    }
}

/* Write NVIC register (current core's state) */
void nvic_write_register(uint32_t addr, uint32_t val) {
    nvic_state_t *ns = nvic_cur();
    systick_state_t *st = systick_cur();

    /* PPB STIR (M33 only): bits [8:0] = IRQ 0-511 to pend. M0+ has no
     * STIR (WI). Route through nvic_signal_irq so both cores pend. */
    if (addr == NVIC_STIR) {
        if (membus_rp2350_mode && (val & 0x1FFu) < NUM_EXTERNAL_IRQS_M33)
            nvic_signal_irq(val & 0x1FFu);
        return;
    }

    switch (addr) {
        /* SysTick registers */
        case SYST_CSR:
            st->csr = (st->csr & ~0x7) | (val & 0x7);
            if (cpu.debug_enabled) {
                printf("[SYSTICK] Core %d: CSR = 0x%08X (EN=%d TICKINT=%d)\n",
                       get_active_core(), st->csr, (int)(val & 1), (int)((val >> 1) & 1));
            }
            break;
        case SYST_RVR:
            st->rvr = val & 0x00FFFFFF;
            st->zero_fired = 0;  /* re-arm zero-reload firing */
            break;
        case SYST_CVR:
            st->cvr = 0;
            st->csr &= ~(1u << 16);
            break;

        /* NVIC registers (ISER/ICER/ISPR/ICPR word-N aliases route to
         * IRQ group N; only group 0 exists in the 32-IRQ model — see
         * the read path above). */
        case NVIC_ISER:
        case NVIC_ISER + 4:
            {
                uint32_t grp = (addr - NVIC_ISER) / 4;
                if (grp < 2) {
                    ns->enable[grp] |= val;
                    if (cpu.debug_enabled)
                        printf("[NVIC] Core %d: Write ISER%u: 0x%X, enabled mask now=0x%X%08X\n",
                               get_active_core(), grp, val, ns->enable[1], ns->enable[0]);
                }
            }
            break;

        case NVIC_ICER:
        case NVIC_ICER + 4:
            {
                uint32_t grp = (addr - NVIC_ICER) / 4;
                if (grp < 2) {
                    ns->enable[grp] &= ~val;
                    if (cpu.debug_enabled)
                        printf("[NVIC] Core %d: Write ICER%u: 0x%X, enabled mask now=0x%X%08X\n",
                               get_active_core(), grp, val, ns->enable[1], ns->enable[0]);
                }
            }
            break;

        case NVIC_ISPR:
        case NVIC_ISPR + 4:
            {
                uint32_t grp = (addr - NVIC_ISPR) / 4;
                if (grp < 2) {
                    ns->pending[grp] |= val;
                    if (cpu.debug_enabled)
                        printf("[NVIC] Core %d: Write ISPR%u: 0x%X, pending mask now=0x%X%08X\n",
                               get_active_core(), grp, val, ns->pending[1], ns->pending[0]);
                }
            }
            break;

        case NVIC_ICPR:
        case NVIC_ICPR + 4:
            {
                uint32_t grp = (addr - NVIC_ICPR) / 4;
                if (grp < 2) {
                    ns->pending[grp] &= ~val;
                    if (cpu.debug_enabled)
                        printf("[NVIC] Core %d: Write ICPR%u: 0x%X, pending mask now=0x%X%08X\n",
                               get_active_core(), grp, val, ns->pending[1], ns->pending[0]);
                }
            }
            break;

        case NVIC_IPR:
        case NVIC_IPR + 4:
        case NVIC_IPR + 8:
        case NVIC_IPR + 12:
        case NVIC_IPR + 16:
        case NVIC_IPR + 20:
        case NVIC_IPR + 24:
        case NVIC_IPR + 28:
        case NVIC_IPR + 32:
        case NVIC_IPR + 36:
        case NVIC_IPR + 40:
        case NVIC_IPR + 44:
        case NVIC_IPR + 48:
        case NVIC_IPR + 52:
        case NVIC_IPR + 56:
        case NVIC_IPR + 60:
            {
                /* M33: one priority byte per IRQ at 0xE000E400+N. */
                uint32_t offset = (addr - NVIC_IPR) / 4;
                if (offset < 16) {
                    for (int i = 0; i < 4; i++) {
                        uint32_t irq_idx = offset * 4 + i;
                        if (irq_idx < NUM_EXTERNAL_IRQS_M33) {
                            ns->priority[irq_idx] = (uint8_t)((val >> (i * 8)) & 0xC0u);
                        }
                    }
                    /* Recompute fast-path flag (H9/L7: only effective bits) */
                    ns->priorities_nondefault = 0;
                    for (uint32_t i = 0; i < NUM_EXTERNAL_IRQS_M33; i++) {
                        if ((ns->priority[i] & 0xC0u) != 0) { ns->priorities_nondefault = 1; break; }
                    }
                }
            }
            break;

        /* SCB registers */
        case SCB_ICSR:
            if (val & ICSR_PENDSVCLR)
                ns->pendsv_pending = 0;
            if (val & ICSR_PENDSTCLR)
                st->pending = 0;
            if (val & ICSR_PENDSVSET) {
                ns->pendsv_pending = 1;
                corepool_wake_cores();
            }
            if (val & ICSR_PENDSTSET) {
                st->pending = 1;
                corepool_wake_cores();
            }
            break;

        case SCB_VTOR:
            cpu.vtor = val & 0xFFFFFF80;
            break;

        case SCB_SHPR1:
            /* M33 fault priorities (MemManage/BusFault/UsageFault at
             * bytes 0-2). M0+: RAZ/WI. */
            if (membus_rp2350_mode) ns->shpr1 = val & 0x00C0C0C0u;
            break;

        case SCB_SHCSR: {
            /* M33: writable SVCALLPENDED/SYSTICK pend bits + fault
             * enables (MEMFAULTENA/BUSFAULTENA/USGFAULTENA). M0+: stub
             * (PendSV/SysTick via ICSR only). */
            if (!membus_rp2350_mode) break;
            /* Writable: bits 15 (SVCALLPENDED is RO-pend — accept set),
             * 18/17/16 enables. Keep others read-only. */
            if (val & (1u << 15)) ns->pendsv_pending = 1; /* SVCALLPENDED */
            ns->shcsr = (ns->shcsr & ~0x00070000u) | (val & 0x00070000u);
            break;
        }

        case SCB_CFSR:
            /* W1C fault status bits (M33 only). */
            if (membus_rp2350_mode) ns->cfsr &= ~val;
            break;

        case SCB_HFSR:
            /* W1C (FORCED/DEBUGEVT/VECTTBL), M33 only. */
            if (membus_rp2350_mode) ns->hfsr &= ~val;
            break;

        case SCB_MMFAR:
        case SCB_BFAR:
            /* RW fault address registers (banked by fault type). */
            if (membus_rp2350_mode) {
                if (addr == SCB_MMFAR) ns->mmfar = val;
                else ns->bfar = val;
            }
            break;

        case MPU_CTRL:
            if (membus_rp2350_mode)
                mpu_state.ctrl = val & (MPU_CTRL_ENABLE | MPU_CTRL_HFNMIENA |
                                        MPU_CTRL_PRIVDEFENA);
            break;
        case MPU_RNR:
            if (membus_rp2350_mode) mpu_state.rnr = val & 7u;
            break;
        case MPU_RBAR:
        case MPU_RBAR_A1:
        case MPU_RBAR_A2:
        case MPU_RBAR_A3: {
            if (!membus_rp2350_mode) break;
            int r = (addr == MPU_RBAR) ? (int)(mpu_state.rnr & 7u)
                : (addr == MPU_RBAR_A1) ? 1 : (addr == MPU_RBAR_A2) ? 2 : 3;
            mpu_state.rbar[r] = val & 0xFFFFFF1Fu;
            break;
        }
        case MPU_RLAR:
        case MPU_RLAR_A1:
        case MPU_RLAR_A2:
        case MPU_RLAR_A3: {
            if (!membus_rp2350_mode) break;
            int r = (addr == MPU_RLAR) ? (int)(mpu_state.rnr & 7u)
                : (addr == MPU_RLAR_A1) ? 1 : (addr == MPU_RLAR_A2) ? 2 : 3;
            mpu_state.rlar[r] = val & 0xFFFFFF3Fu;
            break;
        }
        case MPU_MAIR0:
            if (membus_rp2350_mode) mpu_state.mair[0] = val;
            break;
        case MPU_MAIR1:
            if (membus_rp2350_mode) mpu_state.mair[1] = val;
            break;

        case SAU_CTRL:
            if (membus_rp2350_mode)
                sau_state.ctrl = val & (SAU_CTRL_ENABLE | SAU_CTRL_ALLNS);
            break;
        case SAU_RNR:
            if (membus_rp2350_mode) sau_state.rnr = val & 7u;
            break;
        case SAU_RBAR:
            if (membus_rp2350_mode)
                sau_state.rbar[sau_state.rnr & 7u] = val & 0xFFFFFFE0u;
            break;
        case SAU_RLAR:
            if (membus_rp2350_mode)
                sau_state.rlar[sau_state.rnr & 7u] = val & 0xFFFFFFE3u;
            break;

        case SCB_SHPR2:
            ns->shpr2 = val & 0xC0000000;
            break;

        case SCB_SHPR3:
            ns->shpr3 = val & 0xC0C00000;
            break;

        case SCB_AIRCR:
            if ((val >> 16) == 0x05FA) {
                if (val & (1u << 2)) {
                    extern int watchdog_reboot_pending;
                    watchdog_reboot_pending = 1;
                }
            }
            break;
        case SCB_SCR:
        case SCB_CCR:
            break;

        default:
            break;
    }
}

/* Called by peripherals to signal an interrupt on BOTH cores' NVICs.
 * On real RP2040, the interrupt line goes to both cores' NVICs.
 * Each core independently decides whether to handle based on its own enable mask. */
void nvic_signal_irq(uint32_t irq) {
    if (irq < NUM_EXTERNAL_IRQS_M33) {
        irq_signal_count++;

        if (cpu.debug_enabled) {
            printf("[NVIC] *** SIGNAL IRQ %u (count=%u) ***\n",
                   irq, irq_signal_count);
        }

        last_irq_signal = irq;

        /* IRQ latency profiling: record pend time */
        if (__builtin_expect(irq_latency_enabled, 0))
            irq_latency_pend(irq);

        /* Set pending on BOTH cores (shared interrupt line) */
        for (int c = 0; c < 2; c++) {
            nvic_bit_set(nvic_states[c].pending, irq);
        }
        corepool_wake_cores();
    }
}

/* ========================================================================
 * M33 MPU / SAU / fault model (PMSAv8 + TrustZone attribution)
 *
 * Single Secure world: all emulated memory is Secure; SAU only affects
 * attribution answers (sau_attr / TT). MPU enforces region permissions
 * when MPU_CTRL.ENABLE is set; faults pend MemManage (or HardFault when
 * the MPU is off and the access hits PPB, matching silicon).
 * ======================================================================== */

int sau_attr(uint32_t addr) {
    /* 0 = Secure, 1 = Non-secure, 2 = NSC */
    if (!membus_rp2350_mode) return 0;
    if (!(sau_state.ctrl & SAU_CTRL_ENABLE)) {
        /* SAU disabled: ALLNS decides the whole map. */
        return (sau_state.ctrl & SAU_CTRL_ALLNS) ? 1 : 0;
    }
    for (int r = 0; r < 8; r++) {
        uint32_t rlar = sau_state.rlar[r];
        if (!(rlar & SAU_RLAR_ENABLE)) continue;
        uint32_t base = sau_state.rbar[r] & 0xFFFFFFE0u;
        uint32_t limit = (rlar & 0xFFFFFFE0u) | 0x1Fu;
        if (addr >= base && addr <= limit)
            return (rlar & SAU_RLAR_NSC) ? 2 : 1;
    }
    return 0; /* Secure by default */
}

uint32_t tt_answer(uint32_t addr, int alt) {
    /* TT answer word (ARMv8-M: S=j22, M=j21, R=j20, RW=j19, RWn=j18,
     * RKn=j17, ... I=j0 for MPU no-read). The RP2350 SDK trampoline +
     * pico_processor_state_is_nonsecure() test S (bit 22): Secure
     * execution must report S=1 for Secure addresses. When SAU/MPU are
     * unprogrammed (typical Arduino boot: SAU off/ALLNS=0, MPU off) all
     * memory is Secure and readable, so the answer is S alone
     * (1<<22) — NOT 0 (0 = Non-secure, which sent every SDK ROM lookup
     * down the non-secure table path to address 0 and wedged the core).
     * Once firmware programs SAU/MPU, report real attribution (S vs M
     * + I for no-read). ALT variant reports the unprivileged view. */
    int sau_on = (sau_state.ctrl & SAU_CTRL_ENABLE) != 0;
    int mpu_on = (mpu_state.ctrl & MPU_CTRL_ENABLE) != 0;
    if (!sau_on && !mpu_on) {
        /* SAU off: ALLNS=1 means whole map Non-secure, else Secure.
         * Report S (bit 22) for Secure — this is what the RP2350 SDK
         * trampoline + pico_processor_state_is_nonsecure() test. */
        if (sau_state.ctrl & SAU_CTRL_ALLNS) return (1u << 21); /* M */
        return (1u << 22); /* S */
    }
    int attr = sau_attr(addr);
    int is_priv = alt ? 0 : ((cpu.control & 2) == 0);
    uint32_t ans = 0;
    if (attr == 0) ans |= (1u << 22);       /* S: Secure */
    else ans |= (1u << 21);                 /* M: Non-secure (or NSC) */
    if (mpu_check(addr, 0, is_priv, 0) != 0) ans |= (1u << 0); /* I: no read */
    return ans;
}

int mpu_check(uint32_t addr, int is_write, int is_priv, int is_exec) {
    (void)is_exec;
    if (!membus_rp2350_mode) return 0; /* M0+: no MPU */
    if (!(mpu_state.ctrl & MPU_CTRL_ENABLE)) {
        /* MPU off: PPB (0xE0000000-0xE000FFFF) faults for unprivileged
         * or for any access without PRIVDEFENA; everything else passes. */
        if (addr >= 0xE0000000u && addr < 0xE0010000u) {
            if (!is_priv || !(mpu_state.ctrl & MPU_CTRL_PRIVDEFENA))
                return EXC_HARDFAULT;
        }
        return 0;
    }
    /* MPU on: highest-numbered matching enabled region wins. */
    int hit = -1;
    for (int r = 0; r < 8; r++) {
        uint32_t rlar = mpu_state.rlar[r];
        if (!(rlar & MPU_RLAR_EN)) continue;
        uint32_t base = mpu_state.rbar[r] & 0xFFFFFFE0u;
        uint32_t limit = (rlar & 0xFFFFFFE0u) | 0x1Fu;
        if (addr >= base && addr <= limit) hit = r;
    }
    if (hit < 0) {
        /* No region: background map for privileged when PRIVDEFENA,
         * HardFault otherwise (and always for unprivileged). */
        if (is_priv && (mpu_state.ctrl & MPU_CTRL_PRIVDEFENA)) return 0;
        return EXC_HARDFAULT;
    }
    /* AP[2:1] in RBAR (PMSAv8): 00/01=p/n RW, 10=priv RW only,
     * 11=RO (reads for all, no writes). XN bit 0 forbids execution
     * (checked by caller). */
    uint32_t ap = (mpu_state.rbar[hit] >> 1) & 3u;
    if (is_write) {
        if (ap == 3) return EXC_MEMFAULT;
        if (!is_priv && ap == 2) return EXC_MEMFAULT;
    } else {
        if (!is_priv && ap == 2) return EXC_MEMFAULT;
    }
    return 0;
}

void nvic_raise_fault(uint32_t exc, uint32_t cfsr_bits) {
    nvic_state_t *ns = nvic_cur();
    if (!membus_rp2350_mode) {
        /* M0+: everything escalates to HardFault. */
        nvic_bit_set(ns->pending, 32); /* not a real IRQ — handled by caller */
        (void)exc; (void)cfsr_bits;
        return;
    }
    ns->hfsr |= (1u << 30); /* FORCED: a configurable fault escalated */
    if (exc == EXC_MEMFAULT) {
        ns->cfsr |= cfsr_bits;
        ns->shcsr |= (1u << 0); /* MEMFAULTPENDED */
    } else if (exc == EXC_BUSFAULT) {
        ns->cfsr |= cfsr_bits;
        ns->shcsr |= (1u << 1); /* BUSFAULTPENDED */
    } else if (exc == EXC_USAGEFAULT) {
        ns->cfsr |= cfsr_bits;
        ns->shcsr |= (1u << 2); /* USGFAULTPENDED */
    }
}
