#include <stdio.h>
#include <string.h>
#include "gpio.h"
#include "emulator.h"
#include "nvic.h"
#include "devtools.h"

/* Helper: trace GPIO changes via VCD when gpio_out is modified */
static inline void gpio_trace_changes(uint32_t old_val, uint32_t new_val) {
    if (__builtin_expect(!gpio_trace_enabled, 1)) return;
    uint32_t changed = old_val ^ new_val;
    while (changed) {
        int pin = __builtin_ctz(changed);
        gpio_trace_record((uint8_t)pin, (new_val >> pin) & 1);
        changed &= changed - 1;
    }
}

/* GPIO state */
gpio_state_t gpio_state;

/* Initialize GPIO subsystem */
void gpio_init(void) {
    gpio_reset();
}

/* Reset GPIO to power-on defaults */
void gpio_reset(void) {
    memset(&gpio_state, 0, sizeof(gpio_state_t));

    /* Default: all pins as inputs with SIO function */
    for (int i = 0; i < NUM_GPIO_PINS; i++) {
        gpio_state.pins[i].ctrl = GPIO_FUNC_SIO;  /* Default to SIO function */
        gpio_state.pads[i] = 0x00000056;  /* Default pad config: IE=1, OD=0, PUE=1, PDE=1 */
    }

    /* All pins start as inputs (OE=0) */
    gpio_state.gpio_oe = 0x00000000;
    gpio_state.gpio_out = 0x00000000;
    gpio_state.gpio_in = 0x00000000;
    gpio_state.voltage_select = 0x00000001;  /* L5: default 3V3 bank voltage */
}

/* Recompute INTS and signal NVIC if any interrupt is active.
 * Covers all 6 banks (48 pins) and both cores' enable masks. */
static void gpio_check_irq(void) {
    uint32_t any_active = 0;
    for (int i = 0; i < 6; i++) {
        gpio_state.proc0_ints[i] = (gpio_state.intr[i] | gpio_state.proc0_intf[i])
                                    & gpio_state.proc0_inte[i];
        gpio_state.proc1_ints[i] = (gpio_state.intr[i] | gpio_state.proc1_intf[i])
                                    & gpio_state.proc1_inte[i];
        any_active |= gpio_state.proc0_ints[i] | gpio_state.proc1_ints[i];
    }
    if (any_active) {
        nvic_signal_irq(IRQ_IO_IRQ_BANK0);
    }
}

/*
 * Detect GPIO edge/level events by comparing old and new pin values.
 * Sets INTR bits: per pin 4 bits = [edge_high, edge_low, level_high, level_low]
 * Level interrupts are continuously asserted while the pin is at that level.
 * Edge interrupts are latched (W1C) when a transition occurs.
 */
static void gpio_detect_events(uint32_t old_pins, uint32_t new_pins) {
    /* Recompute level interrupts from current pin state (all 6 banks) */
    for (int reg = 0; reg < 6; reg++) {
        uint32_t level_bits = 0;
        for (int bit = 0; bit < 8; bit++) {
            int pin = reg * 8 + bit;
            if (pin >= NUM_GPIO_PINS) break;
            int val = (new_pins >> pin) & 1;
            uint32_t shift = bit * 4;
            /* Level low (bit 0): pin is 0 */
            if (!val) level_bits |= (GPIO_INTR_LEVEL_LOW << shift);
            /* Level high (bit 1): pin is 1 */
            if (val)  level_bits |= (GPIO_INTR_LEVEL_HIGH << shift);
        }
        /* Level bits are not latched — recompute every time.
         * Merge with existing edge bits (which are latched/W1C). */
        uint32_t edge_mask = 0;
        for (int bit = 0; bit < 8; bit++) {
            uint32_t shift = bit * 4;
            edge_mask |= ((GPIO_INTR_EDGE_LOW | GPIO_INTR_EDGE_HIGH) << shift);
        }
        gpio_state.intr[reg] = (gpio_state.intr[reg] & edge_mask) | level_bits;
    }

    /* Detect edges from changed pins (iterate only set bits, safe for 0-31) */
    uint32_t changed = old_pins ^ new_pins;
    while (changed) {
        int pin = __builtin_ctz(changed);
        changed &= changed - 1;
        {
            int reg = pin / 8;
            int bit = pin % 8;
            uint32_t shift = (uint32_t)bit * 4u;
            int new_val = (int)((new_pins >> (uint32_t)pin) & 1u);
            if (new_val) {
                /* Rising edge */
                gpio_state.intr[reg] |= (GPIO_INTR_EDGE_HIGH << shift);
            } else {
                /* Falling edge */
                gpio_state.intr[reg] |= (GPIO_INTR_EDGE_LOW << shift);
            }
        }
    }

    gpio_check_irq();
}

