#include "touch.h"
#include "spi.h"
#include "lcd.h"
#include "timer.h"
/* Multitouch controller on serial port 2 (NanoEmu nano7_touch.c, protocol
 * of RetailOS's driver, workers/codex-touch.txt):
 *   query  16 bytes  EA 01 00 ...  -> EA, LE16 L (0x24 when a report
 *                                     waits, else 0), LE16 sum at 14
 *   fetch  L+5 bytes EA 01 01 ...  -> 5-byte header (sums to 0), payload,
 *                                     LE16 payload sum; one report each
 * Contact record at payload 0x12 (rx 23): id, state (3 down, 4 move,
 * 5 up), type, pad, s16 raw X, s16 raw Y. Panel pixels (portrait 240x432):
 *   px = (rawX + 75) * 239 / 2845,  py = 431 - (rawY + 75) * 431 / 5250
 * The ready line (GPIO 0x26) needs the GPIO IRQ group armed; with IRQs off
 * the query alone tells whether a report waits. */
#define POLL_US 8000u
#define XFER_US 2000u
#define FETCH_MAX 64u
struct touch_state touch;
static uint32_t last;
static int clamp(int v, int hi) { return v < 0 ? 0 : v > hi ? hi : v; }
static unsigned sum(const uint8_t *p, unsigned n) {
    unsigned s = 0;
    while (n--) s += *p++;
    return s;
}
/* 1 = report read, 0 = none waiting, -1 = transfer or checksum error */
static int read_one(void) {
    uint8_t tx[FETCH_MAX], rx[FETCH_MAX];
    for (unsigned i = 0; i < FETCH_MAX; ++i) tx[i] = 0;   /* no memset */
    tx[0] = 0xea;
    tx[1] = 0x01;
    if (!spi_xfer(SPI_TOUCH, tx, rx, 16, XFER_US)) return -1;
    if (rx[0] != 0xea || (sum(rx, 14) & 0xffff) != (unsigned)(rx[14] | rx[15] << 8))
        return -1;
    unsigned l = rx[1] | rx[2] << 8;
    if (!l) return 0;
    if (l + 5 > FETCH_MAX || l < 0x1a) return -1;
    tx[2] = 0x01;
    if (!spi_xfer(SPI_TOUCH, tx, rx, l + 5, XFER_US)) return -1;
    if (rx[0] != 0xea || (sum(rx, 5) & 0xff) ||
        (sum(rx + 5, l - 2) & 0xffff) != (unsigned)(rx[l + 3] | rx[l + 4] << 8))
        return -1;
    const uint8_t *rec = rx + 5 + rx[7];   /* payload[2] = record offset */
    if (rx[7] + 8u > l - 2) return -1;
    int rawx = (int16_t)(rec[4] | rec[5] << 8);
    int rawy = (int16_t)(rec[6] | rec[7] << 8);
    int px = clamp((rawx + 75) * 239 / 2845, LCD_PANEL_WIDTH - 1);
    int py = clamp(431 - (rawy + 75) * 431 / 5250, LCD_PANEL_HEIGHT - 1);
    /* lcd.c: logical (x, y) -> panel (239 - y, x) */
    touch.x = py;
    touch.y = LCD_PANEL_WIDTH - 1 - px;
    touch.state = rec[1];
    ++touch.reports;
    return 1;
}
void touch_init(void) {
    for (unsigned i = 0; i < 40 && read_one() > 0; ++i) {}
    touch.reports = touch.errors = 0;
    touch.state = 0;
    last = USEC;
}
int touch_poll(void) {
    if (USEC - last < POLL_US) return 0;
    last = USEC;
    int r = read_one();
    if (r < 0) ++touch.errors;
    return r > 0;
}
