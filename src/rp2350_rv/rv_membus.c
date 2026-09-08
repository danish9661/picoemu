/*
 * RP2350 Memory Bus for RISC-V Hazard3
 *
 * Routes memory accesses for the RISC-V execution path.
 * RP2350-specific regions (520KB SRAM, CLINT, RP2350 peripherals, SIO)
 * are handled here. Shared peripherals fall through to the RP2040 membus.
 */

#include <string.h>
#include <stdio.h>
#include "rp2350_rv/rv_membus.h"
#include "rp2350_rv/rp2350_memmap.h"
#include "clocks.h"
#include "emulator.h"
#include "i2c.h"
#include "pwm.h"

#define RV_SHARED_RP2040_SYSCFG_BASE     0x40004000u
#define RV_SHARED_RP2040_CLOCKS_BASE     0x40008000u
#define RV_SHARED_RP2040_PSM_BASE        0x40010000u
#define RV_SHARED_RP2040_RESETS_BASE     0x4000C000u
#define RV_SHARED_RP2040_IO_BANK0_BASE   0x40014000u
#define RV_SHARED_RP2040_IO_QSPI_BASE    0x40018000u
#define RV_SHARED_RP2040_PADS_BANK0_BASE 0x4001C000u
#define RV_SHARED_RP2040_PADS_QSPI_BASE  0x40020000u
#define RV_SHARED_RP2040_XOSC_BASE       0x40024000u
#define RV_SHARED_RP2040_PLL_SYS_BASE    0x40028000u
#define RV_SHARED_RP2040_PLL_USB_BASE    0x4002C000u
#define RV_SHARED_RP2040_BUSCTRL_BASE    0x40030000u
#define RV_SHARED_RP2040_UART0_BASE      0x40034000u
#define RV_SHARED_RP2040_UART1_BASE      0x40038000u
#define RV_SHARED_RP2040_SPI0_BASE       0x4003C000u
#define RV_SHARED_RP2040_SPI1_BASE       0x40040000u
#define RV_SHARED_RP2040_I2C0_BASE       0x40044000u
#define RV_SHARED_RP2040_I2C1_BASE       0x40048000u
#define RV_SHARED_RP2040_ADC_BASE        0x4004C000u
#define RV_SHARED_RP2040_PWM_BASE        0x40050000u
#define RV_SHARED_RP2040_TIMER0_BASE     0x40054000u
#define RV_SHARED_RP2040_WATCHDOG_BASE   0x40058000u
#define RV_SHARED_RP2040_ROSC_BASE       0x40060000u
#define RV_SHARED_RP2040_TBMAN_BASE      0x4006C000u

static uint32_t rv_translate_shared_addr(uint32_t addr) {
    uint32_t block = addr & ~0x3FFFu;
    uint32_t tail = addr & 0x3FFFu;

    switch (block) {
    case RP2350_SYSCFG_BASE:      return RV_SHARED_RP2040_SYSCFG_BASE | tail;
    case RP2350_CLOCKS_BASE:      return RV_SHARED_RP2040_CLOCKS_BASE | tail;
    case RP2350_PSM_BASE:         return RV_SHARED_RP2040_PSM_BASE | tail;
    case RP2350_RESETS_BASE:      return RV_SHARED_RP2040_RESETS_BASE | tail;
    case RP2350_IO_BANK0_BASE:    return RV_SHARED_RP2040_IO_BANK0_BASE | tail;
    case RP2350_IO_QSPI_BASE:     return RV_SHARED_RP2040_IO_QSPI_BASE | tail;
    case RP2350_PADS_BANK0_BASE:  return RV_SHARED_RP2040_PADS_BANK0_BASE | tail;
    case RP2350_PADS_QSPI_BASE:   return RV_SHARED_RP2040_PADS_QSPI_BASE | tail;
    case RP2350_XOSC_BASE:        return RV_SHARED_RP2040_XOSC_BASE | tail;
    case RP2350_PLL_SYS_BASE:     return RV_SHARED_RP2040_PLL_SYS_BASE | tail;
    case RP2350_PLL_USB_BASE:     return RV_SHARED_RP2040_PLL_USB_BASE | tail;
    case RP2350_BUSCTRL_BASE:     return RV_SHARED_RP2040_BUSCTRL_BASE | tail;
    case RP2350_UART0_BASE:       return RV_SHARED_RP2040_UART0_BASE | tail;
    case RP2350_UART1_BASE:       return RV_SHARED_RP2040_UART1_BASE | tail;
    case RP2350_SPI0_BASE:        return RV_SHARED_RP2040_SPI0_BASE | tail;
    case RP2350_SPI1_BASE:        return RV_SHARED_RP2040_SPI1_BASE | tail;
    case RP2350_I2C0_BASE:        return RV_SHARED_RP2040_I2C0_BASE | tail;
    case RP2350_I2C1_BASE:        return RV_SHARED_RP2040_I2C1_BASE | tail;
    case RP2350_ADC_BASE:         return RV_SHARED_RP2040_ADC_BASE | tail;
    case RP2350_PWM_BASE:         return RV_SHARED_RP2040_PWM_BASE | tail;
    case RP2350_TIMER0_BASE:      return RV_SHARED_RP2040_TIMER0_BASE | tail;
    case RP2350_WATCHDOG_BASE:    return RV_SHARED_RP2040_WATCHDOG_BASE | tail;
    case RP2350_ROSC_BASE:        return RV_SHARED_RP2040_ROSC_BASE | tail;
    case RP2350_TBMAN_BASE:       return RV_SHARED_RP2040_TBMAN_BASE | tail;
    default:
        return addr;
    }
}

