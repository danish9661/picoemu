#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <emscripten.h>
#include "emulator.h"
#include "gpio.h"
#include "timer.h"
#include "nvic.h"
#include "clocks.h"
#include "adc.h"
#include "rom.h"
#include "uart.h"
#include "spi.h"
#include "i2c.h"
#include "pwm.h"
#include "dma.h"
#include "pio.h"
#include "usb.h"
#include "rtc.h"
#include "storage.h"
#include "sdcard.h"
#include "emmc.h"
#include "w5500.h"
#include "vnet.h"
#include "sdd.h"
#include "devtools.h"
#include "gdb.h"
#include "netbridge.h"
#include "wire.h"
#include "cyw43.h"
#include "clocks.h"
#include "rp2350_rv/rv_cpu.h"
#include "rp2350_rv/rv_clint.h"
#include "rp2350_rv/rv_membus.h"
#include "rp2350_rv/rv_bootrom.h"
#include "rp2350_rv/rp2350_periph.h"
#include "rp2350_rv/rv_icache.h"
#include "rp2350_rv/rp2350_memmap.h"
#include "rp2350_rv/picobin.h"
#include "rp2350_arm/m33_cpu.h"

/* Cooperative corepool + net shims implemented in wasm_net.c */
extern int wasm_corepool_num_cores(void);
extern void wasm_corepool_set_num_cores(int n);
extern void corepool_set_step_quantum(int q);
extern int bramble_net_pop_rx(uint8_t *out, int maxlen);
extern void bramble_wire_push_rx(const uint8_t *data, int len);

/* UF2 block structure (copied from uf2.c) */
typedef struct {
    uint32_t magic_start0;
    uint32_t magic_start1;
    uint32_t flags;
    uint32_t target_addr;
    uint32_t payload_size;
    uint32_t block_no;
    uint32_t num_blocks;
    uint32_t file_size;
    uint8_t  data[476];
    uint32_t magic_end;
} __attribute__((packed)) uf2_block_t;

#define UF2_MAGIC_START0 0x0A324655
#define UF2_MAGIC_START1 0x9E5D5157
#define UF2_MAGIC_END    0x0AB16F30

typedef enum { ARCH_M0PLUS, ARCH_RV32, ARCH_M33 } arch_t;

static rv_cpu_state_t rv_cores[2];
static rv_membus_state_t rv_bus;
static rv_icache_t rv_icache;
static int current_arch = 1;

/* WASM device instances (defined early for bramble_step polling) */
static sdcard_t wasm_sdcard;
static int wasm_sdcard_on = 0;
static emmc_t wasm_emmc;
static int wasm_emmc_on = 0;
static w5500_t wasm_w5500;
static int wasm_w5500_on = 0;
static int wasm_vnet_on = 0;

/* UART TX buffer: firmware → browser */
#define UART_TX_BUF_SIZE 4096
static uint8_t uart_tx_buf[UART_TX_BUF_SIZE];
static int uart_tx_head = 0;
static int uart_tx_tail = 0;

/* UART RX buffer: browser → firmware */
static uint8_t uart_rx_buf[256];
static int uart_rx_head = 0;
static int uart_rx_tail = 0;

static void reset_runtime_peripherals(void) {
    gpio_init();
    timer_init();
    nvic_init();
    rom_init();
    uart_init();
    spi_init();
    i2c_init();
    pwm_init();
    dma_init();
    pio_init();
    clocks_init();
    adc_init();
    usb_init();
    rtc_init();
}

/* Override putchar to capture UART TX output from emulator */
int __attribute__((used)) putchar(int c) {
    uint8_t ch = (uint8_t)c;
    int next = (uart_tx_head + 1) % UART_TX_BUF_SIZE;
    if (next != uart_tx_tail) {
        uart_tx_buf[uart_tx_head] = ch;
        uart_tx_head = next;
    }
    return c;
}