/* Compute effective pin values (what SIO_GPIO_IN would return) */
static uint32_t gpio_effective_pins(void) {
    return (gpio_state.gpio_out & gpio_state.gpio_oe) |
           (gpio_state.gpio_in & ~gpio_state.gpio_oe);
}

/* Read from GPIO register space */
uint32_t gpio_read32(uint32_t addr) {
    /* SIO GPIO registers (fast access) */
    if (addr >= SIO_BASE_GPIO && addr < SIO_BASE_GPIO + 0x100) {
        switch (addr) {
            case SIO_GPIO_IN:
                /* Return current input values */
                /* For pins configured as outputs, return the output value */
                /* For inputs, return the gpio_in value */
                return (gpio_state.gpio_out & gpio_state.gpio_oe) |
                       (gpio_state.gpio_in & ~gpio_state.gpio_oe);

            case SIO_GPIO_HI_IN:
                /* QSPI GPIO input: 6 pins (SCLK=0, SS=1, SD0-3=2-5) */
                /* Default: CS(SS) high, data lines high (pulled up) */
                return 0x3E;  /* bits 1-5 set: SS + SD0-3 high */

            case SIO_GPIO_OUT:
                return gpio_state.gpio_out;

            case SIO_GPIO_OE:
                return gpio_state.gpio_oe;

            default:
                /* RP2350 high GPIO (pins 32-47) at SIO+0x30-0x4C */
                if (addr >= SIO_BASE_GPIO + 0x30 && addr < SIO_BASE_GPIO + 0x50) {
                    uint32_t off = addr - (SIO_BASE_GPIO + 0x30);
                    if (off == 0x00) /* GPIO_HI_OUT */
                        return gpio_state.gpio_out_hi & 0xFFFFu;
                    if (off == 0x04) /* GPIO_HI_OE */
                        return gpio_state.gpio_oe_hi & 0xFFFFu;
                    if (off == 0x08) /* GPIO_HI_IN */
                        return ((gpio_state.gpio_out_hi & gpio_state.gpio_oe_hi) |
                                (gpio_state.gpio_in_hi & ~gpio_state.gpio_oe_hi)) & 0xFFFFu;
                }
                return 0x00000000;
        }
    }

    /* IO_BANK0 interrupt registers. Layout differs per chip:
     *   RP2040: INTR0-3 @0xF0,  P0INTE @0x100, P0INTF @0x110, P0INTS @0x120,
     *           P1INTE @0x130, P1INTF @0x140, P1INTS @0x150 (4 regs each)
     *   RP2350: INTR0-5 @0x230, P0INTE @0x248, P0INTF @0x260, P0INTS @0x278,
     *           P1INTE @0x290, P1INTF @0x2A8, P1INTS @0x2C0 (6 regs each)
     * A hybrid map (6 regs at RP2040 addresses) matches neither chip and
     * silently misroutes guest IRQ setup (e.g. CYW43 HOST_WAKE on pin 24:
     * guest INTE3@0x10C landed in our INTE1 on RP2040, so RX IRQs died). */
    {
        uint32_t base_addr = addr;
        if (addr >= IO_BANK0_BASE + REG_ALIAS_CLR_BITS && addr < IO_BANK0_BASE + REG_ALIAS_CLR_BITS + 0x400)
            base_addr -= REG_ALIAS_CLR_BITS;
        else if (addr >= IO_BANK0_BASE + REG_ALIAS_SET_BITS && addr < IO_BANK0_BASE + REG_ALIAS_SET_BITS + 0x400)
            base_addr -= REG_ALIAS_SET_BITS;
        else if (addr >= IO_BANK0_BASE + REG_ALIAS_XOR_BITS && addr < IO_BANK0_BASE + REG_ALIAS_XOR_BITS + 0x400)
            base_addr -= REG_ALIAS_XOR_BITS;

        uint32_t off = base_addr - IO_BANK0_BASE;
        int rp2350 = membus_rp2350_mode;
        /* (kind, index): 0=INTR 1=P0INTE 2=P0INTF 3=P0INTS 4=P1INTE 5=P1INTF 6=P1INTS */
        int kind = -1, idx = 0;
        if (!rp2350) {
            if (off >= 0xF0 && off < 0x100)       { kind = 0; idx = (off - 0xF0) / 4; }
            else if (off >= 0x100 && off < 0x110) { kind = 1; idx = (off - 0x100) / 4; }
            else if (off >= 0x110 && off < 0x120) { kind = 2; idx = (off - 0x110) / 4; }
            else if (off >= 0x120 && off < 0x130) { kind = 3; idx = (off - 0x120) / 4; }
            else if (off >= 0x130 && off < 0x140) { kind = 4; idx = (off - 0x130) / 4; }
            else if (off >= 0x140 && off < 0x150) { kind = 5; idx = (off - 0x140) / 4; }
            else if (off >= 0x150 && off < 0x160) { kind = 6; idx = (off - 0x150) / 4; }
        } else {
            if (off >= 0x230 && off < 0x248)      { kind = 0; idx = (off - 0x230) / 4; }
            else if (off >= 0x248 && off < 0x260) { kind = 1; idx = (off - 0x248) / 4; }
            else if (off >= 0x260 && off < 0x278) { kind = 2; idx = (off - 0x260) / 4; }
            else if (off >= 0x278 && off < 0x290) { kind = 3; idx = (off - 0x278) / 4; }
            else if (off >= 0x290 && off < 0x2A8) { kind = 4; idx = (off - 0x290) / 4; }
            else if (off >= 0x2A8 && off < 0x2C0) { kind = 5; idx = (off - 0x2A8) / 4; }
            else if (off >= 0x2C0 && off < 0x2D8) { kind = 6; idx = (off - 0x2C0) / 4; }
        }
        if (kind >= 0) {
            int nreg = rp2350 ? 6 : 4;
            if (idx >= 0 && idx < nreg) {
                switch (kind) {
                case 0: return gpio_state.intr[idx];
                case 1: return gpio_state.proc0_inte[idx];
                case 2: return gpio_state.proc0_intf[idx];
                case 3: return gpio_state.proc0_ints[idx];
                case 4: return gpio_state.proc1_inte[idx];
                case 5: return gpio_state.proc1_intf[idx];
                case 6: return gpio_state.proc1_ints[idx];
                }
            }
            return 0;
        }
    }

    /* IO_BANK0 registers (per-pin configuration; RP2350 has 48 pins so
     * its config extends to 0x180, still below the IRQ block at 0x230) */
    if (addr >= IO_BANK0_BASE &&
        addr < IO_BANK0_BASE + (membus_rp2350_mode ? 0x180u : 0xF0u)) {
        uint32_t offset = addr - IO_BANK0_BASE;
        uint32_t pin = offset / 8;  /* Each pin has 8 bytes (STATUS + CTRL) */
        uint32_t reg = offset % 8;

        if (pin < NUM_GPIO_PINS) {
            if (reg == GPIO_STATUS_OFFSET) {
                return gpio_state.pins[pin].status;
            } else if (reg == GPIO_CTRL_OFFSET) {
                return gpio_state.pins[pin].ctrl;
            }
        }
    }

    /* PADS_BANK0 registers with alias support */
    if ((addr >= PADS_BANK0_BASE && addr < PADS_BANK0_BASE + 0x80) ||
        (addr >= PADS_BANK0_BASE + REG_ALIAS_XOR_BITS && addr < PADS_BANK0_BASE + REG_ALIAS_XOR_BITS + 0x80) ||
        (addr >= PADS_BANK0_BASE + REG_ALIAS_SET_BITS && addr < PADS_BANK0_BASE + REG_ALIAS_SET_BITS + 0x80) ||
        (addr >= PADS_BANK0_BASE + REG_ALIAS_CLR_BITS && addr < PADS_BANK0_BASE + REG_ALIAS_CLR_BITS + 0x80)) {
        /* Strip alias offset to get base address */
        uint32_t base_addr = addr;
        
        if (addr >= PADS_BANK0_BASE + REG_ALIAS_CLR_BITS) {
            base_addr = addr - REG_ALIAS_CLR_BITS;
        } else if (addr >= PADS_BANK0_BASE + REG_ALIAS_SET_BITS) {
            base_addr = addr - REG_ALIAS_SET_BITS;
        } else if (addr >= PADS_BANK0_BASE + REG_ALIAS_XOR_BITS) {
            base_addr = addr - REG_ALIAS_XOR_BITS;
        }

        uint32_t offset = (base_addr - PADS_BANK0_BASE) / 4;
        
        if (offset > 0 && offset <= NUM_GPIO_PINS) {
            return gpio_state.pads[offset - 1];
        }
        if (offset == 0)
            return gpio_state.voltage_select;  /* L5: VOLTAGE_SELECT */
        /* Other pad registers */
        return 0x00000056;  /* Default pad config */
    }

    return 0x00000000;
}