/* ========================================================================
 * Initialization
 * ======================================================================== */

void rv_membus_init(rv_membus_state_t *bus, uint8_t *flash, uint32_t flash_size,
                    uint32_t cycles_per_us) {
    memset(bus->sram, 0, RV_SRAM_SIZE);
    memset(bus->rom, 0, sizeof(bus->rom));
    bus->flash = flash;
    bus->flash_size = flash_size;
    bus->rom_size = 32 * 1024;
    bus->is_riscv = 1;
    bus->hart1_launch_pending = 0;
    bus->gpio_hi_in = 0x3E;  /* CS high + data pulled up (same as RP2040 QSPI) */
    rv_clint_init(&bus->clint, cycles_per_us);
    rp2350_periph_init(&bus->periph, 0);
}

/* ========================================================================
 * Hart 1 Launch Check
 * ======================================================================== */

int rv_membus_check_hart1_launch(rv_membus_state_t *bus, uint32_t *entry,
                                  uint32_t *sp, uint32_t *arg) {
    if (!bus->hart1_launch_pending) return 0;
    bus->hart1_launch_pending = 0;
    *entry = bus->hart1_entry;
    *sp = bus->hart1_sp;
    *arg = bus->hart1_arg;
    return 1;
}

/* ========================================================================
 * RP2350 SIO Handler (0xD0000000)
 * Different from RP2040 SIO: CPUID returns RP2350, hart launch mailbox,
 * GPIO_HI for pins 32-47, additional spinlocks (0-31 instead of 0-31).
 * ======================================================================== */

static uint32_t rv_sio_read(rv_membus_state_t *bus, uint32_t offset) {
    switch (offset) {
    case RV_SIO_CPUID:
        /* RP2350 CPUID: differs from RP2040 */
        return 0x00000002;  /* RP2350 identifier */

    /* GPIO low (pins 0-31) — fall through to RP2040 */
    case 0x04: /* GPIO_IN */
    case 0x08: /* GPIO_HI_IN */
    case 0x10: /* GPIO_OUT */
    case 0x20: /* GPIO_OE */
        break;  /* Will fall through */

    /* GPIO high (pins 32-47) — RP2350-specific */
    case 0x30: return bus->gpio_hi_out;   /* GPIO_HI_OUT */
    case 0x40: return bus->gpio_hi_oe;    /* GPIO_HI_OE */
    case 0x38: return bus->gpio_hi_in;    /* GPIO_HI_IN (same offset as RP2040 QSPI in) */

    /* Hart 1 launch mailbox */
    case RV_SIO_HART1_BOOT_ENTRY:  return bus->hart1_entry;
    case RV_SIO_HART1_BOOT_SP:     return bus->hart1_sp;
    case RV_SIO_HART1_BOOT_ARG:    return bus->hart1_arg;
    case RV_SIO_HART1_BOOT_LAUNCH: return 0;

    default: break;
    }
    /* Fall through for standard SIO registers (L8: return 0 for unhandled
     * offsets — callers only use this value for known offsets and fall
     * through to the shared SIO bus otherwise). */
    return 0;
}

