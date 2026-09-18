/*
 * Developer Tools for Bramble RP2040 Emulator
 *
 * Implementation of semihosting, coverage, hotspots, trace,
 * SYSCFG, and TBMAN peripherals.
 */

#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "emulator.h"
#include "devtools.h"
#include "nvic.h"

/* ========================================================================
 * ARM Semihosting
 * ======================================================================== */

int semihosting_enabled = 0;
int semihost_exit_requested = 0;
int semihost_exit_code = 0;

void semihosting_init(void) {
    semihosting_enabled = 1;
    semihost_exit_requested = 0;
    semihost_exit_code = 0;
}

/* Read a uint32 from guest memory at addr */
static uint32_t sh_read32(uint32_t addr) {
    return mem_read32(addr);
}

/* Read a byte from guest memory */
static uint8_t sh_read8(uint32_t addr) {
    return mem_read8(addr);
}

int semihosting_handle(void) {
    if (!semihosting_enabled) return 0;

    uint32_t op = cpu.r[0];
    uint32_t param = cpu.r[1];

    switch (op) {
    case SEMIHOST_SYS_WRITEC: {
        /* R1 points to a single character */
        char c = (char)sh_read8(param);
        fputc(c, stdout);
        fflush(stdout);
        cpu.r[0] = 0;
        break;
    }
    case SEMIHOST_SYS_WRITE0: {
        /* R1 points to a null-terminated string */
        for (int i = 0; i < 4096; i++) {
            char c = (char)sh_read8(param + (uint32_t)i);
            if (c == '\0') break;
            fputc(c, stdout);
        }
        fflush(stdout);
        cpu.r[0] = 0;
        break;
    }
    case SEMIHOST_SYS_WRITE: {
        /* param[0]=fd, param[1]=data_ptr, param[2]=len */
        uint32_t fd   = sh_read32(param);
        uint32_t data = sh_read32(param + 4);
        uint32_t len  = sh_read32(param + 8);
        FILE *out = (fd == 1) ? stdout : (fd == 2) ? stderr : stdout;
        for (uint32_t i = 0; i < len && i < 65536; i++) {
            fputc(sh_read8(data + i), out);
        }
        fflush(out);
        cpu.r[0] = 0;  /* 0 = all bytes written */
        break;
    }
    case SEMIHOST_SYS_READC: {
        int c = fgetc(stdin);
        cpu.r[0] = (c == EOF) ? (uint32_t)-1 : (uint32_t)c;
        break;
    }
    case SEMIHOST_SYS_EXIT: {
        /* ADP_Stopped_ApplicationExit = 0x20026 */
        semihost_exit_code = (param == 0x20026) ? 0 : (int)param;
        semihost_exit_requested = 1;
        cpu.r[15] = 0xFFFFFFFF;  /* Halt the core */
        break;
    }
    case SEMIHOST_SYS_EXIT_EXTENDED: {
        /* param[0] = reason, param[1] = exit code */
        semihost_exit_code = (int)sh_read32(param + 4);
        semihost_exit_requested = 1;
        cpu.r[15] = 0xFFFFFFFF;
        break;
    }
    case SEMIHOST_SYS_ERRNO:
        cpu.r[0] = 0;
        break;
    case SEMIHOST_SYS_ELAPSED:
        /* Return emulated time in centiseconds */
        cpu.r[0] = 0;
        break;
    case SEMIHOST_SYS_TICKFREQ:
        cpu.r[0] = 100;  /* 100 Hz */
        break;
    case SEMIHOST_SYS_OPEN:
    case SEMIHOST_SYS_CLOSE:
    case SEMIHOST_SYS_READ:
    case SEMIHOST_SYS_SEEK:
    case SEMIHOST_SYS_FLEN:
        /* Unimplemented file ops: return -1 */
        cpu.r[0] = (uint32_t)-1;
        break;
    default:
        /* Unknown operation */
        cpu.r[0] = (uint32_t)-1;
        break;
    }

    /* Advance PC past the BKPT instruction */
    cpu.r[15] += 2;
    return 1;
}

/* ========================================================================
 * RISC-V Semihosting
 *
 * The Pico SDK uses the ebreak instruction with a0=x10 holding the
 * operation number and a1=x11 holding the parameter.  This follows
 * the same ARM semihosting conventions (SYS_* operation codes).
 * ======================================================================== */

#include "rp2350_rv/rv_cpu.h"

int rv_semihosting_handle(void *cpu_ptr) {
    rv_cpu_state_t *cpu = (rv_cpu_state_t *)cpu_ptr;
    if (!semihosting_enabled) return 0;

    uint32_t op = cpu->x[10];    /* a0 = operation */
    uint32_t param = cpu->x[11]; /* a1 = parameter */

    switch (op) {
    case SEMIHOST_SYS_WRITEC: {
        char c = (char)sh_read8(param);
        fputc(c, stdout);
        fflush(stdout);
        cpu->x[0] = 0;
        return 1;
    }
    case SEMIHOST_SYS_WRITE0: {
        for (int i = 0; i < 4096; i++) {
            char c = (char)sh_read8(param + (uint32_t)i);
            if (c == '\0') break;
            fputc(c, stdout);
        }
        fflush(stdout);
        cpu->x[0] = 0;
        return 1;
    }
    case SEMIHOST_SYS_WRITE: {
        uint32_t fd   = sh_read32(param);
        uint32_t data = sh_read32(param + 4);
        uint32_t len  = sh_read32(param + 8);
        FILE *out = (fd == 1) ? stdout : (fd == 2) ? stderr : stdout;
        for (uint32_t i = 0; i < len && i < 65536; i++) {
            fputc(sh_read8(data + i), out);
        }
        fflush(out);
        cpu->x[0] = 0;
        return 1;
    }
    case SEMIHOST_SYS_READC: {
        int c = fgetc(stdin);
        cpu->x[0] = (c == EOF) ? (uint32_t)-1 : (uint32_t)c;
        return 1;
    }
    case SEMIHOST_SYS_EXIT: {
        semihost_exit_code = (param == 0x20026) ? 0 : (int)param;
        semihost_exit_requested = 1;
        return 1;
    }
    case SEMIHOST_SYS_EXIT_EXTENDED: {
        semihost_exit_code = (int)sh_read32(param + 4);
        semihost_exit_requested = 1;
        return 1;
    }
    case SEMIHOST_SYS_ERRNO:
        cpu->x[0] = 0;
        return 1;
    case SEMIHOST_SYS_ELAPSED:
        cpu->x[0] = 0;
        return 1;
    case SEMIHOST_SYS_TICKFREQ:
        cpu->x[0] = 100;
        return 1;
    case SEMIHOST_SYS_OPEN:
    case SEMIHOST_SYS_CLOSE:
    case SEMIHOST_SYS_READ:
    case SEMIHOST_SYS_SEEK:
    case SEMIHOST_SYS_FLEN:
        cpu->x[0] = (uint32_t)-1;
        return 1;
    default:
        return 0;  /* Not a semihosting call */
    }
}

/* ========================================================================
 * Code Coverage
 * ======================================================================== */

int coverage_enabled = 0;
uint8_t *coverage_bitmap = NULL;
uint8_t *coverage_ram_bitmap = NULL;

void coverage_init(void) {
    coverage_bitmap = calloc(1, COVERAGE_BITMAP_BYTES);
    coverage_ram_bitmap = calloc(1, (RAM_SIZE / 2 + 7) / 8);
    if (coverage_bitmap && coverage_ram_bitmap) {
        coverage_enabled = 1;
    } else {
        free(coverage_bitmap);
        free(coverage_ram_bitmap);
        coverage_bitmap = NULL;
        coverage_ram_bitmap = NULL;
    }
}

void coverage_cleanup(void) {
    free(coverage_bitmap);
    free(coverage_ram_bitmap);
    coverage_bitmap = NULL;
    coverage_ram_bitmap = NULL;
    coverage_enabled = 0;
}

void coverage_dump(const char *path) {
    if (!coverage_bitmap) return;
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[Coverage] Failed to open %s for writing\n", path);
        return;
    }
    /* Header: magic, flash_base, flash_size, ram_base, ram_size */
    uint32_t hdr[5] = { 0x434F5642, FLASH_BASE, FLASH_SIZE, RAM_BASE, RAM_SIZE }; /* "BVOC" */
    fwrite(hdr, 4, 5, f);
    fwrite(coverage_bitmap, 1, COVERAGE_BITMAP_BYTES, f);
    fwrite(coverage_ram_bitmap, 1, (RAM_SIZE / 2 + 7) / 8, f);
    fclose(f);
    fprintf(stderr, "[Coverage] Written to %s\n", path);
}

