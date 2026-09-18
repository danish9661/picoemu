#ifndef ADC_H
#define ADC_H

#include <stdint.h>

/* ========================================================================
 * RP2040 ADC (Analog-to-Digital Converter)
 * 4 external channels + 1 internal temperature sensor
 * ======================================================================== */

/* ADC Base Address */
#define ADC_BASE                0x4004C000

/* RP2350 moved ADC (same block + aliases; register layout identical) */
#ifndef RP2350_ADC_BASE
#define RP2350_ADC_BASE         0x400A0000
#endif

/* ADC Registers */
#define ADC_CS                  (ADC_BASE + 0x00)  /* Control and status */
#define ADC_RESULT              (ADC_BASE + 0x04)  /* Conversion result */
#define ADC_FCS                 (ADC_BASE + 0x08)  /* FIFO control/status */
#define ADC_FIFO_REG            (ADC_BASE + 0x0C)  /* FIFO read */
#define ADC_DIV                 (ADC_BASE + 0x10)  /* Clock divider */
#define ADC_INTR                (ADC_BASE + 0x14)  /* Raw interrupts */
#define ADC_INTE                (ADC_BASE + 0x18)  /* Interrupt enable */
#define ADC_INTF                (ADC_BASE + 0x1C)  /* Interrupt force */
#define ADC_INTS                (ADC_BASE + 0x20)  /* Interrupt status */

/* ADC CS bits */
#define ADC_CS_EN               (1u << 0)   /* ADC enable */
#define ADC_CS_TS_EN            (1u << 1)   /* Temperature sensor enable */
#define ADC_CS_START_ONCE       (1u << 2)   /* Start single conversion */
#define ADC_CS_START_MANY       (1u << 3)   /* Start free-running conversions */
#define ADC_CS_READY            (1u << 8)   /* Conversion complete */
#define ADC_CS_ERR              (1u << 9)   /* Conversion error */
#define ADC_CS_ERR_STICKY       (1u << 10)  /* Sticky error bit */
#define ADC_CS_AINSEL_SHIFT     12
/* RP2040/RP2350A: 3-bit AINSEL (0-3 GPIO + 4 temp). RP2350B: 4-bit
 * AINSEL (0-7 GPIO40-47 + 8 temp). Model covers the full 4-bit field
 * everywhere (RP2040 guests never select >4; RP2350B selects 5-8). */
#define ADC_CS_AINSEL_MASK      (0xFu << ADC_CS_AINSEL_SHIFT)
#define ADC_CS_RROBIN_SHIFT     16
/* RROBIN is 5 bits on RP2040/A (mask 0x1F) and 9 bits on RP2350B
 * (mask 0x1FF, one bit per mux input 0-8). Full 9-bit field modeled. */
#define ADC_CS_RROBIN_MASK      (0x1FFu << ADC_CS_RROBIN_SHIFT)

/* ADC FCS bits */
#define ADC_FCS_EN              (1u << 0)   /* FIFO enable */
#define ADC_FCS_SHIFT           (1u << 1)   /* Right-shift result to 8 bits */
#define ADC_FCS_ERR             (1u << 2)   /* Include error bit in FIFO */
#define ADC_FCS_DREQ_EN         (1u << 3)   /* Assert DMA request */
#define ADC_FCS_EMPTY           (1u << 8)   /* FIFO is empty (read-only) */
#define ADC_FCS_FULL            (1u << 9)   /* FIFO is full (read-only) */
#define ADC_FCS_UNDER           (1u << 10)  /* Underflow (W1C) */
#define ADC_FCS_OVER            (1u << 11)  /* Overflow (W1C) */
#define ADC_FCS_LEVEL_SHIFT     16
#define ADC_FCS_LEVEL_MASK      (0xFu << ADC_FCS_LEVEL_SHIFT)
/* RP2350 moved THRESH 24->27 (4 bits kept). Accept both positions on
 * write; report at 24 (RP2040 position) like before. */
#define ADC_FCS_THRESH_SHIFT    24
#define ADC_FCS_THRESH_MASK     (0xFu << ADC_FCS_THRESH_SHIFT)
#define ADC_FCS_THRESH_SHIFT_RP2350 27
#define ADC_FCS_THRESH_MASK_RP2350  (0xFu << ADC_FCS_THRESH_SHIFT_RP2350)

/* Number of ADC channels.
 * RP2040 / RP2350A (QFN-60): 5 (GPIO 26-29 + temp on mux input 4).
 * RP2350B (QFN-80): 9 (GPIO 40-47 on mux inputs 0-7 + temp on 8).
 * The model always implements the full 9-input mux (SDK: NUM_ADC_CHANNELS
 * 5 on A / 9 on B, ADC_BASE_PIN 26 / 40); A-package guests never select
 * inputs 5-8. */
#define ADC_NUM_CHANNELS        9
#define ADC_NUM_CHANNELS_RP2040 5
#define ADC_TEMP_CHANNEL        4   /* RP2040/A temp input */
#define ADC_TEMP_CHANNEL_RP2350B 8  /* RP2350B temp input */

/* ADC FIFO depth: 4 entries on RP2040, 8 on RP2350 (SDK struct shows
 * 8-element FIFO; datasheet §12.4). Model implements 8; RP2040 guests
 * see FULL at 4 via the depth gate below. */
#define ADC_FIFO_DEPTH          8
#define ADC_FIFO_DEPTH_RP2040   4

/* ADC state */
typedef struct {
    uint32_t cs;                /* Control/status */
    uint32_t fcs;               /* FIFO control/status (writable bits only) */
    uint32_t div;               /* Clock divider */
    uint32_t intr;              /* Raw interrupts */
    uint32_t inte;              /* Interrupt enable */
    uint16_t channel_values[ADC_NUM_CHANNELS]; /* Per-channel values (12-bit) */

    /* Effective FIFO depth for FULL reporting (4 on RP2040, 8 on RP2350).
     * Set by adc_reset() from membus_rp2350_mode; tests can override. */
    uint8_t fifo_depth;

    /* FIFO */
    uint16_t fifo[ADC_FIFO_DEPTH];  /* Circular buffer (12-bit results) */
    uint8_t  fifo_rd;               /* Read pointer */
    uint8_t  fifo_wr;               /* Write pointer */
    uint8_t  fifo_count;            /* Number of entries */
    uint8_t  fifo_under;            /* Underflow flag */
    uint8_t  fifo_over;             /* Overflow flag */
} adc_state_t;

/* Functions */
void adc_init(void);
void adc_reset(void);
uint32_t adc_read32(uint32_t addr);
void adc_write32(uint32_t addr, uint32_t val);

/* Set a channel's analog value (for testing or external injection) */
void adc_set_channel_value(uint8_t channel, uint16_t value);

/* Perform one ADC conversion (called when START_ONCE or free-running) */
void adc_do_conversion(void);

extern adc_state_t adc_state;

#endif /* ADC_H */