static int rv_sio_write(rv_membus_state_t *bus, uint32_t offset, uint32_t val) {
    switch (offset) {
    /* GPIO high (pins 32-47) */
    case 0x30: bus->gpio_hi_out = val & 0xFFFF; return 1;  /* GPIO_HI_OUT */
    case 0x34: bus->gpio_hi_out |= (val & 0xFFFF); return 1; /* GPIO_HI_OUT_SET */
    case 0x38: bus->gpio_hi_out &= ~(val & 0xFFFF); return 1; /* GPIO_HI_OUT_CLR */
    case 0x3C: bus->gpio_hi_out ^= (val & 0xFFFF); return 1; /* GPIO_HI_OUT_XOR */
    case 0x40: bus->gpio_hi_oe = val & 0xFFFF; return 1;    /* GPIO_HI_OE */
    case 0x44: bus->gpio_hi_oe |= (val & 0xFFFF); return 1; /* GPIO_HI_OE_SET */
    case 0x48: bus->gpio_hi_oe &= ~(val & 0xFFFF); return 1; /* GPIO_HI_OE_CLR */
    case 0x4C: bus->gpio_hi_oe ^= (val & 0xFFFF); return 1; /* GPIO_HI_OE_XOR */

    /* Hart 1 launch mailbox */
    case RV_SIO_HART1_BOOT_ENTRY: bus->hart1_entry = val; return 1;
    case RV_SIO_HART1_BOOT_SP:    bus->hart1_sp = val; return 1;
    case RV_SIO_HART1_BOOT_ARG:   bus->hart1_arg = val; return 1;
    case RV_SIO_HART1_BOOT_LAUNCH:
        if (val & 1) {
            bus->hart1_launch_pending = 1;
            fprintf(stderr, "[RV-SIO] Hart 1 launch: entry=0x%08X SP=0x%08X arg=0x%08X\n",
                    bus->hart1_entry, bus->hart1_sp, bus->hart1_arg);
        }
        return 1;

    default: break;
    }
    return 0;  /* Not handled — fall through to RP2040 SIO */
}

/* ========================================================================
 * 32-bit Access
 * ======================================================================== */

/* M7: any XIP alias in [FLASH_BASE, XIP_SRAM_BASE) reads the same flash;
 * addresses past the mounted size mirror (hardware repeats the image). */
static int rv_xip_offset(rv_membus_state_t *bus, uint32_t addr, uint32_t *off) {
    if (addr < RP2350_FLASH_BASE || addr >= RP2350_XIP_SRAM_BASE ||
        bus->flash_size == 0)
        return 0;
    uint32_t o = addr - RP2350_FLASH_BASE;
    if (o >= bus->flash_size)
        o %= bus->flash_size;
    *off = o;
    return 1;
}