int bramble_init(int arch) {
    current_arch = arch;
    cpu_init();
    memset(cpu.flash, 0xFF, FLASH_SIZE_MAX);
    timing_set_clock_mhz(1);
    reset_runtime_peripherals();
    dual_core_init();

    memset(&rv_cores[0], 0, sizeof(rv_cpu_state_t));
    memset(&rv_cores[1], 0, sizeof(rv_cpu_state_t));

    if (arch == ARCH_RV32) {
        membus_rp2350_mode = 1;
        rv_membus_init(&rv_bus, cpu.flash, FLASH_SIZE_MAX, timing_config.cycles_per_us);
        rv_bootrom_init(rv_bus.rom, rv_bus.rom_size, RP2350_FLASH_BASE, RP2350_SRAM_END);
        rv_icache_init(&rv_icache);
        rv_cpu_init(&rv_cores[0], 0);
        rv_cpu_init(&rv_cores[1], 1);
        rv_cores[0].bus = &rv_bus;
        rv_cores[1].bus = &rv_bus;
        rv_cores[0].icache = &rv_icache;
        rv_cores[1].icache = &rv_icache;
        gdb_is_riscv = 1;
        gdb_rv_harts[0] = &rv_cores[0];
        gdb_rv_harts[1] = &rv_cores[1];
    } else {
        membus_rp2350_mode = (arch == ARCH_M33) ? 1 : 0;
        gdb_is_riscv = 0;
        /* M33 overlay (mirrors main.c): without the 520KB SRAM, RP2350
         * peripherals and ROM patch, RP2350 firmware dies instantly with
         * zero output (SP=0x20082000 is outside the default 264KB). */
        if (arch == ARCH_M33) {
            m33_init_overlay();
            rom_patch_rp2350_arm();
            static uint8_t wasm_m33_sram[520 * 1024];
            memset(wasm_m33_sram, 0, sizeof(wasm_m33_sram));
            rp2350_sram_ptr = wasm_m33_sram;
            mem_set_ram_ptr(wasm_m33_sram, 0x20000000, 520 * 1024);
            static rp2350_periph_state_t wasm_m33_periph;
            rp2350_periph_init(&wasm_m33_periph, 1);
            membus_rp2350_periph = &wasm_m33_periph;
        }
    }

    uart_tx_head = 0;
    uart_tx_tail = 0;
    uart_rx_head = 0;
    uart_rx_tail = 0;

    return 1;
}

int bramble_load_uf2(const uint8_t *data, int len) {
    FILE *f = fopen("/tmp/fw.uf2", "wb");
    if (!f) return 0;
    fwrite(data, 1, len, f);
    fclose(f);
    int ret = load_uf2("/tmp/fw.uf2");
    // load_uf2 sets detected_arch and flash; sync membus mode if RP2350
    if (loader_detected_arch() == FW_ARCH_RV32) membus_rp2350_mode = 1;
    return ret;
}

int bramble_load_elf(const uint8_t *data, int len) {
    FILE *f = fopen("/tmp/fw.elf", "wb");
    if (!f) return 0;
    fwrite(data, 1, len, f);
    fclose(f);
    return load_elf("/tmp/fw.elf");
}

void bramble_reset(void) {
    if (current_arch == ARCH_RV32) {
        picobin_info_t pbi = picobin_scan(cpu.flash, 4096);
        if (pbi.found && pbi.entry_pc != 0) {
            rv_cpu_reset(&rv_cores[0], pbi.entry_pc);
            if (pbi.entry_sp != 0) rv_cores[0].x[2] = pbi.entry_sp;
        } else {
            rv_cpu_reset(&rv_cores[0], 0x00000000);
        }
        rv_cpu_reset(&rv_cores[1], 0x00000000);
        rv_cores[1].is_halted = 1;
        rv_cores[0].bus = &rv_bus;
        rv_cores[1].bus = &rv_bus;
        rv_cores[0].icache = &rv_icache;
        rv_cores[1].icache = &rv_icache;
    } else {
        cpu_reset_core(CORE0);
        cpu_reset_core(CORE1);
    }
}

void bramble_set_clock(int freq_mhz) {
    timing_set_clock_mhz((uint32_t)freq_mhz);
}