void coverage_report(void) {
    if (!coverage_bitmap) return;
    uint32_t flash_covered = 0;
    for (uint32_t i = 0; i < COVERAGE_BITMAP_BYTES; i++) {
        flash_covered += (uint32_t)__builtin_popcount(coverage_bitmap[i]);
    }
    uint32_t flash_total = FLASH_SIZE / 2;
    uint32_t ram_covered = 0;
    uint32_t ram_bytes = (RAM_SIZE / 2 + 7) / 8;
    for (uint32_t i = 0; i < ram_bytes; i++) {
        ram_covered += (uint32_t)__builtin_popcount(coverage_ram_bitmap[i]);
    }
    fprintf(stderr, " Coverage: %u/%u flash halfwords (%.1f%%), %u RAM halfwords executed\n",
            flash_covered, flash_total,
            flash_total > 0 ? (double)flash_covered / flash_total * 100.0 : 0.0,
            ram_covered);
}

/* ========================================================================
 * Hotspot Profiling
 * ======================================================================== */

int hotspots_enabled = 0;
int hotspots_top_n = 20;
hotspot_entry_t *hotspot_map = NULL;

void hotspots_init(void) {
    hotspot_map = calloc(HOTSPOT_MAP_SIZE, sizeof(hotspot_entry_t));
    if (hotspot_map) {
        hotspots_enabled = 1;
    }
}

void hotspots_cleanup(void) {
    free(hotspot_map);
    hotspot_map = NULL;
    hotspots_enabled = 0;
}

/* Comparison for qsort (descending by count) */
static int hotspot_cmp(const void *a, const void *b) {
    const hotspot_entry_t *ha = a, *hb = b;
    if (hb->count > ha->count) return 1;
    if (hb->count < ha->count) return -1;
    return 0;
}

void hotspots_report(void) {
    if (!hotspot_map) return;

    /* Collect non-zero entries */
    int count = 0;
    for (int i = 0; i < HOTSPOT_MAP_SIZE; i++) {
        if (hotspot_map[i].count > 0) count++;
    }
    if (count == 0) return;

    /* Copy to sortable array */
    int n = count < hotspots_top_n ? count : hotspots_top_n;
    hotspot_entry_t *sorted = malloc((size_t)count * sizeof(hotspot_entry_t));
    if (!sorted) return;

    int si = 0;
    for (int i = 0; i < HOTSPOT_MAP_SIZE; i++) {
        if (hotspot_map[i].count > 0) {
            sorted[si++] = hotspot_map[i];
        }
    }
    qsort(sorted, (size_t)count, sizeof(hotspot_entry_t), hotspot_cmp);

    fprintf(stderr, " Hotspots (top %d of %d unique PCs):\n", n, count);
    fprintf(stderr, "   %-12s  %-12s  %s\n", "Address", "Count", "Region");
    for (int i = 0; i < n; i++) {
        const char *region = "???";
        uint32_t pc = sorted[i].pc;
        if (pc >= FLASH_BASE && pc < FLASH_BASE + FLASH_SIZE)
            region = "flash";
        else if (pc >= RAM_BASE && pc < RAM_BASE + RAM_SIZE)
            region = "ram";
        else if (pc < 0x4000)
            region = "rom";
        fprintf(stderr, "   0x%08X  %-12u  %s\n", pc, sorted[i].count, region);
    }
    free(sorted);
}

/* ========================================================================
 * Instruction Trace
 * ======================================================================== */

int trace_enabled = 0;
FILE *trace_file = NULL;

void trace_init(const char *path) {
    trace_file = fopen(path, "wb");
    if (trace_file) {
        trace_enabled = 1;
        /* Write header: magic + version */
        uint32_t hdr[2] = { 0x54524342, 1 }; /* "BCRT" v1 */
        fwrite(hdr, 4, 2, trace_file);
        fprintf(stderr, "[Trace] Writing instruction trace to %s\n", path);
    } else {
        fprintf(stderr, "[Trace] Failed to open %s\n", path);
    }
}

void trace_cleanup(void) {
    if (trace_file) {
        fclose(trace_file);
        trace_file = NULL;
    }
    trace_enabled = 0;
}

/* ========================================================================
 * Exit Code Hook
 * ======================================================================== */

int exit_code_enabled = 0;
uint32_t exit_code_addr = 0;

/* ========================================================================
 * Timeout Enforcement
 * ======================================================================== */

int timeout_seconds = 0;
volatile int timeout_expired = 0;

static void timeout_handler(int sig) {
    (void)sig;
    timeout_expired = 1;
}

void timeout_start(int seconds) {
    timeout_seconds = seconds;
    timeout_expired = 0;
    signal(SIGALRM, timeout_handler);
    alarm((unsigned)seconds);
    fprintf(stderr, "[Init] Timeout: %d seconds\n", seconds);
}

void timeout_cancel(void) {
    alarm(0);
    signal(SIGALRM, SIG_DFL);
}

/* ========================================================================
 * SYSCFG Peripheral (0x40004000)
 *
 * RP2040 datasheet section 2.22. Registers:
 *   0x00 PROC0_NMI_MASK      R/W  NMI mask for proc0
 *   0x04 PROC1_NMI_MASK      R/W  NMI mask for proc1
 *   0x08 PROC_CONFIG          RO   Processor configuration
 *   0x0C PROC_IN_SYNC_BYPASS  R/W  Input sync bypass
 *   0x10 PROC_IN_SYNC_BYPASS_HI R/W
 *   0x14 DBGFORCE             R/W  Debug force
 *   0x18 MEMPOWERDOWN         R/W  Memory power-down control
 * ======================================================================== */

static uint32_t syscfg_regs[SYSCFG_SIZE / 4];

int syscfg_match(uint32_t addr) {
    uint32_t base = addr & ~0x3000u;
    return (base >= SYSCFG_BASE && base < SYSCFG_BASE + SYSCFG_SIZE);
}

uint32_t syscfg_read(uint32_t offset) {
    offset &= 0xFFF;
    if (offset >= SYSCFG_SIZE) return 0;
    if (offset == 0x08) {
        /* PROC_CONFIG: both processors out of reset */
        return 0x00000000;
    }
    return syscfg_regs[offset / 4];
}

void syscfg_write(uint32_t offset, uint32_t val) {
    offset &= 0xFFF;
    if (offset >= SYSCFG_SIZE) return;
    if (offset == 0x08) return; /* PROC_CONFIG is read-only */
    syscfg_regs[offset / 4] = val;
}

/* ========================================================================
 * TBMAN Peripheral (0x4006C000)
 *
 * RP2040 datasheet section 2.21. Registers:
 *   0x00 PLATFORM   RO  Bit 0 = ASIC, Bit 1 = FPGA
 *
 * In a real chip, PLATFORM reads 0x01 (ASIC). In simulation/emulation
 * it should read 0x02 (FPGA/sim) or a custom value.  We return 0x01
 * to match real hardware behavior (firmware expects ASIC).
 * ======================================================================== */

int tbman_match(uint32_t addr) {
    uint32_t base = addr & ~0x3000u;
    return (base >= TBMAN_BASE && base < TBMAN_BASE + TBMAN_SIZE);
}

uint32_t tbman_read(uint32_t offset) {
    offset &= 0xFFF;
    if (offset == 0x00) {
        return 0x01;  /* ASIC platform */
    }
    return 0;
}

void tbman_write(uint32_t offset, uint32_t val) {
    (void)offset;
    (void)val;
    /* All TBMAN registers are read-only */
}

/* ========================================================================
 * ELF Symbol Table Loading
 * ======================================================================== */

int symbols_loaded = 0;

/* ELF32 section and symbol structures (local to this file) */
typedef struct { uint32_t sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size;
                 uint32_t sh_link, sh_info, sh_addralign, sh_entsize; } elf32_shdr_t;
typedef struct { uint32_t st_name, st_value, st_size; uint8_t st_info, st_other;
                 uint16_t st_shndx; } elf32_sym_t;