/* Write to GPIO register space */
void gpio_write32(uint32_t addr, uint32_t val) {
    /* SIO GPIO registers (fast access with atomic operations) */
    if (addr >= SIO_BASE_GPIO && addr < SIO_BASE_GPIO + 0x100) {
        uint32_t old_pins = gpio_effective_pins();
        uint32_t old_hi = (gpio_state.gpio_out_hi & gpio_state.gpio_oe_hi) |
                          (gpio_state.gpio_in_hi & ~gpio_state.gpio_oe_hi);
        switch (addr) {
            case SIO_GPIO_OUT: {
                uint32_t old = gpio_state.gpio_out;
                gpio_state.gpio_out = val;
                gpio_trace_changes(old, val);
                break;
            }
            case SIO_GPIO_OUT_SET: {
                uint32_t old = gpio_state.gpio_out;
                gpio_state.gpio_out |= val;
                gpio_trace_changes(old, gpio_state.gpio_out);
                break;
            }
            case SIO_GPIO_OUT_CLR: {
                uint32_t old = gpio_state.gpio_out;
                gpio_state.gpio_out &= ~val;
                gpio_trace_changes(old, gpio_state.gpio_out);
                break;
            }
            case SIO_GPIO_OUT_XOR: {
                uint32_t old = gpio_state.gpio_out;
                gpio_state.gpio_out ^= val;
                gpio_trace_changes(old, gpio_state.gpio_out);
                break;
            }

            case SIO_GPIO_OE:
                gpio_state.gpio_oe = val;
                break;

            case SIO_GPIO_OE_SET:
                gpio_state.gpio_oe |= val;  /* Atomic set */
                break;

            case SIO_GPIO_OE_CLR:
                gpio_state.gpio_oe &= ~val;  /* Atomic clear */
                break;

            case SIO_GPIO_OE_XOR:
                gpio_state.gpio_oe ^= val;  /* Atomic toggle */
                break;

            /* GPIO_IN is read-only, writes ignored */
            case SIO_GPIO_IN:
                break;
            default: {
                /* RP2350 high pins 32-47 */
                if (addr >= SIO_BASE_GPIO + 0x30 && addr < SIO_BASE_GPIO + 0x50) {
                    uint32_t off = addr - (SIO_BASE_GPIO + 0x30);
                    uint32_t m = val & 0xFFFFu;
                    if (off == 0x00) gpio_state.gpio_out_hi = (gpio_state.gpio_out_hi & ~0xFFFFu) | m;
                    else if (off == 0x04) { /* HI_OUT_SET */
                        gpio_state.gpio_out_hi |= m;
                    } else if (off == 0x08) { /* HI_OUT_CLR */
                        gpio_state.gpio_out_hi &= ~m;
                    } else if (off == 0x0C) { /* HI_OUT_XOR */
                        gpio_state.gpio_out_hi ^= m;
                    } else if (off == 0x10) { /* HI_OE */
                        gpio_state.gpio_oe_hi = (gpio_state.gpio_oe_hi & ~0xFFFFu) | m;
                    } else if (off == 0x14) { /* HI_OE_SET */
                        gpio_state.gpio_oe_hi |= m;
                    } else if (off == 0x18) { /* HI_OE_CLR */
                        gpio_state.gpio_oe_hi &= ~m;
                    } else if (off == 0x1C) { /* HI_OE_XOR */
                        gpio_state.gpio_oe_hi ^= m;
                    }
                    /* edge detect for hi pins into intr[4..5] */
                    {
                        uint32_t new_hi = (gpio_state.gpio_out_hi & gpio_state.gpio_oe_hi) |
                                          (gpio_state.gpio_in_hi & ~gpio_state.gpio_oe_hi);
                        uint32_t chg = old_hi ^ new_hi;
                        while (chg) {
                            int b = __builtin_ctz(chg);
                            chg &= chg - 1;
                            int pin = 32 + b;
                            int reg = pin / 8;
                            int bit = pin % 8;
                            uint32_t shift = (uint32_t)bit * 4u;
                            if ((new_hi >> (uint32_t)b) & 1u)
                                gpio_state.intr[reg] |= (GPIO_INTR_EDGE_HIGH << shift);
                            else
                                gpio_state.intr[reg] |= (GPIO_INTR_EDGE_LOW << shift);
                            /* recompute level bits for this bank */
                            gpio_state.intr[reg] &= ~((0x3u) << shift);
                            if ((new_hi >> (uint32_t)b) & 1u)
                                gpio_state.intr[reg] |= (GPIO_INTR_LEVEL_HIGH << shift);
                            else
                                gpio_state.intr[reg] |= (GPIO_INTR_LEVEL_LOW << shift);
                        }
                        if (old_hi != new_hi) gpio_check_irq();
                    }
                }
                break;
            }
        }
        /* Detect edge/level events from pin value changes */
        gpio_detect_events(old_pins, gpio_effective_pins());
        return;
    }

    /* IO_BANK0 interrupt registers FIRST (C2 fix) */
    {
        uint32_t irq_alias = REG_ALIAS_RW_BITS;
        uint32_t base_addr = addr;
        if (addr >= IO_BANK0_BASE + REG_ALIAS_CLR_BITS &&
            addr <  IO_BANK0_BASE + REG_ALIAS_CLR_BITS + 0x400) {
            irq_alias = REG_ALIAS_CLR_BITS;
            base_addr -= REG_ALIAS_CLR_BITS;
        } else if (addr >= IO_BANK0_BASE + REG_ALIAS_SET_BITS &&
                   addr <  IO_BANK0_BASE + REG_ALIAS_SET_BITS + 0x400) {
            irq_alias = REG_ALIAS_SET_BITS;
            base_addr -= REG_ALIAS_SET_BITS;
        } else if (addr >= IO_BANK0_BASE + REG_ALIAS_XOR_BITS &&
                   addr <  IO_BANK0_BASE + REG_ALIAS_XOR_BITS + 0x400) {
            irq_alias = REG_ALIAS_XOR_BITS;
            base_addr -= REG_ALIAS_XOR_BITS;
        }

        /* Same arch-aware map as the read path (see gpio_read32). */
        uint32_t off = base_addr - IO_BANK0_BASE;
        int rp2350 = membus_rp2350_mode;
        int kind = -1, idx = 0;
        if (!rp2350) {
            if (off >= 0xF0 && off < 0x100)       { kind = 0; idx = (off - 0xF0) / 4; }
            else if (off >= 0x100 && off < 0x110) { kind = 1; idx = (off - 0x100) / 4; }
            else if (off >= 0x110 && off < 0x120) { kind = 2; idx = (off - 0x110) / 4; }
            else if (off >= 0x120 && off < 0x130) { kind = 3; idx = (off - 0x120) / 4; }
            else if (off >= 0x130 && off < 0x140) { kind = 4; idx = (off - 0x130) / 4; }
            else if (off >= 0x140 && off < 0x150) { kind = 5; idx = (off - 0x140) / 4; }
            else if (off >= 0x150 && off < 0x160) { kind = 6; idx = (off - 0x150) / 4; }
        } else {
            if (off >= 0x230 && off < 0x248)      { kind = 0; idx = (off - 0x230) / 4; }
            else if (off >= 0x248 && off < 0x260) { kind = 1; idx = (off - 0x248) / 4; }
            else if (off >= 0x260 && off < 0x278) { kind = 2; idx = (off - 0x260) / 4; }
            else if (off >= 0x278 && off < 0x290) { kind = 3; idx = (off - 0x278) / 4; }
            else if (off >= 0x290 && off < 0x2A8) { kind = 4; idx = (off - 0x290) / 4; }
            else if (off >= 0x2A8 && off < 0x2C0) { kind = 5; idx = (off - 0x2A8) / 4; }
            else if (off >= 0x2C0 && off < 0x2D8) { kind = 6; idx = (off - 0x2C0) / 4; }
        }
        if (kind >= 0) {
            int nreg = rp2350 ? 6 : 4;
            uint32_t *reg_ptr = NULL;
            if (idx >= 0 && idx < nreg) {
                if (kind == 1 && idx == 3)
                    fprintf(stderr, "[GPIO-TRACE] INTE3 write 0x%08X (alias=%u pc=0x%08X)\n",
                            val, irq_alias, cpu.r[15]);
                switch (kind) {
                case 0: /* INTR - W1C regardless of alias */
                    gpio_state.intr[idx] &= ~val;
                    break;
                case 1: reg_ptr = &gpio_state.proc0_inte[idx]; break;
                case 2: reg_ptr = &gpio_state.proc0_intf[idx]; break;
                case 4: reg_ptr = &gpio_state.proc1_inte[idx]; break;
                case 5: reg_ptr = &gpio_state.proc1_intf[idx]; break;
                default: break; /* INTS read-only */
                }
            }
            /* INTS is read-only */

            if (reg_ptr) {
                switch (irq_alias) {
                case REG_ALIAS_SET_BITS: *reg_ptr |= val;  break;
                case REG_ALIAS_CLR_BITS: *reg_ptr &= ~val; break;
                case REG_ALIAS_XOR_BITS: *reg_ptr ^= val;  break;
                default:                 *reg_ptr  = val;  break;
                }
            }
            gpio_check_irq();
            return;
        }
    }

    /* IO_BANK0 registers (per-pin configuration, only <0xF0) */
    if (addr >= IO_BANK0_BASE && addr < IO_BANK0_BASE + 0xF0) {
        uint32_t offset = addr - IO_BANK0_BASE;
        uint32_t pin = offset / 8;
        uint32_t reg = offset % 8;

        if (pin < NUM_GPIO_PINS) {
            if (reg == GPIO_STATUS_OFFSET) {
                /* STATUS is read-only on hardware; ignore writes */
                (void)val;
            } else if (reg == GPIO_CTRL_OFFSET) {
                /* Store full CTRL (FUNCSEL + overrides) */
                gpio_state.pins[pin].ctrl = val;
            }
        }
        return;
    }

    /* ===== CRITICAL FIX: PADS_BANK0 with Alias Support ===== */
    /* Handle all 4 alias regions: 0x0000, 0x1000, 0x2000, 0x3000 */
    if ((addr >= PADS_BANK0_BASE && addr < PADS_BANK0_BASE + 0x80) ||
        (addr >= PADS_BANK0_BASE + REG_ALIAS_XOR_BITS && addr < PADS_BANK0_BASE + REG_ALIAS_XOR_BITS + 0x80) ||
        (addr >= PADS_BANK0_BASE + REG_ALIAS_SET_BITS && addr < PADS_BANK0_BASE + REG_ALIAS_SET_BITS + 0x80) ||
        (addr >= PADS_BANK0_BASE + REG_ALIAS_CLR_BITS && addr < PADS_BANK0_BASE + REG_ALIAS_CLR_BITS + 0x80)) {
        /* Determine which alias region we're in */
        uint32_t alias_offset = REG_ALIAS_RW_BITS;  /* Default to normal access */
        uint32_t base_addr = addr;
        
        if (addr >= PADS_BANK0_BASE + REG_ALIAS_CLR_BITS) {
            alias_offset = REG_ALIAS_CLR_BITS;  /* 0x3000 - CLEAR */
            base_addr = addr - REG_ALIAS_CLR_BITS;
        } else if (addr >= PADS_BANK0_BASE + REG_ALIAS_SET_BITS) {
            alias_offset = REG_ALIAS_SET_BITS;  /* 0x2000 - SET */
            base_addr = addr - REG_ALIAS_SET_BITS;
        } else if (addr >= PADS_BANK0_BASE + REG_ALIAS_XOR_BITS) {
            alias_offset = REG_ALIAS_XOR_BITS;  /* 0x1000 - XOR */
            base_addr = addr - REG_ALIAS_XOR_BITS;
        }

        uint32_t offset = (base_addr - PADS_BANK0_BASE) / 4;
        
        if (offset > 0 && offset <= NUM_GPIO_PINS) {
            uint32_t pin_idx = offset - 1;
            
            /* Apply atomic operation based on alias */
            switch (alias_offset) {
                case REG_ALIAS_RW_BITS:  /* 0x0000 - Normal write */
                    gpio_state.pads[pin_idx] = val;
                    break;
                    
                case REG_ALIAS_XOR_BITS:  /* 0x1000 - XOR */
                    gpio_state.pads[pin_idx] ^= val;
                    break;
                    
                case REG_ALIAS_SET_BITS:  /* 0x2000 - SET */
                    gpio_state.pads[pin_idx] |= val;  /* Set bits where val=1 */
                    break;
                    
                case REG_ALIAS_CLR_BITS:  /* 0x3000 - CLEAR */
                    gpio_state.pads[pin_idx] &= ~val;  /* Clear bits where val=1 */
                    break;
            }
        } else if (offset == 0) {
            /* L5: VOLTAGE_SELECT register (bit0 only) with alias ops */
            switch (alias_offset) {
                case REG_ALIAS_RW_BITS: gpio_state.voltage_select = val & 0x1; break;
                case REG_ALIAS_XOR_BITS: gpio_state.voltage_select ^= (val & 0x1); break;
                case REG_ALIAS_SET_BITS: gpio_state.voltage_select |= (val & 0x1); break;
                case REG_ALIAS_CLR_BITS: gpio_state.voltage_select &= ~(val & 0x1); break;
            }
        }
        return;
    }
}