/* Read one byte from UART TX buffer (firmware output). Returns -1 if empty. */
int bramble_read_uart(void) {
    if (uart_tx_tail != uart_tx_head) {
        int ch = uart_tx_buf[uart_tx_tail];
        uart_tx_tail = (uart_tx_tail + 1) % UART_TX_BUF_SIZE;
        return ch;
    }
    return -1;
}

/* Read up to max_len bytes from UART TX buffer. Returns bytes read. */
int bramble_read_uart_bulk(uint8_t *dest, int max_len) {
    int count = 0;
    while (count < max_len && uart_tx_tail != uart_tx_head) {
        dest[count++] = uart_tx_buf[uart_tx_tail];
        uart_tx_tail = (uart_tx_tail + 1) % UART_TX_BUF_SIZE;
    }
    return count;
}

/* Push a byte into UART RX FIFO (firmware input). */
void bramble_write_uart(int ch) {
    int next = (uart_rx_head + 1) % 256;
    if (next != uart_rx_tail) {
        uart_rx_buf[uart_rx_head] = (uint8_t)ch;
        uart_rx_head = next;
    }
}

/* Called periodically by the step loop to feed UART RX from our buffer. */
static void feed_uart_rx(void) {
    /* Route like native stdin_pending_flush: USB CDC preferred when enumerated,
     * else UART0. USB gets raw bytes (MicroPython wants CR); UART normalizes
     * CR/CRLF to LF (littleOS shells). */
    static int wasm_saw_cr = 0;
    int usb_active = usb_cdc_stdio_active();
    while (uart_rx_tail != uart_rx_head) {
        int ch = uart_rx_buf[uart_rx_tail];
        uart_rx_tail = (uart_rx_tail + 1) % 256;
        if (usb_active) {
            wasm_saw_cr = 0;
            if (!usb_cdc_rx_push((uint8_t)ch))
                uart_rx_push(0, (uint8_t)ch);
        } else if (ch == '\r') {
            uart_rx_push(0, (uint8_t)'\n');
            wasm_saw_cr = 1;
        } else if (ch == '\n') {
            if (wasm_saw_cr) { wasm_saw_cr = 0; continue; }
            uart_rx_push(0, (uint8_t)ch);
        } else {
            wasm_saw_cr = 0;
            uart_rx_push(0, (uint8_t)ch);
        }
    }
    /* Drain WebSocket net RX queue into guest console as well */
    {
        uint8_t tmp[256];
        int n = bramble_net_pop_rx(tmp, sizeof(tmp));
        for (int i = 0; i < n; i++) {
            if (usb_cdc_stdio_active()) {
                if (!usb_cdc_rx_push((uint8_t)tmp[i]))
                    uart_rx_push(0, tmp[i]);
            } else {
                uart_rx_push(0, tmp[i]);
            }
        }
    }
}

int bramble_get_gpio(int pin) {
    return gpio_get_pin((uint8_t)pin);
}

int bramble_get_gpio_raw(int pin) {
    if (pin < 0 || pin >= 48) return 0;
    if (pin < 32) return (int)((gpio_state.gpio_out >> (uint32_t)pin) & 1u);
    return (int)((gpio_state.gpio_out_hi >> (uint32_t)(pin - 32)) & 1u);
}

uint32_t bramble_get_gpio_out(void) {
    return gpio_state.gpio_out;
}

uint32_t bramble_get_gpio_oe(void) {
    return gpio_state.gpio_oe;
}

void bramble_set_gpio(int pin, int val) {
    gpio_set_pin((uint8_t)pin, (uint8_t)val);
}

uint32_t bramble_mem_read32(uint32_t addr) {
    return mem_read32(addr);
}

void bramble_mem_write32(uint32_t addr, uint32_t val) {
    mem_write32(addr, val);
}

/* WASM watchdog reboot (mirrors main.c reboot_from_watchdog, minus CLI paths) */
static void bramble_watchdog_reboot(void) {
    watchdog_reboot_pending = 0;
    clocks_state.wdog_ctrl &= ~(1u << 31);
    reset_runtime_peripherals();
    dual_core_init();
    if (current_arch == ARCH_RV32) {
        rv_cpu_reset(&rv_cores[0], 0x00000000);
        rv_cpu_reset(&rv_cores[1], 0x00000000);
        rv_cores[1].is_halted = 1;
    } else {
        cpu_reset_core(CORE0);
        if (wasm_corepool_num_cores() > 1) cpu_reset_core(CORE1);
    }
}