uint32_t rv_mem_read32(rv_membus_state_t *bus, uint32_t addr) {
    uint32_t val;

    /* SRAM: 0x20000000 - 0x20082000 (520KB) */
    if (addr >= RP2350_SRAM_BASE && addr < RP2350_SRAM_END) {
        memcpy(&val, &bus->sram[addr - RP2350_SRAM_BASE], 4);
        return val;
    }

    /* SRAM alias: 0x21000000 */
    if (addr >= RP2350_SRAM_ALIAS_BASE && addr < RP2350_SRAM_ALIAS_BASE + RV_SRAM_SIZE) {
        memcpy(&val, &bus->sram[addr - RP2350_SRAM_ALIAS_BASE], 4);
        return val;
    }

    /* ROM: 0x00000000 - 0x00007FFF (32KB) */
    if (addr < bus->rom_size) {
        memcpy(&val, &bus->rom[addr], 4);
        return val;
    }

    /* Flash: 0x10000000+ (all XIP aliases, M7) */
    {
        uint32_t off;
        if (rv_xip_offset(bus, addr, &off)) {
            memcpy(&val, &bus->flash[off], 4);
            return val;
        }
    }

    /* CLINT registers (in SIO space) */
    if (rv_clint_match(addr))
        return rv_clint_read(&bus->clint, addr - RV_CLINT_BASE);

    /* PSM (0x40018000, 16KB with aliases): route straight to the shared
     * PSM logic. Must NOT use the shared-bus translation: RP2040 PSM
     * (0x40010000) collides with RP2350 CLOCKS on the ARM bus, so the
     * translated FRCE writes land in the clocks domain and core1 reset
     * never fires (RV littleOS hung in multicore_reset_core1's pop). */
    if ((addr & ~0x3FFFu) == RP2350_PSM_BASE)
        return psm_read(addr);

    /* Shadowed-translation bypasses: these RP2350 blocks translate to
     * RP2040 targets that alias DIFFERENT RP2350 natives, which the
     * shared bus (in RP2350 mode) claims first:
     *   IO_QSPI  -> 0x40018000 (= RP2350 PSM, clocks handler wins)
     *   PADS_QSPI-> 0x40020000 (= RP2350 RESETS, clocks handler wins)
     *   I2C1     -> 0x40048000 (= RP2350 XOSC, clocks handler wins)
     *   PWM      -> 0x40050000 (= RP2350 PLL_SYS, clocks handler wins)
     *   WATCHDOG -> 0x40058000 (= RP2350 PLL_USB, clocks handler wins)
     * Route straight to the RP2040-semantics handlers (PSM pattern). */
    if ((addr & ~0x3FFFu) == RP2350_IO_QSPI_BASE)
        return io_qspi_read(addr & 0xFFFu);
    if ((addr & ~0x3FFFu) == RP2350_PADS_QSPI_BASE)
        return pads_qspi_read(addr & 0xFFFu);
    if ((addr & ~0x3FFFu) == RP2350_I2C1_BASE)
        return i2c_read32(1, addr & 0xFFFu);
    if ((addr & ~0x3FFFu) == RP2350_PWM_BASE)
        return pwm_read32(addr & 0xFFFu);
    if ((addr & ~0x3FFFu) == RP2350_WATCHDOG_BASE)
        return watchdog_read(RV_SHARED_RP2040_WATCHDOG_BASE | (addr & 0xFFFu));

    /* RP2350-specific peripherals */
    if (rp2350_periph_match(addr))
        return rp2350_periph_read32(&bus->periph, addr);

    /* SIO: handle RP2350-specific registers, fall through for standard */
    if (addr >= RP2350_SIO_BASE && addr < RP2350_SIO_BASE + 0x200) {
        uint32_t offset = addr - RP2350_SIO_BASE;
        uint32_t sio_val = rv_sio_read(bus, offset);
        /* If CPUID or GPIO_HI or hart launch, return directly */
        if (offset == RV_SIO_CPUID || offset == 0x30 || offset == 0x38 ||
            offset == 0x40 ||
            (offset >= RV_SIO_HART1_BOOT_ENTRY && offset <= RV_SIO_HART1_BOOT_LAUNCH))
            return sio_val;
        /* Otherwise fall through to RP2040 SIO */
    }

    /* Fall through to shared RP2040 peripheral bus */
    return mem_read32(rv_translate_shared_addr(addr));
}

