#include <string.h>
#include "pwm.h"
#include "nvic.h"
#include "emulator.h"
#include "rp2350_rv/rp2350_memmap.h"

pwm_state_t pwm_state;

/* Slice count follows the chip: 8 on RP2040 (globals at 0xA0 block),
 * 12 on RP2350 (globals at 0xF0 block). The two layouts overlap at
 * 0xA0-0xEF (slices 8-11 vs legacy globals), so the active layout is
 * selected by membus_rp2350_mode — matching silicon. */
static inline int pwm_nslices(void) {
    return membus_rp2350_mode ? PWM_NUM_SLICES : PWM_NUM_SLICES_RP2040;
}

void pwm_init(void) {
    memset(&pwm_state, 0, sizeof(pwm_state));
    /* Default TOP = 0xFFFF for all slices */
    for (int i = 0; i < PWM_NUM_SLICES; i++) {
        pwm_state.slice[i].top = 0xFFFF;
        pwm_state.slice[i].div = 0x10;  /* Divider = 1.0 (integer 1, frac 0) */
    }
}

int pwm_match(uint32_t addr) {
    uint32_t base = addr & ~0x3000;
    if (base >= PWM_BASE && base < PWM_BASE + PWM_BLOCK_SIZE)
        return 1;
    /* RP2350 moved PWM to 0x400A8000 (same layout + 4 slices + IRQ1 block).
     * Without this, RP2350-mode writes to 0x400A8xxx fall into the
     * unmapped hole (0x40050000 there is PLL_SYS). */
    if (membus_rp2350_mode && base >= RP2350_PWM_BASE &&
        base < RP2350_PWM_BASE + PWM_BLOCK_SIZE)
        return 1;
    return 0;
}

uint32_t pwm_read32(uint32_t offset) {
    /* Per-slice registers (0x14 per slice; 8 on RP2040, 12 on RP2350) */
    if (offset < (uint32_t)(pwm_nslices() * 0x14)) {
        int slice = offset / 0x14;
        int reg = offset % 0x14;
        pwm_slice_t *s = &pwm_state.slice[slice];

        switch (reg) {
        case PWM_CH_CSR: return s->csr;
        case PWM_CH_DIV: return s->div;
        case PWM_CH_CTR: return s->ctr;
        case PWM_CH_CC:  return s->cc;
        case PWM_CH_TOP: return s->top;
        default: return 0;
        }
    }

    /* RP2350 globals at 0xF0 block (always live: 0xF0 > any slice). */
    switch (offset) {
    case PWM_RP2350_EN:   return pwm_state.en;
    case PWM_RP2350_INTR: return pwm_state.intr;
    case PWM_RP2350_IRQ0_INTE: return pwm_state.inte;
    case PWM_RP2350_IRQ0_INTF: return pwm_state.intf;
    case PWM_RP2350_IRQ0_INTS: return (pwm_state.intr | pwm_state.intf) & pwm_state.inte;
    case PWM_RP2350_IRQ1_INTE: return pwm_state.inte1;
    case PWM_RP2350_IRQ1_INTF: return pwm_state.intf1;
    case PWM_RP2350_IRQ1_INTS: return (pwm_state.intr | pwm_state.intf1) & pwm_state.inte1;
    default: break;
    }

    /* Legacy RP2040 globals at 0xA0 block: only when the 0xA0-0xEF range
     * is not slices (RP2040 mode). Masks are 8-bit there. */
    if (!membus_rp2350_mode) {
        switch (offset) {
        case PWM_EN:   return pwm_state.en;
        case PWM_INTR: return pwm_state.intr;
        case PWM_INTE: return pwm_state.inte;
        case PWM_INTF: return pwm_state.intf;
        case PWM_INTS: return (pwm_state.intr | pwm_state.intf) & pwm_state.inte;
        default: return 0;
        }
    }
    return 0;
}

void pwm_write32(uint32_t offset, uint32_t val) {
    /* Per-slice registers */
    if (offset < (uint32_t)(pwm_nslices() * 0x14)) {
        int slice = offset / 0x14;
        int reg = offset % 0x14;
        pwm_slice_t *s = &pwm_state.slice[slice];

        switch (reg) {
        case PWM_CH_CSR: s->csr = val & 0xFF; break;
        case PWM_CH_DIV: s->div = val & 0x0FFF; break;
        case PWM_CH_CTR: s->ctr = val & 0xFFFF; break;
        case PWM_CH_CC:  s->cc  = val; break;
        case PWM_CH_TOP: s->top = val & 0xFFFF; break;
        default: break;
        }
        return;
    }

    /* RP2350 globals (12-bit masks). */
    switch (offset) {
    case PWM_RP2350_EN:
        pwm_state.en = val & PWM_ALL_SLICES;
        break;
    case PWM_RP2350_INTR:
        /* Write-1-to-clear */
        pwm_state.intr &= ~(val & PWM_ALL_SLICES);
        break;
    case PWM_RP2350_IRQ0_INTE:
        pwm_state.inte = val & PWM_ALL_SLICES;
        break;
    case PWM_RP2350_IRQ0_INTF:
        pwm_state.intf = val & PWM_ALL_SLICES;
        break;
    case PWM_RP2350_IRQ1_INTE:
        pwm_state.inte1 = val & PWM_ALL_SLICES;
        break;
    case PWM_RP2350_IRQ1_INTF:
        pwm_state.intf1 = val & PWM_ALL_SLICES;
        break;
    default:
        break;
    }

    /* Legacy RP2040 globals (8-bit masks, RP2040 mode only). */
    if (!membus_rp2350_mode) {
        switch (offset) {
        case PWM_EN:
            pwm_state.en = val & 0xFF;
            break;
        case PWM_INTR:
            pwm_state.intr &= ~(val & 0xFF);
            break;
        case PWM_INTE:
            pwm_state.inte = val & 0xFF;
            break;
        case PWM_INTF:
            pwm_state.intf = val & 0xFF;
            break;
        default:
            break;
        }
    }

    /* Signal NVIC if any masked interrupt is active (either IRQ line) */
    if (((pwm_state.intr | pwm_state.intf) & pwm_state.inte) ||
        ((pwm_state.intr | pwm_state.intf1) & pwm_state.inte1))
        nvic_signal_rp2350_irq(IRQ_PWM_IRQ_WRAP);
}