/* GDB stop state for UI polling (non-blocking RSP via bramble_gdb_poll) */
static int wasm_gdb_enabled = 0;
static int wasm_gdb_hit = 0;
static int wasm_gdb_hit_core = 0;

int bramble_gdb_enable(int on) {
    wasm_gdb_enabled = on ? 1 : 0;
    if (on) {
        bramble_gdb_start();
        bramble_gdb_notify_stop();
    } else {
        bramble_gdb_stop();
        wasm_gdb_hit = 0;
    }
    return wasm_gdb_enabled;
}
int bramble_gdb_is_hit(void) { return wasm_gdb_hit; }
int bramble_gdb_hit_core(void) { return wasm_gdb_hit_core; }
void bramble_gdb_break(void) {
    if (wasm_gdb_enabled && gdb.active) {
        /* Inject Ctrl-C like native 'pkt[0]==0x03' path */
        uint8_t c = 0x03;
        bramble_gdb_push_rx(&c, 1);
    }
}
/* Called each bramble_step while stopped: drain one RSP packet.
 * Returns 0=resume, 1=resume+single-step once, 2=stay stopped, -1=detached. */
static int bramble_gdb_service_stopped(void) {
    int r = bramble_gdb_poll();
    if (r == 0) { wasm_gdb_hit = 0; return 0; }
    if (r == 1) { wasm_gdb_hit = 0; return 1; }
    if (r == -1) { wasm_gdb_hit = 0; wasm_gdb_enabled = 0; return 0; }
    return 2;
}

