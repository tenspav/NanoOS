#pragma GCC optimize("Os")   /* polled, no hot path: kernel.bin must fit 64 KB */
#include "spi.h"
#include "timer.h"
/* RetailOS's polled path (0x08400430 -> 0x083f4ee8, taken with IRQs masked,
 * as here); round 2, moved from touch.c so the codec uses it too. The old
 * START handshake (+0x04 bit 8) is the IRQ path without its TX/RX enables
 * (+0x04 0x200000 / 0x1080, serviced by the IRQ handler): "done" never came
 * on the device, for touch (port 2) and the codec (SPI0) alike.
 *   +0x00 bits 2/3 flush both FIFOs
 *   +0x04 bits 5/6 cleared = polled mode (bit 5 = IRQ mode, left set by
 *         RetailOS's set-up), bit 0 cleared = TX + RX, bit 22 restored after
 *   +0x08 status: bits 15:11 RX level, 10:6 TX level, bit 22 set before
 *   +0x0c chip select, bit1 set = deselected
 *   +0x10 TX byte, +0x20 RX byte, +0x34 / +0x4c per-byte lengths
 * FIFOs flushed and drained, then per byte +0x34 = 1, TX empty, +0x10 =
 * byte, +0x4c = 1, RX non-empty, pop.
 * Round 12: the touch port gets CS setup/hold/high delays (spi.h). */
#define R(b, o) (*(volatile uint32_t *)((b) + (o)))
uint32_t spi_timeouts;
int spi_xfer(uint32_t base, const uint8_t *tx, uint8_t *rx, unsigned n,
             uint32_t us) {
    volatile uint32_t *st = &R(base, 0x08);
    int ok = 0, touch = base == SPI_TOUCH;
    R(base, 0x0c) &= ~2u;               /* select */
    if (touch) delay_us(TOUCH_CS_SETUP_US);
    R(base, 0x04) &= ~0x60u;            /* polled mode */
    R(base, 0x00) |= 0xcu;              /* flush; levels must read 0 */
    if (wait_until(st, 0xffc0u, 0, us) != WAIT_OK) goto out;
    R(base, 0x04) &= ~1u;               /* TX + RX */
    *st |= 0x400000u;
    for (unsigned i = 0; i < n; ++i) {
        R(base, 0x34) = 1;
        if (wait_until(st, 0x7c0u, 0, us) != WAIT_OK) goto out;
        R(base, 0x10) = tx[i];
        R(base, 0x4c) = 1;
        if (wait_any(st, 0xf800u, us) != WAIT_OK) goto out;
        rx[i] = (uint8_t)R(base, 0x20);
    }
    ok = 1;                             /* last byte in: TX is empty too */
out:
    if (!ok) ++spi_timeouts;
    if (touch) delay_us(TOUCH_CS_HOLD_US);
    R(base, 0x04) &= 0xffbffffeu;
    R(base, 0x0c) |= 2u;                /* deselect: the touch controller
                                         * releases its ready line here */
    if (touch) delay_us(TOUCH_CS_HIGH_US);   /* before the next select */
    return ok;
}