#define SHT_SYMTAB 2
#define STT_FUNC   2

typedef struct {
    uint32_t addr;
    uint32_t size;
    char name[64];
} sym_entry_t;

static sym_entry_t *sym_table = NULL;
static int sym_count = 0;
static int sym_cap = 0;

static int sym_cmp(const void *a, const void *b) {
    const sym_entry_t *sa = a, *sb = b;
    if (sa->addr < sb->addr) return -1;
    if (sa->addr > sb->addr) return 1;
    return 0;
}

int symbols_load(const char *elf_path) {
    FILE *f = fopen(elf_path, "rb");
    if (!f) return 0;

    uint8_t ident[16];
    if (fread(ident, 1, 16, f) != 16 || ident[0] != 0x7f || ident[1] != 'E') {
        fclose(f); return 0;
    }

    /* Read ELF header fields we need */
    fseek(f, 32, SEEK_SET); /* e_shoff */
    uint32_t e_shoff; if (fread(&e_shoff, 4, 1, f) != 1) { fclose(f); return 0; }
    fseek(f, 46, SEEK_SET);
    uint16_t e_shentsize, e_shnum, e_shstrndx;
    if (fread(&e_shentsize, 2, 1, f) != 1 ||
        fread(&e_shnum, 2, 1, f) != 1 ||
        fread(&e_shstrndx, 2, 1, f) != 1) { fclose(f); return 0; }

    if (e_shnum == 0 || e_shentsize < 40) { fclose(f); return 0; }

    /* Read all section headers */
    elf32_shdr_t *shdrs = calloc(e_shnum, sizeof(elf32_shdr_t));
    if (!shdrs) { fclose(f); return 0; }
    fseek(f, (long)e_shoff, SEEK_SET);
    for (int i = 0; i < e_shnum; i++) {
        if (fread(&shdrs[i], sizeof(elf32_shdr_t), 1, f) != 1) break;
        if (e_shentsize > sizeof(elf32_shdr_t))
            fseek(f, e_shentsize - sizeof(elf32_shdr_t), SEEK_CUR);
    }

    /* Find .symtab */
    for (int i = 0; i < e_shnum; i++) {
        if (shdrs[i].sh_type != SHT_SYMTAB) continue;

        uint32_t strtab_idx = shdrs[i].sh_link;
        if (strtab_idx >= e_shnum) continue;

        /* Load string table */
        uint32_t str_size = shdrs[strtab_idx].sh_size;
        char *strtab = malloc(str_size);
        if (!strtab) continue;
        fseek(f, (long)shdrs[strtab_idx].sh_offset, SEEK_SET);
        if (fread(strtab, 1, str_size, f) != str_size) { free(strtab); continue; }

        /* Load symbols */
        uint32_t nsyms = shdrs[i].sh_size / sizeof(elf32_sym_t);
        fseek(f, (long)shdrs[i].sh_offset, SEEK_SET);

        sym_cap = 1024;
        sym_table = malloc((size_t)sym_cap * sizeof(sym_entry_t));
        sym_count = 0;

        for (uint32_t s = 0; s < nsyms; s++) {
            elf32_sym_t sym;
            if (fread(&sym, sizeof(sym), 1, f) != 1) break;
            if ((sym.st_info & 0xF) != STT_FUNC) continue;
            if (sym.st_value == 0) continue;
            if (sym.st_name >= str_size) continue;

            if (sym_count >= sym_cap) {
                sym_cap *= 2;
                sym_table = realloc(sym_table, (size_t)sym_cap * sizeof(sym_entry_t));
            }
            sym_entry_t *e = &sym_table[sym_count++];
            e->addr = sym.st_value & ~1u; /* Clear Thumb bit */
            e->size = sym.st_size;
            strncpy(e->name, &strtab[sym.st_name], sizeof(e->name) - 1);
            e->name[sizeof(e->name) - 1] = '\0';
        }

        free(strtab);
        break;
    }

    free(shdrs);
    fclose(f);

    if (sym_count > 0) {
        qsort(sym_table, (size_t)sym_count, sizeof(sym_entry_t), sym_cmp);
        symbols_loaded = 1;
        fprintf(stderr, "[Symbols] Loaded %d function symbols from %s\n", sym_count, elf_path);
    }
    return sym_count;
}

void symbols_cleanup(void) {
    free(sym_table);
    sym_table = NULL;
    sym_count = 0;
    symbols_loaded = 0;
}

const char *symbols_lookup(uint32_t addr, uint32_t *offset_out) {
    if (!symbols_loaded || sym_count == 0) return NULL;
    /* Binary search for largest addr <= target */
    int lo = 0, hi = sym_count - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (sym_table[mid].addr <= addr) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (best < 0) return NULL;
    uint32_t off = addr - sym_table[best].addr;
    if (sym_table[best].size > 0 && off >= sym_table[best].size) return NULL;
    if (offset_out) *offset_out = off;
    return sym_table[best].name;
}

/* ========================================================================
 * Scripted I/O
 * ======================================================================== */

int script_enabled = 0;

typedef struct {
    uint32_t time_us;
    int type;     /* 0=uart, 1=gpio */
    int channel;  /* uart num or gpio pin */
    uint8_t data[256];
    int data_len;
    int gpio_val;
    int fired;
} script_event_t;

static script_event_t *script_events = NULL;
static int script_event_count = 0;
static int script_event_cap = 0;

int script_init(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[Script] Failed to open %s\n", path); return -1; }

    script_event_cap = 64;
    script_events = calloc((size_t)script_event_cap, sizeof(script_event_t));
    script_event_count = 0;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        uint32_t ms;
        char cmd[32], arg[256];
        if (sscanf(line, "%ums: %31s %255[^\n]", &ms, cmd, arg) < 2) continue;

        if (script_event_count >= script_event_cap) {
            script_event_cap *= 2;
            script_events = realloc(script_events, (size_t)script_event_cap * sizeof(script_event_t));
        }
        script_event_t *ev = &script_events[script_event_count++];
        memset(ev, 0, sizeof(*ev));
        ev->time_us = ms * 1000;

        if (strncmp(cmd, "uart", 4) == 0) {
            ev->type = 0;
            ev->channel = cmd[4] - '0';
            /* Parse string: strip quotes, handle \n */
            int di = 0;
            int in_str = 0;
            for (int i = 0; arg[i] && di < 255; i++) {
                if (arg[i] == '"') { in_str = !in_str; continue; }
                if (arg[i] == '\\' && arg[i+1] == 'n') { ev->data[di++] = '\r'; i++; continue; }
                if (arg[i] == '\\' && arg[i+1] == 'r') { ev->data[di++] = '\r'; i++; continue; }
                ev->data[di++] = (uint8_t)arg[i];
            }
            ev->data_len = di;
        } else if (strncmp(cmd, "gpio", 4) == 0) {
            ev->type = 1;
            ev->channel = atoi(cmd + 4);
            ev->gpio_val = atoi(arg);
        }
    }
    fclose(f);
    script_enabled = 1;
    fprintf(stderr, "[Script] Loaded %d events from %s\n", script_event_count, path);
    return 0;
}

void script_poll(uint32_t elapsed_us) {
    if (!script_enabled) return;
    extern int uart_rx_push(int uart_num, uint8_t byte);
    extern void gpio_set_input_pin(uint8_t pin, uint8_t value);

    for (int i = 0; i < script_event_count; i++) {
        script_event_t *ev = &script_events[i];
        if (ev->fired || elapsed_us < ev->time_us) continue;
        ev->fired = 1;

        if (ev->type == 0) {
            for (int d = 0; d < ev->data_len; d++) {
                uart_rx_push(ev->channel, ev->data[d]);
            }
        } else if (ev->type == 1) {
            gpio_set_input_pin((uint8_t)ev->channel, (uint8_t)ev->gpio_val);
        }
    }
}

void script_cleanup(void) {
    free(script_events);
    script_events = NULL;
    script_event_count = 0;
    script_enabled = 0;
}

/* ========================================================================
 * Expected Output Matching
 * ======================================================================== */

int expect_enabled = 0;
char *expect_path = NULL;
char *expect_capture_buf = NULL;
size_t expect_capture_len = 0;
size_t expect_capture_cap = 0;

void expect_init(const char *path) {
    expect_path = strdup(path);
    expect_capture_cap = 4096;
    expect_capture_buf = malloc(expect_capture_cap);
    expect_capture_len = 0;
    expect_enabled = 1;
}