int bramble_step(int n_instructions) {
    if (timing_config.cycles_per_us == 0)
        timing_set_clock_mhz(1);
    int ncores = wasm_corepool_num_cores();
    if (ncores < 1) ncores = 1;
    if (ncores > 2) ncores = 2;

    if (current_arch == ARCH_RV32) {
        int total = 0;
        uint32_t poll_ctr = 0;
        while (total < n_instructions) {
            /* If stopped on breakpoint, service RSP without advancing */
            if (wasm_gdb_hit) {
                int svc = bramble_gdb_service_stopped();
                if (svc == 2) break; /* still stopped */
                if (svc == 1) {
                    /* single-step once then re-stop */
                    if (!rv_rom_intercept(&rv_cores[0])) rv_cpu_step(&rv_cores[0]);
                    total++;
                    wasm_gdb_hit = 1;
                    bramble_gdb_notify_stop();
                    break;
                }
                /* svc==0 resumed */
            }
            /* GDB non-blocking stop check (breakpoints/watchpoints/step) */
            if (wasm_gdb_enabled && gdb.active && !wasm_gdb_hit) {
                if (gdb_should_stop(rv_cores[0].pc, 0)) {
                    wasm_gdb_hit = 1; wasm_gdb_hit_core = 0; gdb.stop_core = 0;
                    bramble_gdb_notify_stop();
                    break;
                }
                if (wasm_corepool_num_cores() > 1 && !rv_cores[1].is_halted &&
                    gdb_should_stop(rv_cores[1].pc, 1)) {
                    wasm_gdb_hit = 1; wasm_gdb_hit_core = 1; gdb.stop_core = 1;
                    bramble_gdb_notify_stop();
                    break;
                }
            }
            if (rv_cpu_is_halted(&rv_cores[0]))
                break;
            if (!rv_cores[0].is_wfi) {
                if (!rv_rom_intercept(&rv_cores[0])) {
                    rv_cpu_step(&rv_cores[0]);
                    total++;
                }
            }
            if (ncores > 1 && !rv_cores[1].is_halted && !rv_cores[1].is_wfi) {
                if (!rv_rom_intercept(&rv_cores[1]))
                    rv_cpu_step(&rv_cores[1]);
            }
            /* Advance PIO + USB like native cooperative loop */
            pio_step();
            usb_step();
            uart_tick();
            rv_clint_tick(&rv_bus.clint, 1);
            if (rv_bus.clint.cycle_accum == 0) {
                timer_tick(1);
                rp2350_timer1_tick(&rv_bus.periph, 1);
            }
            rv_clint_check_interrupts(&rv_bus.clint, &rv_cores[0]);
            if (ncores > 1 && !rv_cores[1].is_halted)
                rv_clint_check_interrupts(&rv_bus.clint, &rv_cores[1]);
            if (ncores > 1 && rv_cores[1].is_halted) {
                uint32_t h1_entry, h1_sp, h1_arg;
                if (rv_membus_check_hart1_launch(&rv_bus, &h1_entry, &h1_sp, &h1_arg)) {
                    rv_cpu_reset(&rv_cores[1], h1_entry);
                    rv_cores[1].x[2] = h1_sp;
                    rv_cores[1].x[10] = h1_arg;
                }
            }
            if ((total & 0x3FF) == 0) {
                feed_uart_rx();
                net_bridge_poll();
                wire_poll();
                cyw43_tap_poll();
                if (wasm_vnet_on) vnet_poll();
                if (wasm_w5500_on) w5500_poll(&wasm_w5500);
                if (fault_count > 0) fault_check(rv_cores[0].cycle_count);
                if (script_enabled) script_poll((uint32_t)(rv_cores[0].cycle_count / (timing_config.cycles_per_us ? timing_config.cycles_per_us : 1)));
            }
            if (++poll_ctr >= 0xFFFFF) {
                poll_ctr = 0;
                if (wasm_sdcard_on) sdcard_flush(&wasm_sdcard);
                if (wasm_emmc_on) emmc_flush(&wasm_emmc);
            }
            if (watchdog_reboot_pending) bramble_watchdog_reboot();
            if (rv_cores[0].csr[CSR_MCAUSE] == MCAUSE_BREAKPOINT && rv_cores[0].x[10] == 0x20026)
                break;
            if (total >= n_instructions)
                break;
        }
        feed_uart_rx();
        return total;
    } else {
        int total = 0;
        uint32_t poll_ctr = 0;
        extern cpu_state_dual_t cores[2];
        while (total < n_instructions) {
            if (wasm_gdb_hit) {
                int svc = bramble_gdb_service_stopped();
                if (svc == 2) break;
                if (svc == 1) {
                    cpu_step_core((int)wasm_gdb_hit_core);
                    total++;
                    wasm_gdb_hit = 1;
                    bramble_gdb_notify_stop();
                    break;
                }
            }
            if (wasm_gdb_enabled && gdb.active && !wasm_gdb_hit) {
                int stop = 0;
                for (int gc = 0; gc < ncores; gc++) {
                    if (!cores[gc].is_halted && gdb_should_stop(cores[gc].r[15], gc)) {
                        wasm_gdb_hit = 1; wasm_gdb_hit_core = gc; gdb.stop_core = gc;
                        bramble_gdb_notify_stop();
                        stop = 1; break;
                    }
                }
                if (stop) break;
            }
            if (cpu_is_halted_core(0))
                break;
            /* WFI fast-forward (native dual_core_step mirror): when every
             * active core is asleep, jump guest time to the next timer
             * deadline instead of spinning. Skipped cycles count against
             * the budget so bramble_step(n) keeps its contract. */
            extern cpu_state_dual_t cores[2];
            int c0sleep = cores[0].is_wfi;
            int c1gone = (ncores <= 1 || cpu_is_halted_core(1) || cores[1].is_wfi);
            if (c0sleep && c1gone) {
                uint32_t chunk_us = timer_next_wakeup_us();
                if (chunk_us > 10000) chunk_us = 10000;
                if (chunk_us == 0) chunk_us = 1;
                uint32_t cpus = timing_config.cycles_per_us ?
                    timing_config.cycles_per_us : 1;
                systick_tick_for_core(0, chunk_us * cpus);
                if (ncores > 1) systick_tick_for_core(1, chunk_us * cpus);
                timer_tick(chunk_us);
                rtc_tick(chunk_us);
                for (int c = 0; c < ncores; c++) {
                    if (!cores[c].is_wfi) continue;
                    int saved = get_active_core();
                    set_active_core(c);
                    uint32_t pending = nvic_get_pending_irq();
                    set_active_core(saved);
                    if (pending != 0xFFFFFFFF ||
                        systick_states[c].pending ||
                        nvic_states[c].pendsv_pending)
                        cores[c].is_wfi = 0;
                }
                /* Bounded spurious wakeup (native mirror): SDK WFE loops
                 * always re-check their condition, so waking core0 every
                 * ~5ms of fast-forwarded time is harmless and prevents
                 * eternal sleep when no IRQ was ever programmed. */
                static uint32_t wasm_wfe_acc_us = 0;
                wasm_wfe_acc_us += chunk_us;
                if (wasm_wfe_acc_us >= 5000 && cores[0].is_wfi) {
                    wasm_wfe_acc_us = 0;
                    cores[0].is_wfi = 0;
                }
                total += chunk_us * cpus;
            } else {
                if (!cores[0].is_wfi) cpu_step_core(0);
                total++;
                if (ncores > 1 && !cpu_is_halted_core(1) && !cores[1].is_wfi)
                    cpu_step_core(1);
            }
            pio_step();
            usb_step();
            uart_tick();
            if ((total & 0x3FF) == 0) {
                feed_uart_rx();
                net_bridge_poll();
                wire_poll();
                cyw43_tap_poll();
                if (wasm_vnet_on) vnet_poll();
                if (wasm_w5500_on) w5500_poll(&wasm_w5500);
                if (fault_count > 0) fault_check(global_cycle_count);
                if (script_enabled) {
                    uint32_t eus = timing_config.cycles_per_us ?
                        (uint32_t)(global_cycle_count / timing_config.cycles_per_us) : 0;
                    script_poll(eus);
                }
            }
            if (++poll_ctr >= 0xFFFFF) {
                poll_ctr = 0;
                if (wasm_sdcard_on) sdcard_flush(&wasm_sdcard);
                if (wasm_emmc_on) emmc_flush(&wasm_emmc);
            }
            if (watchdog_reboot_pending) bramble_watchdog_reboot();
            if (total >= n_instructions)
                break;
        }
        feed_uart_rx();
        return total;
    }
}