/* Helper functions for GPIO pin operations */

void gpio_set_pin(uint8_t pin, uint8_t value) {
    if (pin >= NUM_GPIO_PINS) return;
    if (pin < 32) {
        uint32_t old_pins = gpio_effective_pins();
        uint32_t mask = 1u << (uint32_t)pin;
        if (value) {
            gpio_state.gpio_out |= mask;
        } else {
            gpio_state.gpio_out &= ~mask;
        }
        gpio_detect_events(old_pins, gpio_effective_pins());
    } else {
        uint32_t b = (uint32_t)pin - 32u;
        uint32_t mask = 1u << b;
        uint32_t old_hi = (gpio_state.gpio_out_hi & gpio_state.gpio_oe_hi) |
                          (gpio_state.gpio_in_hi & ~gpio_state.gpio_oe_hi);
        if (value) gpio_state.gpio_out_hi |= mask;
        else gpio_state.gpio_out_hi &= ~mask;
        uint32_t new_hi = (gpio_state.gpio_out_hi & gpio_state.gpio_oe_hi) |
                          (gpio_state.gpio_in_hi & ~gpio_state.gpio_oe_hi);
        /* reuse hi edge logic via direct intr update */
        uint32_t chg = old_hi ^ new_hi;
        while (chg) {
            int bb = __builtin_ctz(chg);
            chg &= chg - 1;
            int p = 32 + bb;
            int reg = p / 8;
            int bit = p % 8;
            uint32_t shift = (uint32_t)bit * 4u;
            if ((new_hi >> (uint32_t)bb) & 1u)
                gpio_state.intr[reg] |= (GPIO_INTR_EDGE_HIGH << shift);
            else
                gpio_state.intr[reg] |= (GPIO_INTR_EDGE_LOW << shift);
        }
        gpio_check_irq();
    }
}

