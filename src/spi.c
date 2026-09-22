/*
 * RP2040 PL022 SPI Controller
 *
 * Two SPI instances with 8-deep TX/RX FIFOs. Writes to SSPDR push to the
 * TX FIFO and immediately execute a full-duplex transfer through the attached
 * device callback (if any). The response byte is pushed to the RX FIFO.
 *
 * When no device is attached, TX data is discarded and RX returns 0x00
 * (matching pull-up behavior on an unconnected bus).
 */

#include <string.h>
#include "spi.h"
#include "nvic.h"
#include "emulator.h"

spi_state_t spi_state[2];

static void spi_reset_instance(spi_state_t *s) {
    memset(s, 0, sizeof(*s));
}

void spi_init(void) {
    for (int i = 0; i < 2; i++) {
        spi_reset_instance(&spi_state[i]);
    }
}

int spi_match(uint32_t addr) {
    uint32_t base = addr & ~0x3000;
    /* RP2350 bases first when in RP2350 mode (they differ from RP2040) */
    if (membus_rp2350_mode) {
        if (base >= RP2350_SPI0_BASE && base < RP2350_SPI0_BASE + SPI_BLOCK_SIZE)
            return 0;
        if (base >= RP2350_SPI1_BASE && base < RP2350_SPI1_BASE + SPI_BLOCK_SIZE)
            return 1;
    }
    if (base >= SPI0_BASE && base < SPI0_BASE + SPI_BLOCK_SIZE)
        return 0;
    if (base >= SPI1_BASE && base < SPI1_BASE + SPI_BLOCK_SIZE)
        return 1;
    return -1;
}

/* ========================================================================
 * FIFO helpers
 * ======================================================================== */

static void tx_push(spi_state_t *s, uint16_t val) {
    if (s->tx_count >= SPI_FIFO_SIZE) return;
    s->tx_fifo[s->tx_head] = val;
    s->tx_head = (s->tx_head + 1) % SPI_FIFO_SIZE;
    s->tx_count++;
}

static uint16_t tx_pop(spi_state_t *s) {
    if (s->tx_count == 0) return 0;
    uint16_t val = s->tx_fifo[s->tx_tail];
    s->tx_tail = (s->tx_tail + 1) % SPI_FIFO_SIZE;
    s->tx_count--;
    return val;
}

static void rx_push(spi_state_t *s, uint16_t val) {
    if (s->rx_count >= SPI_FIFO_SIZE) {
        s->ris |= SPI_INT_ROR;  /* RX overrun */
        return;
    }
    s->rx_fifo[s->rx_head] = val;
    s->rx_head = (s->rx_head + 1) % SPI_FIFO_SIZE;
    s->rx_count++;
}

static uint16_t rx_pop(spi_state_t *s) {
    if (s->rx_count == 0) return 0;
    uint16_t val = s->rx_fifo[s->rx_tail];
    s->rx_tail = (s->rx_tail + 1) % SPI_FIFO_SIZE;
    s->rx_count--;
    return val;
}

/* ========================================================================
 * Transfer execution
 * ======================================================================== */

/* Execute all pending TX frames through the device callback immediately.
 * Frame width follows CR0.DSS (PL022: 4-16 bits): 8-bit guests push one
 * byte per DR write, 16-bit guests push one halfword (the W5500 Arduino
 * driver sets DSS=15 via transfer16 paths — masking to 8 bits desyncs
 * every VERSIONR/SR read and eth.begin fails). The W5500 xfer callback
 * is byte-oriented, so 16-bit frames execute as two byte transfers
 * (MSB first, matching MSBFIRST wire order). */
static void spi_execute_transfers(spi_state_t *s) {
    int wide = ((s->cr0 & 0xF) >= 8);  /* DSS>8: 9-16 bit frames */
    while (s->tx_count > 0) {
        uint16_t mosi = tx_pop(s);
        if (s->device.xfer) {
            if (wide) {
                uint16_t m1 = s->device.xfer(s->device.ctx, (uint8_t)(mosi >> 8));
                uint16_t m0 = s->device.xfer(s->device.ctx, (uint8_t)(mosi & 0xFF));
                rx_push(s, (uint16_t)((m1 << 8) | (m0 & 0xFF)));
            } else {
                uint16_t miso = s->device.xfer(s->device.ctx, (uint8_t)mosi);
                rx_push(s, miso);
            }
        } else {
            rx_push(s, 0);
        }
    }
}

/* Mirror a CS line change into the attached device (the board watches
 * GPIO17/20 via the CS/RST edge path, but the PL022 device callback
 * model has no GPIO line — RV32 SIO writes never reach gpio_write32,
 * so RV32 guests must report CS through here). Harmless for ARM: the
 * gpio.c watch dedups via edge memory (same level = no-op). */
void spi_device_cs(int spi_num, int cs_active) {
    if (spi_num < 0 || spi_num > 1) return;
    spi_state_t *s = &spi_state[spi_num];
    if (s->device.cs)
        s->device.cs(s->device.ctx, cs_active);
}