int bramble_is_halted(void) {
    if (current_arch == ARCH_RV32)
        return rv_cpu_is_halted(&rv_cores[0]);
    return cpu_is_halted_core(0);
}

void bramble_get_core_state(int core, uint32_t *pc, uint32_t *sp) {
    if (current_arch == ARCH_RV32) {
        if (core == 0) { *pc = rv_cores[0].pc; *sp = rv_cores[0].x[2]; }
        else { *pc = rv_cores[1].pc; *sp = rv_cores[1].x[2]; }
    } else {
        extern cpu_state_dual_t cores[2];
        if (core == 0) { *pc = cores[0].r[15]; *sp = cores[0].r[13]; }
        else { *pc = cores[1].r[15]; *sp = cores[1].r[13]; }
    }
}

uint8_t *bramble_get_flash_ptr(void) {
    return cpu.flash;
}

uint8_t *bramble_get_sram_ptr(void) {
    return rv_bus.sram;
}

/* USB comprehesion probe: (enum<<16)|ctrl_state, for diagnosing stalls */
uint32_t bramble_usb_state32(void) {
    extern int usb_enum_state_dbg(void);
    extern int usb_ctrl_state_dbg(void);
    return ((uint32_t)(uint32_t)usb_enum_state_dbg() << 16) |
           (uint32_t)(uint32_t)usb_ctrl_state_dbg();
}

/* ============ Completed WASM controls (cores/JIT/debug/flash/SD/net) ============ */