void expect_append(const char *data, size_t len) {
    if (!expect_enabled || !expect_capture_buf) return;
    while (expect_capture_len + len > expect_capture_cap) {
        expect_capture_cap *= 2;
        expect_capture_buf = realloc(expect_capture_buf, expect_capture_cap);
    }
    memcpy(expect_capture_buf + expect_capture_len, data, len);
    expect_capture_len += len;
}

int expect_check(void) {
    if (!expect_enabled || !expect_path) return 0;

    FILE *f = fopen(expect_path, "rb");
    if (!f) {
        fprintf(stderr, "[Expect] Cannot open golden file: %s\n", expect_path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *golden = malloc((size_t)fsize);
    size_t nread = fread(golden, 1, (size_t)fsize, f);
    fclose(f);
    if ((long)nread != fsize) { free(golden); return 1; }

    int match = ((size_t)fsize == expect_capture_len &&
                 memcmp(golden, expect_capture_buf, expect_capture_len) == 0);

    if (match) {
        fprintf(stderr, "[Expect] Output matches golden file (%zu bytes)\n", expect_capture_len);
    } else {
        fprintf(stderr, "[Expect] Output DIFFERS from golden file\n");
        fprintf(stderr, "[Expect]   Expected: %ld bytes, Got: %zu bytes\n",
                fsize, expect_capture_len);
        /* Show first difference */
        size_t min_len = (size_t)fsize < expect_capture_len ? (size_t)fsize : expect_capture_len;
        for (size_t i = 0; i < min_len; i++) {
            if (golden[i] != expect_capture_buf[i]) {
                fprintf(stderr, "[Expect]   First diff at byte %zu: expected 0x%02X got 0x%02X\n",
                        i, (uint8_t)golden[i], (uint8_t)expect_capture_buf[i]);
                break;
            }
        }
    }
    free(golden);
    return match ? 0 : 1;
}

void expect_cleanup(void) {
    free(expect_path);
    free(expect_capture_buf);
    expect_path = NULL;
    expect_capture_buf = NULL;
    expect_enabled = 0;
}

/* ========================================================================
 * Memory Watch Log
 * ======================================================================== */

watch_region_t watch_regions[MAX_WATCH_REGIONS];
int watch_count = 0;

int watch_add(uint32_t addr, uint32_t len) {
    if (watch_count >= MAX_WATCH_REGIONS) return -1;
    watch_regions[watch_count].addr = addr;
    watch_regions[watch_count].len = len;
    watch_regions[watch_count].active = 1;
    watch_count++;
    fprintf(stderr, "[Watch] Monitoring 0x%08X..0x%08X\n", addr, addr + len);
    return 0;
}

void watch_check_write(uint32_t addr, uint32_t val, int width) {
    for (int i = 0; i < watch_count; i++) {
        if (!watch_regions[i].active) continue;
        if (addr >= watch_regions[i].addr &&
            addr < watch_regions[i].addr + watch_regions[i].len) {
            const char *sym = symbols_lookup(cpu.r[15], NULL);
            fprintf(stderr, "[Watch] WRITE%d 0x%08X = 0x%08X (PC=0x%08X%s%s)\n",
                    width * 8, addr, val, cpu.r[15],
                    sym ? " " : "", sym ? sym : "");
        }
    }
}

void watch_check_read(uint32_t addr, uint32_t val, int width) {
    for (int i = 0; i < watch_count; i++) {
        if (!watch_regions[i].active) continue;
        if (addr >= watch_regions[i].addr &&
            addr < watch_regions[i].addr + watch_regions[i].len) {
            const char *sym = symbols_lookup(cpu.r[15], NULL);
            fprintf(stderr, "[Watch] READ%d  0x%08X → 0x%08X (PC=0x%08X%s%s)\n",
                    width * 8, addr, val, cpu.r[15],
                    sym ? " " : "", sym ? sym : "");
        }
    }
}

/* ========================================================================
 * Call Graph
 * ======================================================================== */

int callgraph_enabled = 0;

typedef struct {
    uint32_t caller;
    uint32_t callee;
    uint32_t count;
} callgraph_edge_t;

#define CALLGRAPH_MAP_SIZE (1 << 16)
#define CALLGRAPH_MAP_MASK (CALLGRAPH_MAP_SIZE - 1)

static callgraph_edge_t *cg_map = NULL;

void callgraph_init(void) {
    cg_map = calloc(CALLGRAPH_MAP_SIZE, sizeof(callgraph_edge_t));
    if (cg_map) callgraph_enabled = 1;
}

void callgraph_record_call(uint32_t caller_pc, uint32_t target_pc) {
    if (!callgraph_enabled) return;
    uint32_t hash = ((caller_pc >> 1) ^ (target_pc >> 1) ^ (caller_pc >> 17)) & CALLGRAPH_MAP_MASK;
    callgraph_edge_t *e = &cg_map[hash];
    if (e->caller == caller_pc && e->callee == target_pc) {
        e->count++;
    } else if (e->count == 0) {
        e->caller = caller_pc;
        e->callee = target_pc;
        e->count = 1;
    } else {
        /* Collision — overwrite (frequency bias) */
        e->caller = caller_pc;
        e->callee = target_pc;
        e->count++;
    }
}

void callgraph_dump(const char *path) {
    if (!cg_map) return;
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "[CallGraph] Failed to open %s\n", path); return; }

    fprintf(f, "digraph callgraph {\n");
    fprintf(f, "  rankdir=LR;\n");
    fprintf(f, "  node [shape=box, fontsize=10];\n");

    for (int i = 0; i < CALLGRAPH_MAP_SIZE; i++) {
        if (cg_map[i].count == 0) continue;
        const char *caller_name = symbols_lookup(cg_map[i].caller, NULL);
        const char *callee_name = symbols_lookup(cg_map[i].callee, NULL);

        if (caller_name && callee_name) {
            fprintf(f, "  \"%s\" -> \"%s\" [label=\"%u\"];\n",
                    caller_name, callee_name, cg_map[i].count);
        } else {
            fprintf(f, "  \"0x%08X\" -> \"0x%08X\" [label=\"%u\"];\n",
                    cg_map[i].caller, cg_map[i].callee, cg_map[i].count);
        }
    }
    fprintf(f, "}\n");
    fclose(f);
    fprintf(stderr, "[CallGraph] Written to %s\n", path);
}

void callgraph_cleanup(void) {
    free(cg_map);
    cg_map = NULL;
    callgraph_enabled = 0;
}

/* ========================================================================
 * Stack Watermark
 * ======================================================================== */

int stack_check_enabled = 0;
uint32_t stack_watermark[2] = { 0xFFFFFFFF, 0xFFFFFFFF };

void stack_check_report(void) {
    if (!stack_check_enabled) return;
    for (int c = 0; c < 2; c++) {
        if (stack_watermark[c] == 0xFFFFFFFF) continue;
        uint32_t stack_top = RAM_BASE + RAM_SIZE;
        uint32_t used = stack_top - stack_watermark[c];
        fprintf(stderr, " Stack Core %d: low watermark 0x%08X (peak %u bytes used)\n",
                c, stack_watermark[c], used);
        if (stack_watermark[c] < RAM_BASE + 1024) {
            fprintf(stderr, " WARNING: Core %d stack within 1KB of RAM base!\n", c);
        }
    }
}

/* ========================================================================
 * IRQ Latency Profiling
 * ======================================================================== */

int irq_latency_enabled = 0;
irq_latency_entry_t irq_latency[IRQ_LATENCY_MAX_IRQS];
uint64_t global_cycle_count = 0;

void irq_latency_pend(uint32_t irq) {
    if (!irq_latency_enabled || irq >= IRQ_LATENCY_MAX_IRQS) return;
    irq_latency[irq].pend_cycle = global_cycle_count;
}

void irq_latency_enter(uint32_t irq) {
    if (!irq_latency_enabled || irq >= IRQ_LATENCY_MAX_IRQS) return;
    if (irq_latency[irq].pend_cycle == 0) return;

    uint32_t lat = (uint32_t)(global_cycle_count - irq_latency[irq].pend_cycle);
    irq_latency_entry_t *e = &irq_latency[irq];
    e->total_cycles += lat;
    e->count++;
    if (lat < e->min_cycles || e->min_cycles == 0) e->min_cycles = lat;
    if (lat > e->max_cycles) e->max_cycles = lat;
    e->pend_cycle = 0;
}