void rv_mem_write32(rv_membus_state_t *bus, uint32_t addr, uint32_t val) {
    /* SRAM */
    if (addr >= RP2350_SRAM_BASE && addr < RP2350_SRAM_END) {
        memcpy(&bus->sram[addr - RP2350_SRAM_BASE], &val, 4);
        return;
    }
    if (addr >= RP2350_SRAM_ALIAS_BASE && addr < RP2350_SRAM_ALIAS_BASE + RV_SRAM_SIZE) {
        memcpy(&bus->sram[addr - RP2350_SRAM_ALIAS_BASE], &val, 4);
        return;
    }

    /* ROM and flash are read-only (flash: whole XIP window, M7) */
    if (addr < bus->rom_size) return;
    if (addr >= RP2350_FLASH_BASE && addr < RP2350_XIP_SRAM_BASE) return;

    /* CLINT */
    if (rv_clint_match(addr)) {
        rv_clint_write(&bus->clint, addr - RV_CLINT_BASE, val);
        return;
    }

    /* PSM: direct to shared logic (see read path for why translation
     * cannot be used here). */
    if ((addr & ~0x3FFFu) == RP2350_PSM_BASE) {
        psm_write(addr, val, (addr >> 12) & 3);
        return;
    }

    /* Shadowed-translation bypasses (see read path). Atomic aliases
     * (+0x1000 XOR/+0x2000 SET/+0x3000 CLR) mirror the shared bus. */
    if ((addr & ~0x3FFFu) == RP2350_IO_QSPI_BASE) {
        uint32_t alias = addr & 0x3000u, off = addr & 0xFFFu;
        if (alias == 0x0000) io_qspi_write(off, val);
        else if (alias == 0x2000) io_qspi_write(off, io_qspi_read(off) | val);
        else if (alias == 0x3000) io_qspi_write(off, io_qspi_read(off) & ~val);
        else io_qspi_write(off, io_qspi_read(off) ^ val);
        return;
    }
    if ((addr & ~0x3FFFu) == RP2350_PADS_QSPI_BASE) {
        uint32_t alias = addr & 0x3000u, off = addr & 0xFFFu;
        if (alias == 0x0000) pads_qspi_write(off, val);
        else if (alias == 0x2000) pads_qspi_write(off, pads_qspi_read(off) | val);
        else if (alias == 0x3000) pads_qspi_write(off, pads_qspi_read(off) & ~val);
        else pads_qspi_write(off, pads_qspi_read(off) ^ val);
        return;
    }
    if ((addr & ~0x3FFFu) == RP2350_I2C1_BASE) {
        uint32_t alias = addr & 0x3000u, off = addr & 0xFFFu;
        if (alias == 0x0000) i2c_write32(1, off, val);
        else if (alias == 0x2000) i2c_write32(1, off, i2c_read32(1, off) | val);
        else if (alias == 0x3000) i2c_write32(1, off, i2c_read32(1, off) & ~val);
        else i2c_write32(1, off, i2c_read32(1, off) ^ val);
        return;
    }
    if ((addr & ~0x3FFFu) == RP2350_PWM_BASE) {
        uint32_t alias = addr & 0x3000u, off = addr & 0xFFFu;
        if (alias == 0x0000) pwm_write32(off, val);
        else if (alias == 0x2000) pwm_write32(off, pwm_read32(off) | val);
        else if (alias == 0x3000) pwm_write32(off, pwm_read32(off) & ~val);
        else pwm_write32(off, pwm_read32(off) ^ val);
        return;
    }
    if ((addr & ~0x3FFFu) == RP2350_WATCHDOG_BASE) {
        watchdog_write(RV_SHARED_RP2040_WATCHDOG_BASE | (addr & 0xFFFu),
                       val, (addr >> 12) & 3);
        return;
    }

    /* RP2350-specific peripherals */
    if (rp2350_periph_match(addr)) {
        rp2350_periph_write32(&bus->periph, addr, val);
        return;
    }

    /* SIO: try RP2350-specific first */
    if (addr >= RP2350_SIO_BASE && addr < RP2350_SIO_BASE + 0x200) {
        uint32_t offset = addr - RP2350_SIO_BASE;
        if (rv_sio_write(bus, offset, val))
            return;
        /* Fall through to RP2040 SIO */
    }

    /* Fall through to shared peripheral bus */
    mem_write32(rv_translate_shared_addr(addr), val);
}

/* ========================================================================
 * 16-bit Access
 * ======================================================================== */

uint16_t rv_mem_read16(rv_membus_state_t *bus, uint32_t addr) {
    uint16_t val;
    if (addr >= RP2350_SRAM_BASE && addr < RP2350_SRAM_END) {
        memcpy(&val, &bus->sram[addr - RP2350_SRAM_BASE], 2);
        return val;
    }
    if (addr >= RP2350_SRAM_ALIAS_BASE && addr < RP2350_SRAM_ALIAS_BASE + RV_SRAM_SIZE) {
        memcpy(&val, &bus->sram[addr - RP2350_SRAM_ALIAS_BASE], 2);
        return val;
    }
    if (addr < bus->rom_size) {
        memcpy(&val, &bus->rom[addr], 2);
        return val;
    }
    {
        uint32_t off;
        if (rv_xip_offset(bus, addr, &off)) {
            memcpy(&val, &bus->flash[off], 2);
            return val;
        }
    }
    /* M6: 16-bit CLINT/SIO access composes via the 32-bit path so halfword
     * firmware accesses see the same registers as word accesses. */
    if (rv_clint_match(addr & ~3u) ||
        (addr >= RP2350_SIO_BASE && addr < RP2350_SIO_BASE + 0x200)) {
        uint32_t w = rv_mem_read32(bus, addr & ~3u);
        return (uint16_t)((w >> ((addr & 2) * 8)) & 0xFFFF);
    }
    return mem_read16(rv_translate_shared_addr(addr));
}

