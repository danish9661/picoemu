#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

/* Timer Base Address (RP2040 map; RP2350 TIMER0 lives at 0x400B0000 —
 * see RP2350_TIMER0_BASE in rp2350_memmap.h. The RP2350 timer map inserts
 * LOCKED/SOURCE at +0x34/+0x38, shifting INTR/INTE/INTF/INTS by +8; the
 * TIMER0 redirect in rp2350_periph.c translates those (see below). The
 * shared model itself stays on the RP2040 map.) */
#define TIMER_BASE          0x40054000

/* Timer Registers */
#define TIMER_TIMEHW        (TIMER_BASE + 0x00)  /* Write high word */
#define TIMER_TIMELW        (TIMER_BASE + 0x04)  /* Write low word */
#define TIMER_TIMEHR        (TIMER_BASE + 0x08)  /* Read high word */
#define TIMER_TIMELR        (TIMER_BASE + 0x0C)  /* Read low word */
#define TIMER_ALARM0        (TIMER_BASE + 0x10)  /* Alarm 0 */
#define TIMER_ALARM1        (TIMER_BASE + 0x14)  /* Alarm 1 */
#define TIMER_ALARM2        (TIMER_BASE + 0x18)  /* Alarm 2 */
#define TIMER_ALARM3        (TIMER_BASE + 0x1C)  /* Alarm 3 */
#define TIMER_ARMED         (TIMER_BASE + 0x20)  /* Armed alarms */
#define TIMER_TIMERAWH      (TIMER_BASE + 0x24)  /* Raw high word */
#define TIMER_TIMERAWL      (TIMER_BASE + 0x28)  /* Raw low word */
#define TIMER_DBGPAUSE      (TIMER_BASE + 0x2C)  /* Debug pause */
#define TIMER_PAUSE         (TIMER_BASE + 0x30)  /* Pause timer */
#define TIMER_INTR          (TIMER_BASE + 0x34)  /* Raw interrupt status */
#define TIMER_INTE          (TIMER_BASE + 0x38)  /* Interrupt enable */
#define TIMER_INTF          (TIMER_BASE + 0x3C)  /* Interrupt force */
#define TIMER_INTS          (TIMER_BASE + 0x40)  /* Interrupt status */
/* RP2350-map LOCKED/SOURCE live at the RP2040 INTR/INTE offsets (+0x34 /
 * +0x38). They only ever arrive here via the TIMER0 redirect in
 * rp2350_periph.c (translated back to LOCKED_RP2350/SOURCE_RP2350); the
 * shared model ignores them (timer never locks, always clk_sys). */
#define TIMER_LOCKED_RP2350 (TIMER_BASE + 0x100)
#define TIMER_SOURCE_RP2350 (TIMER_BASE + 0x104)

/* Timer state */
typedef struct {
    uint64_t time_us;        /* Current time in microseconds */
    uint32_t alarm[4];       /* 4 alarm compare values */
    uint32_t armed;          /* Which alarms are armed */
    uint32_t intr;           /* Raw interrupt status */
    uint32_t inte;           /* Interrupt enable */
    uint32_t intf;           /* Interrupt force */
    uint32_t paused;         /* Timer paused flag */
    uint32_t timehw_latch;   /* M1: TIMEHW latched high word, applied on TIMELW */
} timer_state_t;

/* Functions */
void timer_init(void);
void timer_reset(void);
void timer_tick(uint32_t cycles);  /* Update timer based on CPU cycles */
uint32_t timer_next_wakeup_us(void);  /* µs until next armed alarm (WFI fast-forward) */
uint32_t timer_read32(uint32_t addr);
void timer_write32(uint32_t addr, uint32_t val);

extern timer_state_t timer_state;

#endif /* TIMER_H */