void irq_latency_report(void) {
    if (!irq_latency_enabled) return;
    fprintf(stderr, " IRQ Latency (cycles):\n");
    fprintf(stderr, "   %-6s  %-8s  %-8s  %-8s  %-8s\n",
            "IRQ", "Count", "Min", "Avg", "Max");
    for (int i = 0; i < IRQ_LATENCY_MAX_IRQS; i++) {
        if (irq_latency[i].count == 0) continue;
        uint32_t avg = (uint32_t)(irq_latency[i].total_cycles / irq_latency[i].count);
        fprintf(stderr, "   %-6d  %-8u  %-8u  %-8u  %-8u\n",
                i, irq_latency[i].count, irq_latency[i].min_cycles,
                avg, irq_latency[i].max_cycles);
    }
}

/* ========================================================================
 * Bus Transaction Logging
 * ======================================================================== */

int log_uart_enabled = 0;
int log_spi_enabled = 0;
int log_i2c_enabled = 0;

void bus_log_uart(int num, int is_tx, uint8_t byte) {
    if (!log_uart_enabled) return;
    char printable = (byte >= 0x20 && byte < 0x7F) ? (char)byte : '.';
    fprintf(stderr, "[UART%d] %s 0x%02X '%c'\n", num, is_tx ? "TX" : "RX", byte, printable);
}

void bus_log_spi(int num, int is_tx, uint8_t byte) {
    if (!log_spi_enabled) return;
    fprintf(stderr, "[SPI%d] %s 0x%02X\n", num, is_tx ? "MOSI" : "MISO", byte);
}

void bus_log_i2c(int num, int is_write, uint8_t addr7, uint8_t byte) {
    if (!log_i2c_enabled) return;
    fprintf(stderr, "[I2C%d] addr=0x%02X %s 0x%02X\n",
            num, addr7, is_write ? "W" : "R", byte);
}

/* ========================================================================
 * GPIO VCD Trace
 * ======================================================================== */

int gpio_trace_enabled = 0;
static FILE *vcd_file = NULL;
static uint32_t vcd_prev_pins = 0;

void gpio_trace_init(const char *path) {
    vcd_file = fopen(path, "w");
    if (!vcd_file) { fprintf(stderr, "[VCD] Failed to open %s\n", path); return; }

    /* VCD header */
    fprintf(vcd_file, "$timescale 1us $end\n");
    fprintf(vcd_file, "$scope module gpio $end\n");
    for (int i = 0; i < 30; i++) {
        fprintf(vcd_file, "$var wire 1 %c gpio%d $end\n", (char)('!' + i), i);
    }
    fprintf(vcd_file, "$upscope $end\n");
    fprintf(vcd_file, "$enddefinitions $end\n");
    fprintf(vcd_file, "#0\n");
    /* Initial state: all low */
    for (int i = 0; i < 30; i++) {
        fprintf(vcd_file, "0%c\n", (char)('!' + i));
    }

    vcd_prev_pins = 0;
    gpio_trace_enabled = 1;
    fprintf(stderr, "[VCD] GPIO trace → %s\n", path);
}

void gpio_trace_record(uint8_t pin, uint8_t value) {
    if (!gpio_trace_enabled || !vcd_file || pin >= 30) return;

    uint32_t mask = 1u << pin;
    uint32_t new_pins = (vcd_prev_pins & ~mask) | ((value ? 1u : 0u) << pin);
    if (new_pins == vcd_prev_pins) return;

    /* Timestamp in microseconds from global cycle count */
    uint32_t us = (timing_config.cycles_per_us > 0)
        ? (uint32_t)(global_cycle_count / timing_config.cycles_per_us)
        : (uint32_t)global_cycle_count;

    fprintf(vcd_file, "#%u\n%d%c\n", us, value ? 1 : 0, (char)('!' + pin));
    vcd_prev_pins = new_pins;
}

void gpio_trace_cleanup(void) {
    if (vcd_file) fclose(vcd_file);
    vcd_file = NULL;
    gpio_trace_enabled = 0;
}

/* ========================================================================
 * Fault Injection
 * ======================================================================== */

fault_injection_t fault_injections[MAX_FAULT_INJECTIONS];
int fault_count = 0;

int fault_add(const char *spec) {
    if (fault_count >= MAX_FAULT_INJECTIONS) return -1;

    fault_injection_t *fi = &fault_injections[fault_count];
    memset(fi, 0, sizeof(*fi));

    char type[32];
    uint64_t cycle;
    uint32_t addr = 0;

    if (sscanf(spec, "%31[^:]:%lu:%x", type, &cycle, &addr) < 2) {
        fprintf(stderr, "[Fault] Invalid spec: %s\n", spec);
        return -1;
    }

    if (strcmp(type, "flash_bitflip") == 0) {
        fi->type = FAULT_FLASH_BITFLIP;
        fi->addr = addr;
    } else if (strcmp(type, "ram_corrupt") == 0) {
        fi->type = FAULT_RAM_CORRUPT;
        fi->addr = addr;
    } else if (strcmp(type, "brownout") == 0) {
        fi->type = FAULT_BROWNOUT;
    } else {
        fprintf(stderr, "[Fault] Unknown type: %s\n", type);
        return -1;
    }

    fi->trigger_cycle = cycle;
    fi->fired = 0;
    fault_count++;
    fprintf(stderr, "[Fault] Scheduled %s at cycle %lu\n", type, (unsigned long)cycle);
    return 0;
}

void fault_check(uint64_t cycle) {
    extern int watchdog_reboot_pending;
    for (int i = 0; i < fault_count; i++) {
        fault_injection_t *fi = &fault_injections[i];
        if (fi->fired || cycle < fi->trigger_cycle) continue;
        fi->fired = 1;

        switch (fi->type) {
        case FAULT_FLASH_BITFLIP:
            if (fi->addr < FLASH_SIZE) {
                cpu.flash[fi->addr] ^= 0x01;
                fprintf(stderr, "[Fault] Flash bit flip at 0x%08X (cycle %lu)\n",
                        FLASH_BASE + fi->addr, (unsigned long)cycle);
            }
            break;
        case FAULT_RAM_CORRUPT:
            if (fi->addr >= RAM_BASE && fi->addr < RAM_BASE + RAM_SIZE) {
                uint32_t off = fi->addr - RAM_BASE;
                cpu.ram[off] = 0xDE;
                fprintf(stderr, "[Fault] RAM corruption at 0x%08X (cycle %lu)\n",
                        fi->addr, (unsigned long)cycle);
            }
            break;
        case FAULT_BROWNOUT:
            watchdog_reboot_pending = 1;
            fprintf(stderr, "[Fault] Brownout reset (cycle %lu)\n", (unsigned long)cycle);
            break;
        }
    }
}

/* ========================================================================
 * Cycle Profiling
 * ======================================================================== */

int profile_enabled = 0;
profile_entry_t *profile_map = NULL;

void profile_init(void) {
    profile_map = calloc(PROFILE_MAP_SIZE, sizeof(profile_entry_t));
    if (profile_map) profile_enabled = 1;
}

void profile_cleanup(void) {
    free(profile_map);
    profile_map = NULL;
    profile_enabled = 0;
}

static int profile_cmp(const void *a, const void *b) {
    const profile_entry_t *pa = a, *pb = b;
    if (pb->total_cycles > pa->total_cycles) return 1;
    if (pb->total_cycles < pa->total_cycles) return -1;
    return 0;
}

void profile_dump(const char *path) {
    if (!profile_map) return;
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "[Profile] Failed to open %s\n", path); return; }

    fprintf(f, "address,cycles,count,avg_cycles,function\n");
    for (int i = 0; i < PROFILE_MAP_SIZE; i++) {
        if (profile_map[i].count == 0) continue;
        uint32_t off = 0;
        const char *name = symbols_lookup(profile_map[i].pc, &off);
        fprintf(f, "0x%08X,%u,%u,%u,%s+0x%X\n",
                profile_map[i].pc, profile_map[i].total_cycles,
                profile_map[i].count,
                profile_map[i].total_cycles / profile_map[i].count,
                name ? name : "???", off);
    }
    fclose(f);
    fprintf(stderr, "[Profile] Written to %s\n", path);
}