void rv_mem_write16(rv_membus_state_t *bus, uint32_t addr, uint16_t val) {
    if (addr >= RP2350_SRAM_BASE && addr < RP2350_SRAM_END) {
        memcpy(&bus->sram[addr - RP2350_SRAM_BASE], &val, 2);
        return;
    }
    if (addr >= RP2350_SRAM_ALIAS_BASE && addr < RP2350_SRAM_ALIAS_BASE + RV_SRAM_SIZE) {
        memcpy(&bus->sram[addr - RP2350_SRAM_ALIAS_BASE], &val, 2);
        return;
    }
    if (addr < bus->rom_size) return;
    if (addr >= RP2350_FLASH_BASE && addr < RP2350_XIP_SRAM_BASE) return;
    /* M6: 16-bit CLINT/SIO stores are read-modify-write through the 32-bit
     * path (same rule as the ARM H7 subword fix). */
    if (rv_clint_match(addr & ~3u) ||
        (addr >= RP2350_SIO_BASE && addr < RP2350_SIO_BASE + 0x200)) {
        uint32_t base = addr & ~3u;
        uint32_t shift = (addr & 2) * 8;
        uint32_t w = rv_mem_read32(bus, base);
        w = (w & ~(0xFFFFu << shift)) | ((uint32_t)val << shift);
        rv_mem_write32(bus, base, w);
        return;
    }
    mem_write16(rv_translate_shared_addr(addr), val);
}

/* ========================================================================
 * 8-bit Access
 * ======================================================================== */

uint8_t rv_mem_read8(rv_membus_state_t *bus, uint32_t addr) {
    if (addr >= RP2350_SRAM_BASE && addr < RP2350_SRAM_END)
        return bus->sram[addr - RP2350_SRAM_BASE];
    if (addr >= RP2350_SRAM_ALIAS_BASE && addr < RP2350_SRAM_ALIAS_BASE + RV_SRAM_SIZE)
        return bus->sram[addr - RP2350_SRAM_ALIAS_BASE];
    if (addr < bus->rom_size)
        return bus->rom[addr];
    {
        uint32_t off;
        if (rv_xip_offset(bus, addr, &off))
            return bus->flash[off];
    }
    /* RP2350 peripherals byte access */
    if (rp2350_periph_match(addr))
        return rp2350_periph_read8(&bus->periph, addr);
    /* M6: byte CLINT/SIO access composes via the 32-bit path. */
    if (rv_clint_match(addr & ~3u) ||
        (addr >= RP2350_SIO_BASE && addr < RP2350_SIO_BASE + 0x200)) {
        uint32_t w = rv_mem_read32(bus, addr & ~3u);
        return (uint8_t)((w >> ((addr & 3) * 8)) & 0xFF);
    }
    return mem_read8(rv_translate_shared_addr(addr));
}

void rv_mem_write8(rv_membus_state_t *bus, uint32_t addr, uint8_t val) {
    if (addr >= RP2350_SRAM_BASE && addr < RP2350_SRAM_END) {
        bus->sram[addr - RP2350_SRAM_BASE] = val;
        return;
    }
    if (addr >= RP2350_SRAM_ALIAS_BASE && addr < RP2350_SRAM_ALIAS_BASE + RV_SRAM_SIZE) {
        bus->sram[addr - RP2350_SRAM_ALIAS_BASE] = val;
        return;
    }
    if (addr < bus->rom_size) return;
    if (addr >= RP2350_FLASH_BASE && addr < RP2350_XIP_SRAM_BASE) return;
    /* RP2350 peripherals byte access */
    if (rp2350_periph_match(addr)) {
        rp2350_periph_write8(&bus->periph, addr, val);
        return;
    }
    /* M6: byte CLINT/SIO stores are read-modify-write through the 32-bit path. */
    if (rv_clint_match(addr & ~3u) ||
        (addr >= RP2350_SIO_BASE && addr < RP2350_SIO_BASE + 0x200)) {
        uint32_t base = addr & ~3u;
        uint32_t shift = (addr & 3) * 8;
        uint32_t w = rv_mem_read32(bus, base);
        w = (w & ~(0xFFu << shift)) | ((uint32_t)val << shift);
        rv_mem_write32(bus, base, w);
        return;
    }
    mem_write8(rv_translate_shared_addr(addr), val);
}