/* Update interrupt status based on FIFO state */
static void spi_update_irq(int spi_num) {
    spi_state_t *s = &spi_state[spi_num];

    /* TX FIFO half-empty or less: assert TX interrupt */
    if (s->tx_count <= SPI_FIFO_SIZE / 2) {
        s->ris |= SPI_INT_TX;
    } else {
        s->ris &= ~SPI_INT_TX;
    }

    /* RX FIFO half-full or more: assert RX interrupt */
    if (s->rx_count >= SPI_FIFO_SIZE / 2) {
        s->ris |= SPI_INT_RX;
    } else {
        s->ris &= ~SPI_INT_RX;
    }

    if (s->ris & s->imsc) {
        nvic_signal_rp2350_irq(spi_num == 0 ? IRQ_SPI0_IRQ : IRQ_SPI1_IRQ);
    }
}

/* ========================================================================
 * Register access
 * ======================================================================== */

uint32_t spi_read32(int spi_num, uint32_t offset) {
    spi_state_t *s = &spi_state[spi_num];

    switch (offset) {
    case SPI_SSPCR0:
        return s->cr0;

    case SPI_SSPCR1:
        return s->cr1;

    case SPI_SSPDR:
        /* Read pops from RX FIFO */
        {
            uint16_t val = rx_pop(s);
            spi_update_irq(spi_num);
            return val;
        }

    case SPI_SSPSR: {
        /* PL022 status bits (RP2350 datasheet): TFE=0, TNF=1, RNE=2,
         * RFF=3, BSY=4. The old model had TNF/RNE/RFF/BSY shifted by
         * one (1,2,3,4 → TFE collided with TNF), so the SDK's
         * spi_write_read_blocking spun forever: TNF-at-2 never set
         * (TX path dead) and RNE-at-3 never set (RX path dead) — the
         * M33 W5500 eth.begin never clocked a byte. */
        uint32_t sr = 0;
        if (s->tx_count == 0)             sr |= SPI_SSPSR_TFE;
        if (s->tx_count < SPI_FIFO_SIZE)  sr |= SPI_SSPSR_TNF;
        if (s->rx_count > 0)              sr |= SPI_SSPSR_RNE;
        if (s->rx_count >= SPI_FIFO_SIZE) sr |= SPI_SSPSR_RFF;
        /* BSY: set when TX FIFO is not empty (transfer in progress) */
        if (s->tx_count > 0)              sr |= SPI_SSPSR_BSY;
        return sr;
    }

    case SPI_SSPCPSR:
        return s->cpsr;

    case SPI_SSPIMSC:
        return s->imsc;

    case SPI_SSPRIS:
        return s->ris;

    case SPI_SSPMIS:
        return s->ris & s->imsc;

    case SPI_SSPDMACR:
        return s->dmacr;

    /* PL022 Peripheral ID */
    case SPI_PERIPHID0: return 0x22;
    case SPI_PERIPHID1: return 0x10;
    case SPI_PERIPHID2: return 0x04;
    case SPI_PERIPHID3: return 0x00;

    default:
        return 0;
    }
}

void spi_write32(int spi_num, uint32_t offset, uint32_t val) {
    spi_state_t *s = &spi_state[spi_num];

    switch (offset) {
    case SPI_SSPCR0:
        s->cr0 = val & 0xFFFF;
        break;

    case SPI_SSPCR1:
        s->cr1 = val & 0x0F;
        break;

    case SPI_SSPDR:
        /* TX write: push one FRAME to TX FIFO, then execute. Frame width
         * follows CR0.DSS: 8-bit mode pushes the low byte, 9-16-bit modes
         * push the low halfword (mask to 16 bits so a word store carrying
         * garbage above bit 15 cannot corrupt the wire frame). */
        if (s->cr1 & SPI_CR1_SSE) {
            int wide = ((s->cr0 & 0xF) >= 8);
            tx_push(s, wide ? (uint16_t)(val & 0xFFFF) : (uint16_t)(val & 0xFF));
            spi_execute_transfers(s);
        }
        break;

    case SPI_SSPCPSR:
        s->cpsr = val & 0xFF;
        break;

    case SPI_SSPIMSC:
        s->imsc = val & 0x0F;
        break;

    case SPI_SSPICR:
        s->ris &= ~(val & 0x03);
        break;

    case SPI_SSPDMACR:
        s->dmacr = val & 0x03;
        break;

    default:
        break;
    }

    spi_update_irq(spi_num);
}

void spi_attach_device(int spi_num, spi_device_xfer_fn xfer,
                        spi_device_cs_fn cs, void *ctx) {
    if (spi_num < 0 || spi_num > 1) return;
    spi_state[spi_num].device.xfer = xfer;
    spi_state[spi_num].device.cs = cs;
    spi_state[spi_num].device.ctx = ctx;
}