void profile_report(void) {
    if (!profile_map) return;

    /* Collect and sort top 20 */
    int count = 0;
    for (int i = 0; i < PROFILE_MAP_SIZE; i++) {
        if (profile_map[i].count > 0) count++;
    }
    if (count == 0) return;

    int n = count < 20 ? count : 20;
    profile_entry_t *sorted = malloc((size_t)count * sizeof(profile_entry_t));
    if (!sorted) return;

    int si = 0;
    for (int i = 0; i < PROFILE_MAP_SIZE; i++) {
        if (profile_map[i].count > 0) sorted[si++] = profile_map[i];
    }
    qsort(sorted, (size_t)count, sizeof(profile_entry_t), profile_cmp);

    fprintf(stderr, " Cycle Profile (top %d of %d):\n", n, count);
    fprintf(stderr, "   %-12s  %-12s  %-8s  %s\n", "Address", "Cycles", "Count", "Function");
    for (int i = 0; i < n; i++) {
        uint32_t off = 0;
        const char *name = symbols_lookup(sorted[i].pc, &off);
        if (name) {
            fprintf(stderr, "   0x%08X  %-12u  %-8u  %s+0x%X\n",
                    sorted[i].pc, sorted[i].total_cycles, sorted[i].count, name, off);
        } else {
            fprintf(stderr, "   0x%08X  %-12u  %-8u\n",
                    sorted[i].pc, sorted[i].total_cycles, sorted[i].count);
        }
    }
    free(sorted);
}

/* ========================================================================
 * Memory Access Heatmap
 * ======================================================================== */

int mem_heatmap_enabled = 0;
heatmap_entry_t *heatmap_ram = NULL;

void mem_heatmap_init(void) {
    heatmap_ram = calloc(HEATMAP_RAM_BLOCKS, sizeof(heatmap_entry_t));
    if (heatmap_ram) mem_heatmap_enabled = 1;
}

void mem_heatmap_cleanup(void) {
    free(heatmap_ram);
    heatmap_ram = NULL;
    mem_heatmap_enabled = 0;
}

void mem_heatmap_dump(const char *path) {
    if (!heatmap_ram) return;
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "[Heatmap] Failed to open %s\n", path); return; }

    fprintf(f, "block_addr,reads,writes,total\n");
    for (uint32_t i = 0; i < HEATMAP_RAM_BLOCKS; i++) {
        if (heatmap_ram[i].reads == 0 && heatmap_ram[i].writes == 0) continue;
        fprintf(f, "0x%08X,%u,%u,%u\n",
                RAM_BASE + (i << HEATMAP_BLOCK_SHIFT),
                heatmap_ram[i].reads, heatmap_ram[i].writes,
                heatmap_ram[i].reads + heatmap_ram[i].writes);
    }
    fclose(f);
    fprintf(stderr, "[Heatmap] Written to %s\n", path);
}

/* ========================================================================
 * VREG_AND_CHIP_RESET Peripheral (0x40064000)
 *
 * RP2040 datasheet section 2.10. Three registers:
 *   0x00 VREG:       bit 12 = ROK (read-only, regulator OK), bits[7:4] = VSEL,
 *                    bit 1 = HIZ (high impedance), bit 0 = EN
 *   0x04 BOD:        bits[7:4] = VSEL, bit 0 = EN
 *   0x08 CHIP_RESET: bit 20 = had_psm, bit 16 = had_run, bit 8 = had_por
 *                    (all W1C — write 1 to clear)
 * ======================================================================== */

static uint32_t vreg_regs[VREG_SIZE / 4];
static int vreg_initialized = 0;

static void vreg_init_defaults(void) {
    if (vreg_initialized) return;
    /* VREG: enabled, VSEL=0xB (1.10V default), ROK=1 */
    vreg_regs[0] = (1u << 12) | (0xBu << 4) | 1u;
    /* BOD: enabled, VSEL=0x9 */
    vreg_regs[1] = (0x9u << 4) | 1u;
    /* CHIP_RESET: had_por=1 (we just powered on) */
    vreg_regs[2] = (1u << 8);
    vreg_initialized = 1;
}

int vreg_match(uint32_t addr) {
    uint32_t base = addr & ~0x3000u;
    return (base >= VREG_BASE && base < VREG_BASE + VREG_SIZE);
}

uint32_t vreg_read(uint32_t offset) {
    vreg_init_defaults();
    offset &= 0xFFF;
    if (offset >= VREG_SIZE) return 0;
    return vreg_regs[offset / 4];
}

void vreg_write(uint32_t offset, uint32_t val) {
    vreg_init_defaults();
    offset &= 0xFFF;
    if (offset >= VREG_SIZE) return;

    if (offset == 0x08) {
        /* CHIP_RESET: W1C — writing 1 clears the bit */
        vreg_regs[2] &= ~val;
    } else {
        vreg_regs[offset / 4] = val;
        /* Keep ROK always set when VREG is enabled */
        if (offset == 0x00 && (val & 1))
            vreg_regs[0] |= (1u << 12);
    }
}

/* ========================================================================
 * RP2350 Peripheral Stubs
 * ======================================================================== */

/* ========================================================================
 * RP2350 TRNG / SHA-256 / HSTX functional models
 * Register maps from pico-sdk regs/{trng,sha256,hstx_ctrl,hstx_fifo}.h.
 * ======================================================================== */

/* TRNG: Arm TrustZone RNG block. xorshift128+ entropy stream feeds a
 * 192-bit EHR (6 words). RND_SOURCE_ENABLE starts collection; when 6
 * words are ready VALID/EHR_VALID set, ISR EHR_VALID sets, IRQ 39 fires
 * if unmasked in IMR. Reading EHR words consumes them (new collection
 * starts if the source is still enabled). SW_RESET clears everything. */
static uint64_t trng_s[2] = { 0x9E3779B97F4A7C15ULL, 0xDEADBEEFCAFEBABEULL };
static uint32_t trng_imr = 0xFu; /* all masked at reset (no IRQs) */
static uint32_t trng_isr = 0;
static uint32_t trng_config = 0;
static uint32_t trng_src_en = 0;
static uint32_t trng_ehr[6] = { 0, 0, 0, 0, 0, 0 };
static int trng_ehr_valid = 0;
static int trng_ehr_idx = 0;

void trng_init(void) {
    trng_s[0] = 0x9E3779B97F4A7C15ULL;
    trng_s[1] = 0xDEADBEEFCAFEBABEULL;
    trng_imr = 0xFu;
    trng_isr = 0;
    trng_config = 0;
    trng_src_en = 0;
    trng_ehr_valid = 0;
    trng_ehr_idx = 0;
    memset(trng_ehr, 0, sizeof(trng_ehr));
}

static uint32_t trng_next(void) {
    /* xorshift128+ */
    uint64_t x = trng_s[0];
    uint64_t y = trng_s[1];
    trng_s[0] = y;
    x ^= x << 23;
    trng_s[1] = x ^ y ^ (x >> 17) ^ (y >> 26);
    return (uint32_t)(trng_s[1] + y);
}

static void trng_collect(void) {
    for (int i = 0; i < 6; i++) trng_ehr[i] = trng_next();
    trng_ehr_valid = 1;
    trng_ehr_idx = 0;
    trng_isr |= TRNG_ISR_EHR_VALID;
    if (!(trng_imr & (1u << 3)))
        nvic_signal_irq(39); /* TRNG_IRQ on RP2350 */
}

int trng_match(uint32_t addr) {
    uint32_t base = addr & ~0x3000u;
    return (base >= TRNG_BASE && base < TRNG_BASE + TRNG_SIZE);
}