void bramble_set_cores(int n) {
    wasm_corepool_set_num_cores(n);
    if (n == 1) num_active_cores = 1;
    else if (n == 2) num_active_cores = 2;
}
int bramble_get_cores(void) { return wasm_corepool_num_cores(); }
void bramble_set_quantum(int q) { corepool_set_step_quantum(q); }
void bramble_set_jit(int on) { jit_enable(on ? 1 : 0); }
void bramble_set_debug(int on, int core) {
    extern cpu_state_dual_t cores[2];
    if (core < 0 || core > 1) {
        cores[0].debug_enabled = on ? 1 : 0;
        cores[1].debug_enabled = on ? 1 : 0;
        rv_cores[0].debug_enabled = on ? 1 : 0;
        rv_cores[1].debug_enabled = on ? 1 : 0;
    } else if (current_arch == ARCH_RV32) {
        rv_cores[core].debug_enabled = on ? 1 : 0;
    } else {
        cores[core].debug_enabled = on ? 1 : 0;
    }
}
void bramble_set_semihosting(int on) { semihosting_enabled = on ? 1 : 0; }

/* Flash persistence via MEMFS (/flash.bin + /persist for IDBFS) */
int bramble_flash_save(void) {
    FILE *f = fopen("/flash.bin", "wb");
    if (!f) return 0;
    size_t n = fwrite(cpu.flash, 1, FLASH_SIZE_MAX, f);
    fclose(f);
    EM_ASM({ try { if (typeof FS !== 'undefined' && FS.syncfs) FS.syncfs(function(){}); } catch(e) {} });
    return (int)n;
}
int bramble_flash_load(void) {
    FILE *f = fopen("/flash.bin", "rb");
    if (!f) f = fopen("/persist/bramble_flash.bin", "rb");
    if (!f) return 0;
    size_t n = fread(cpu.flash, 1, FLASH_SIZE_MAX, f);
    fclose(f);
    return (int)n;
}
int bramble_flash_write(const uint8_t *data, int len, int offset) {
    if (!data || len <= 0) return 0;
    if (offset < 0) offset = 0;
    if ((uint32_t)offset >= FLASH_SIZE_MAX) return 0;
    if ((uint32_t)len > FLASH_SIZE_MAX - (uint32_t)offset) len = (int)(FLASH_SIZE_MAX - (uint32_t)offset);
    memcpy(cpu.flash + offset, data, (size_t)len);
    return len;
}

/* SD/eMMC images from JS buffers -> MEMFS -> native init + SPI attach */
int bramble_sdcard_load(const uint8_t *data, int len, int spi_num) {
    if (!data || len <= 0) return -1;
    FILE *f = fopen("/sdcard.img", "wb");
    if (!f) return -1;
    fwrite(data, 1, (size_t)len, f);
    fclose(f);
    if (wasm_sdcard_on) { sdcard_flush(&wasm_sdcard); sdcard_cleanup(&wasm_sdcard); wasm_sdcard_on = 0; }
    size_t mb = ((size_t)len + (1024*1024-1)) / (1024*1024);
    if (mb < 1) mb = 1;
    if (sdcard_init(&wasm_sdcard, "/sdcard.img", mb * 1024 * 1024) < 0) return -1;
    spi_attach_device(spi_num, sdcard_spi_xfer, sdcard_spi_cs, &wasm_sdcard);
    wasm_sdcard_on = 1;
    return 0;
}
int bramble_emmc_load(const uint8_t *data, int len, int spi_num) {
    if (!data || len <= 0) return -1;
    FILE *f = fopen("/emmc.img", "wb");
    if (!f) return -1;
    fwrite(data, 1, (size_t)len, f);
    fclose(f);
    if (wasm_emmc_on) { emmc_flush(&wasm_emmc); emmc_cleanup(&wasm_emmc); wasm_emmc_on = 0; }
    size_t mb = ((size_t)len + (1024*1024-1)) / (1024*1024);
    if (mb < 1) mb = 1;
    if (emmc_init(&wasm_emmc, "/emmc.img", mb * 1024 * 1024) < 0) return -1;
    spi_attach_device(spi_num, emmc_spi_xfer, emmc_spi_cs, &wasm_emmc);
    wasm_emmc_on = 1;
    return 0;
}

