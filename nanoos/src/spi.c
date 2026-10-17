#include "spi.h"
#include "timer.h"
/* Register use (NanoEmu nano7_touch.c:221-268, nano7_codec.c:122-164):
 *   +0x00 control: writing bits 2/3 flushes both FIFOs
 *   +0x04 bit8 START, taken on a 0 -> 1 edge (ignored while busy)
 *   +0x08 status, W1C: bit0 done, bit5 RX data, bits 15:11 RX level
 *   +0x0c chip select, bit1 set = deselected
 *   +0x10 TX byte, +0x20 RX byte, +0x34 / +0x4c transfer length
 * The other bits RetailOS left in +0x00/+0x04/+0x0c (mode, clock, CS
 * polarity) are kept: only the named bits are touched. */
#define R(b, o) (*(volatile uint32_t *)((b) + (o)))
uint32_t spi_timeouts;
int spi_xfer(uint32_t base, const uint8_t *tx, uint8_t *rx, unsigned n,
             uint32_t us) {
    uint32_t ctrl = R(base, 0x00);
    R(base, 0x00) = ctrl | 0xcu;        /* flush */
    R(base, 0x00) = ctrl;
    R(base, 0x08) = 0xffffffffu;
    R(base, 0x0c) &= ~2u;               /* select */
    R(base, 0x34) = n;
    R(base, 0x4c) = n;
    R(base, 0x04) &= ~0x100u;
    R(base, 0x04) |= 0x100u;            /* START */
    for (unsigned i = 0; i < n; ++i) R(base, 0x10) = tx[i];
    int ok = wait_any(&R(base, 0x08), 1, us) == WAIT_OK;
    if (ok) for (unsigned i = 0; i < n; ++i) rx[i] = (uint8_t)R(base, 0x20);
    else ++spi_timeouts;
    R(base, 0x08) = 0xffffffffu;
    R(base, 0x04) &= ~0x100u;
    R(base, 0x0c) |= 2u;                /* deselect: the touch controller
                                         * releases its ready line here */
    return ok;
}