uint32_t trng_read(uint32_t offset) {
    offset &= 0xFFF;
    switch (offset) {
    case TRNG_RNG_IMR: return trng_imr;
    case TRNG_RNG_ISR: return trng_isr;
    case TRNG_TRNG_CONFIG: return trng_config;
    case TRNG_TRNG_VALID: return trng_ehr_valid ? 1u : 0u;
    case TRNG_RND_SRC_EN: return trng_src_en;
    case TRNG_SAMPLE_CNT1: return 0;
    case TRNG_TRNG_BUSY: return (!trng_ehr_valid && trng_src_en) ? 1u : 0u;
    case TRNG_RNG_VERSION: return 0x00000000u;
    default: break;
    }
    if (offset >= TRNG_EHR_DATA0 && offset < TRNG_EHR_DATA0 + 24 &&
        ((offset - TRNG_EHR_DATA0) % 4) == 0) {
        int idx = (int)((offset - TRNG_EHR_DATA0) / 4);
        if (!trng_ehr_valid) {
            /* Reading before valid: return current word (silicon holds
             * stale data); do not advance. */
            return trng_ehr[idx];
        }
        uint32_t v = trng_ehr[idx];
        if (idx == 5) {
            /* Last word consumed: clear valid, start next collection
             * if the source is still enabled. */
            trng_ehr_valid = 0;
            trng_isr &= ~TRNG_ISR_EHR_VALID;
            if (trng_src_en) trng_collect();
        }
        return v;
    }
    return 0;
}

void trng_write(uint32_t offset, uint32_t val) {
    offset &= 0xFFF;
    switch (offset) {
    case TRNG_RNG_IMR: trng_imr = val & 0xFu; break;
    case TRNG_RNG_ICR:
        trng_isr &= ~val; /* W1C */
        break;
    case TRNG_TRNG_CONFIG: trng_config = val; break;
    case TRNG_RND_SRC_EN:
        trng_src_en = val & 1u;
        if (trng_src_en && !trng_ehr_valid) trng_collect();
        break;
    case 0x140: { /* TRNG_SW_RESET */
        uint32_t keep_imr = trng_imr;
        trng_init();
        trng_imr = keep_imr;
        break;
    }
    default: break;
    }
}

/* ---- SHA-256: real digest engine (FIPS 180-4, host computed) ---- */

static uint32_t sha256_h[8];
static uint32_t sha256_w[16];
static int sha256_nw = 0;
static uint32_t sha256_csr = SHA256_CSR_WDATA_RDY;
static int sha256_bswap = 1;

void sha256_init(void) {
    /* Initial hash values (fractional parts of square roots of primes) */
    sha256_h[0] = 0x6a09e667; sha256_h[1] = 0xbb67ae85;
    sha256_h[2] = 0x3c6ef372; sha256_h[3] = 0xa54ff53a;
    sha256_h[4] = 0x510e527f; sha256_h[5] = 0x9b05688c;
    sha256_h[6] = 0x1f83d9ab; sha256_h[7] = 0x5be0cd19;
    sha256_nw = 0;
    /* START forces WDATA_RDY and SUM_VLD high (silicon behavior). */
    sha256_csr = SHA256_CSR_WDATA_RDY | SHA256_CSR_SUM_VLD;
    sha256_bswap = 1;
}