uint8_t gpio_get_pin(uint8_t pin) {
    if (pin >= NUM_GPIO_PINS) return 0;
    if (pin < 32) {
        /* If pin is output, return output value (C4: use 1u) */
        if (gpio_state.gpio_oe & (1u << (uint32_t)pin)) {
            return (uint8_t)((gpio_state.gpio_out >> (uint32_t)pin) & 1u);
        }
        /* Otherwise return input value */
        return (uint8_t)((gpio_state.gpio_in >> (uint32_t)pin) & 1u);
    } else {
        uint32_t b = (uint32_t)pin - 32u;
        if (gpio_state.gpio_oe_hi & (1u << b)) {
            return (uint8_t)((gpio_state.gpio_out_hi >> b) & 1u);
        }
        return (uint8_t)((gpio_state.gpio_in_hi >> b) & 1u);
    }
}

void gpio_set_input_pin(uint8_t pin, uint8_t value) {
    if (pin >= NUM_GPIO_PINS) return;
    if (pin < 32) {
        uint32_t old_pins = gpio_effective_pins();
        uint32_t mask = 1u << (uint32_t)pin;
        if (value) {
            gpio_state.gpio_in |= mask;
        } else {
            gpio_state.gpio_in &= ~mask;
        }
        gpio_detect_events(old_pins, gpio_effective_pins());
        /* Emulator-driven input change (e.g. CYW43 HOST_WAKE) must
         * signal NVIC like guest-driven changes do, or level/edge
         * IRQs (IO_BANK0) never fire. */
        gpio_check_irq();
    } else {
        uint32_t b = (uint32_t)pin - 32u;
        uint32_t mask = 1u << b;
        uint32_t old_hi = (gpio_state.gpio_out_hi & gpio_state.gpio_oe_hi) |
                          (gpio_state.gpio_in_hi & ~gpio_state.gpio_oe_hi);
        if (value) gpio_state.gpio_in_hi |= mask;
        else gpio_state.gpio_in_hi &= ~mask;
        uint32_t new_hi = (gpio_state.gpio_out_hi & gpio_state.gpio_oe_hi) |
                          (gpio_state.gpio_in_hi & ~gpio_state.gpio_oe_hi);
        uint32_t chg = old_hi ^ new_hi;
        while (chg) {
            int bb = __builtin_ctz(chg);
            chg &= chg - 1;
            int p = 32 + bb;
            int reg = p / 8;
            int bit = p % 8;
            uint32_t shift = (uint32_t)bit * 4u;
            if ((new_hi >> (uint32_t)bb) & 1u)
                gpio_state.intr[reg] |= (GPIO_INTR_EDGE_HIGH << shift);
            else
                gpio_state.intr[reg] |= (GPIO_INTR_EDGE_LOW << shift);
        }
        gpio_check_irq();
    }
}

void gpio_set_direction(uint8_t pin, uint8_t output) {
    if (pin >= NUM_GPIO_PINS) return;
    if (pin < 32) {
        uint32_t mask = 1u << (uint32_t)pin;
        if (output) {
            gpio_state.gpio_oe |= mask;
        } else {
            gpio_state.gpio_oe &= ~mask;
        }
    } else {
        uint32_t mask = 1u << ((uint32_t)pin - 32u);
        if (output) {
            gpio_state.gpio_oe_hi |= mask;
        } else {
            gpio_state.gpio_oe_hi &= ~mask;
        }
    }
}

void gpio_set_function(uint8_t pin, uint8_t func) {
    if (pin >= NUM_GPIO_PINS) return;

    gpio_state.pins[pin].ctrl = (gpio_state.pins[pin].ctrl & ~0x1F) | (func & 0x1F);
}