/* Virtual net + W5500 + SDD */
int bramble_net_enable(int live) {
    if (!wasm_vnet_on) { vnet_init(); wasm_vnet_on = 1; }
    if (live && !wasm_w5500_on) {
        w5500_init(&wasm_w5500);
        w5500_set_live(&wasm_w5500, 1);
        wasm_w5500_on = 1;
    }
    return 1;
}
int bramble_sdd_add(const char *arg) {
    if (!arg) return -1;
    sdd_init();
    return sdd_create_from_arg((char*)arg);
}
/* ETH mesh RX from BroadcastChannel/WebSocket proxy -> vnet */
int bramble_eth_push_rx(const uint8_t *data, int len) {
    if (!data || len < 14 || len > 1522) return -1;
    if (!wasm_vnet_on) { vnet_init(); wasm_vnet_on = 1; }
    vnet_tx_frame(-1, data, len);
    return 0;
}
/* W5500 proxy RX into default live device */
int bramble_w5500_push_rx(int sock, const uint8_t *data, int len) {
    if (!wasm_w5500_on) return -1;
    extern int bramble_w5500_dev_push_rx(w5500_t *dev, int sock, const uint8_t *data, int len);
    return bramble_w5500_dev_push_rx(&wasm_w5500, sock, data, len);
}
int bramble_w5500_push_status(int sock, int code) {
    if (!wasm_w5500_on) return -1;
    extern int bramble_w5500_dev_push_status(w5500_t *dev, int sock, int code);
    return bramble_w5500_dev_push_status(&wasm_w5500, sock, code);
}

/* Devtools: all 18 tools over MEMFS (tmp bins) + query hooks */
int bramble_coverage_start(void) { coverage_init(); coverage_enabled = 1; return 1; }
int bramble_coverage_dump(void) { coverage_dump("/coverage.bin"); coverage_report(); return 1; }
int bramble_trace_start(void) { trace_init("/trace.bin"); return 1; }
void bramble_trace_stop(void) { trace_cleanup(); }
int bramble_hotspots_start(int n) { hotspots_init(); hotspots_enabled = 1; hotspots_top_n = n > 0 ? n : 20; return 1; }
int bramble_hotspots_report(void) { hotspots_report(); return 1; }
int bramble_profile_start(void) { profile_init(); profile_enabled = 1; return 1; }
int bramble_profile_dump(void) { profile_dump("/profile.csv"); profile_report(); return 1; }
int bramble_callgraph_start(void) { callgraph_init(); callgraph_enabled = 1; return 1; }
int bramble_callgraph_dump(void) { callgraph_dump("/callgraph.dot"); return 1; }
int bramble_gpiotrace_start(void) { gpio_trace_init("/gpio.vcd"); return 1; }
void bramble_gpiotrace_stop(void) { gpio_trace_cleanup(); }
int bramble_irqlat_start(void) { irq_latency_enabled = 1; return 1; }
int bramble_irqlat_report(void) { irq_latency_report(); return 1; }
int bramble_stackcheck_start(void) { stack_check_enabled = 1; return 1; }
int bramble_stackcheck_report(void) { stack_check_report(); return 1; }
int bramble_symbols_load(const char *path) { return symbols_load(path); }
int bramble_watch_add(uint32_t addr, uint32_t len) { return watch_add(addr, len); }
int bramble_fault_add(const char *spec) { return fault_add(spec); }
int bramble_script_load(const uint8_t *data, int len) {
    if (!data || len <= 0) return -1;
    FILE *f = fopen("/script.txt", "wb");
    if (!f) return -1;
    fwrite(data, 1, (size_t)len, f);
    fclose(f);
    return script_init("/script.txt");
}
int bramble_expect_start(const char *path) { expect_init(path); return 1; }
int bramble_expect_check(void) { return expect_check(); }
int bramble_heatmap_start(void) { mem_heatmap_init(); mem_heatmap_enabled = 1; return 1; }
int bramble_heatmap_dump(void) { mem_heatmap_dump("/heatmap.csv"); return 1; }
void bramble_set_buslog(int uart, int spi, int i2c) {
    log_uart_enabled = uart ? 1 : 0;
    log_spi_enabled = spi ? 1 : 0;
    log_i2c_enabled = i2c ? 1 : 0;
}