static uint32_t sha256_rotr(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_compress_block(void) {
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = sha256_w[i];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = sha256_rotr(w[i-15], 7) ^ sha256_rotr(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = sha256_rotr(w[i-2], 17) ^ sha256_rotr(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a = sha256_h[0], b = sha256_h[1], c = sha256_h[2], d = sha256_h[3];
    uint32_t e = sha256_h[4], f = sha256_h[5], g = sha256_h[6], h = sha256_h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^ sha256_rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^ sha256_rotr(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    sha256_h[0] += a; sha256_h[1] += b; sha256_h[2] += c; sha256_h[3] += d;
    sha256_h[4] += e; sha256_h[5] += f; sha256_h[6] += g; sha256_h[7] += h;
}

int sha256_match(uint32_t addr) {
    uint32_t base = addr & ~0x3000u;
    return (base >= SHA256_BASE && base < SHA256_BASE + SHA256_SIZE);
}

uint32_t sha256_read(uint32_t offset) {
    offset &= 0xFFF;
    if (offset == SHA256_CSR) return sha256_csr;
    if (offset >= SHA256_SUM0 && offset < SHA256_SUM0 + 32 &&
        ((offset - SHA256_SUM0) % 4) == 0)
        return sha256_h[(offset - SHA256_SUM0) / 4];
    return 0;
}

void sha256_write(uint32_t offset, uint32_t val) {
    offset &= 0xFFF;
    if (offset == SHA256_CSR) {
        if (val & SHA256_CSR_START) sha256_init();
        /* BSWAP/DMA_SIZE accepted; ERR bit is W1C */
        if (val & SHA256_CSR_ERR_WDATA_NOT_RDY)
            sha256_csr &= ~SHA256_CSR_ERR_WDATA_NOT_RDY;
        return;
    }
    if (offset == SHA256_WDATA) {
        if (sha256_nw >= 16) {
            sha256_csr |= SHA256_CSR_ERR_WDATA_NOT_RDY;
            return;
        }
        uint32_t w = val;
        if (sha256_bswap)
            w = ((w >> 24) & 0xFF) | ((w >> 8) & 0xFF00) |
                ((w << 8) & 0xFF0000) | ((w << 24) & 0xFF000000);
        if (sha256_nw == 0) sha256_csr &= ~SHA256_CSR_SUM_VLD;
        sha256_w[sha256_nw++] = w;
        if (sha256_nw == 16) {
            sha256_compress_block();
            sha256_nw = 0;
            sha256_csr |= SHA256_CSR_SUM_VLD;
        }
        return;
    }
}

/* OTP: returns 0xFFFFFFFF (unprogrammed) */
int otp_match(uint32_t addr) {
    uint32_t base = addr & ~0x3000u;
    return (base >= OTP_BASE && base < OTP_BASE + OTP_SIZE);
}

uint32_t otp_read(uint32_t offset) {
    (void)offset;
    return 0xFFFFFFFF;  /* Blank/unprogrammed */
}

/* HSTX: functional serializer. CTRL regs (CSR/BIT0-7/EXPAND_*) read
 * back; the 8-deep command FIFO accepts words (FULL/WOF per silicon);
 * with EN set, popped FIFO words run through the optional command
 * expander (encoded/raw shifters + TMDS encoder) and the main shifter
 * (N_SHIFTS x SHIFT rotate-refills), and the resulting serial bits are
 * logged byte-wise into an output ring observable via hstx_pop_tx().
 * BITx CLK/INV/SEL mapping and coupled mode select which lanes consume
 * the stream; the model serializes lane 0 (LSB-first bit stream) which
 * is what DVI firmware validates with a logic probe. */
static uint32_t hstx_csr = 0x10050600u; /* silicon reset value */
static uint32_t hstx_bit[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
static uint32_t hstx_expand_shift = 0;
static uint32_t hstx_expand_tmds = 0;
static uint32_t hstx_fifo[HSTX_FIFO_DEPTH];
static int hstx_fifo_count = 0;
static int hstx_fifo_wof = 0;
/* Serialized output log (bytes, LSB-first bit packing of lane 0). */
static uint8_t hstx_tx[1024];
static int hstx_tx_wr = 0, hstx_tx_rd = 0, hstx_tx_n = 0;

void hstx_init(void) {
    hstx_csr = 0x10050600u;
    memset(hstx_bit, 0, sizeof(hstx_bit));
    hstx_expand_shift = 0;
    hstx_expand_tmds = 0;
    hstx_fifo_count = 0;
    hstx_fifo_wof = 0;
    hstx_tx_wr = hstx_tx_rd = hstx_tx_n = 0;
}

/* DVI TMDS encode of one 8-bit payload (10-bit symbol, running
 * disparity kept across calls like silicon). q_m + disparity choice
 * per DVI 1.0 §3.2. */
static int hstx_tmds_disp = 0;
static uint16_t hstx_tmds_encode(uint8_t d) {
    int n1 = __builtin_popcount(d);
    uint8_t q_m = 0;
    if (n1 > 4 || (n1 == 4 && !(d & 1))) {
        /* XNOR chain, bit8=0 */
        q_m = d ^ 0xFF; /* placeholder replaced below */
        uint8_t q = d;
        q_m = 0;
        uint8_t prev = q & 1;
        q_m |= prev;
        for (int i = 1; i < 8; i++) {
            uint8_t bit = (q >> i) & 1;
            prev = !(prev ^ bit);
            q_m |= (uint8_t)(prev << i);
        }
        /* bit8 = 0 for XNOR */
    } else {
        /* XOR chain, bit8=1 */
        uint8_t prev = d & 1;
        q_m = prev;
        for (int i = 1; i < 8; i++) {
            uint8_t bit = (d >> i) & 1;
            prev ^= bit;
            q_m |= (uint8_t)(prev << i);
        }
        q_m |= 0x100;
    }
    int n1q = __builtin_popcount(q_m & 0xFF);
    int balance = 2 * n1q - 8;
    uint16_t sym;
    if (hstx_tmds_disp == 0 || balance == 0) {
        if (q_m & 0x100) { sym = (uint16_t)(0x200 | q_m); hstx_tmds_disp += balance; }
        else { sym = (uint16_t)(0x100 | (q_m ^ 0xFF)); hstx_tmds_disp -= balance; }
        if (balance == 0 && !(q_m & 0x100)) hstx_tmds_disp = -hstx_tmds_disp;
    } else if ((hstx_tmds_disp > 0) == (balance > 0)) {
        sym = (uint16_t)(0x200 | ((~q_m) & 0x1FF));
        hstx_tmds_disp += (q_m & 0x100) ? -balance : balance;
        if (q_m & 0x100) hstx_tmds_disp += 2; else hstx_tmds_disp -= 2;
    } else {
        sym = (uint16_t)(q_m & 0x1FF);
        hstx_tmds_disp += (q_m & 0x100) ? balance : -balance;
        if (!(q_m & 0x100)) hstx_tmds_disp += 2; else hstx_tmds_disp -= 2;
    }
    return sym & 0x3FF;
}

static void hstx_tx_push_byte(uint8_t b) {
    if (hstx_tx_n >= (int)sizeof(hstx_tx)) return; /* drop when full */
    hstx_tx[hstx_tx_wr] = b;
    hstx_tx_wr = (hstx_tx_wr + 1) % (int)sizeof(hstx_tx);
    hstx_tx_n++;
}

int hstx_pop_tx(void) {
    if (hstx_tx_n == 0) return 0;
    int v = hstx_tx[hstx_tx_rd];
    hstx_tx_rd = (hstx_tx_rd + 1) % (int)sizeof(hstx_tx);
    hstx_tx_n--;
    return v ? v : 1; /* 0 byte reported as 1 to distinguish empty */
}

uint32_t hstx_tx_level(void) { return (uint32_t)hstx_tx_n; }

/* Run one FIFO word through the shifter + optional expander, logging
 * lane-0 serial bytes. Called on FIFO push while EN is set. */
static void hstx_serialize_word(uint32_t word) {
    uint32_t shift = (hstx_csr >> 8) & 0x1Fu;
    uint32_t n_shifts = (hstx_csr >> 16) & 0x1Fu;
    if (n_shifts == 0) n_shifts = 32;
    int expand = (hstx_csr & HSTX_CSR_EXPAND_EN) != 0;
    uint32_t sr = word;
    uint32_t acc = 0;
    int acc_n = 0;
    for (uint32_t s = 0; s < n_shifts; s++) {
        uint32_t out;
        if (expand) {
            /* Encoded path: low 8 bits of the rotated shifter feed
             * the TMDS encoder; symbol bits stream out. */
            uint8_t payload = (uint8_t)(sr & 0xFF);
            uint16_t sym = hstx_tmds_encode(payload);
            out = sym; /* 10 bits */
            for (int b = 0; b < 10; b++) {
                acc |= ((uint32_t)((out >> b) & 1)) << acc_n++;
                if (acc_n == 8) { hstx_tx_push_byte((uint8_t)acc); acc = 0; acc_n = 0; }
            }
        } else {
            out = (sr >> 31) & 1u; /* MSB-first raw lane bit */
            acc |= out << acc_n++;
            if (acc_n == 8) { hstx_tx_push_byte((uint8_t)acc); acc = 0; acc_n = 0; }
        }
        /* Rotate right by SHIFT (SHIFT=0 with expand = byte consume). */
        uint32_t r = expand ? 8u : (shift ? shift : 32u);
        r &= 31u;
        if (r) sr = (sr >> r) | (sr << (32 - r));
    }
    if (acc_n) hstx_tx_push_byte((uint8_t)acc);
}

int hstx_match(uint32_t addr) {
    uint32_t base = addr & ~0x3000u;
    if (base >= HSTX_BASE && base < HSTX_BASE + HSTX_SIZE) return 1;
    return (base >= HSTX_FIFO_BASE && base < HSTX_FIFO_BASE + HSTX_FIFO_SIZE);
}

uint32_t hstx_read(uint32_t offset) {
    /* FIFO block reads use the high base; callers pass addr & 0xFFF so
     * distinguish by size: offsets here are within-block. The membus
     * routes 0x50600xxx here too — STAT at +0, data writes at +4. */
    offset &= 0xFFF;
    if (offset == HSTX_FIFO_STAT_OFF + 0x00 && hstx_fifo_count >= 0 &&
        offset < HSTX_SIZE) {
        /* Ambiguous +0: CTRL CSR lives at +0 and FIFO STAT at +0 of the
         * other block. Reads via the CTRL base return CSR; the FIFO
         * STAT is only reachable via the FIFO base (handled by checking
         * the caller's block in hstx_match — keep CSR here). */
    }
    if (offset == HSTX_CSR_OFF) return hstx_csr;
    if (offset >= 0x04 && offset <= 0x20 && ((offset - 0x04) % 4) == 0)
        return hstx_bit[(offset - 0x04) / 4];
    if (offset == HSTX_EXPAND_SHIFT_OFF) return hstx_expand_shift;
    if (offset == HSTX_EXPAND_TMDS_OFF) return hstx_expand_tmds;
    return 0;
}

/* FIFO-block read (STAT). Separate entry so the +0 ambiguity above
 * resolves by block base, not offset. */
uint32_t hstx_fifo_read(uint32_t offset) {
    offset &= 0xFFF;
    if (offset == HSTX_FIFO_STAT_OFF) {
        uint32_t v = (uint32_t)(hstx_fifo_count & 0xFF);
        if (hstx_fifo_count == 0) v |= HSTX_FIFO_STAT_EMPTY;
        if (hstx_fifo_count >= HSTX_FIFO_DEPTH) v |= HSTX_FIFO_STAT_FULL;
        if (hstx_fifo_wof) v |= HSTX_FIFO_STAT_WOF;
        return v;
    }
    return 0;
}

void hstx_write(uint32_t offset, uint32_t val) {
    offset &= 0xFFF;
    if (offset == HSTX_CSR_OFF) { hstx_csr = val; return; }
    if (offset >= 0x04 && offset <= 0x20 && ((offset - 0x04) % 4) == 0) {
        hstx_bit[(offset - 0x04) / 4] = val;
        return;
    }
    if (offset == HSTX_EXPAND_SHIFT_OFF) { hstx_expand_shift = val; return; }
    if (offset == HSTX_EXPAND_TMDS_OFF) { hstx_expand_tmds = val; return; }
}

/* FIFO-block write (data push / WOF clear). */
void hstx_fifo_write(uint32_t offset, uint32_t val) {
    offset &= 0xFFF;
    if (offset == HSTX_FIFO_STAT_OFF) {
        if (val & HSTX_FIFO_STAT_WOF) hstx_fifo_wof = 0;
        return;
    }
    if (offset == HSTX_FIFO_FIFO_OFF) {
        if (hstx_fifo_count >= HSTX_FIFO_DEPTH) { hstx_fifo_wof = 1; return; }
        hstx_fifo[hstx_fifo_count++] = val;
        if (hstx_csr & HSTX_CSR_EN) {
            /* Shift out immediately (functional model: no clock
             * gating — data appears in the TX log at write time). */
            hstx_fifo_count--;
            memmove(hstx_fifo, hstx_fifo + 1,
                    (size_t)hstx_fifo_count * sizeof(uint32_t));
            hstx_serialize_word(val);
        }
        return;
    }
}

/* TICKS: stub — tick generator returns configured values */
static uint32_t ticks_regs[TICKS_SIZE / 4];

int ticks_match(uint32_t addr) {
    uint32_t base = addr & ~0x3000u;
    return (base >= TICKS_BASE && base < TICKS_BASE + TICKS_SIZE);
}

uint32_t ticks_read(uint32_t offset) {
    offset &= 0xFFF;
    if (offset >= TICKS_SIZE) return 0;
    return ticks_regs[offset / 4];
}

void ticks_write(uint32_t offset, uint32_t val) {
    offset &= 0xFFF;
    if (offset < TICKS_SIZE) ticks_regs[offset / 4] = val;
}
