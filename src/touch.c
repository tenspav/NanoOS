#pragma GCC optimize("Os", "no-tree-loop-distribute-patterns")   /* polled, no hot path: kernel.bin must fit 64 KB; no memset */
#include "touch.h"
#include "spi.h"
#include "lcd.h"
#include "timer.h"
#include "fmt.h"
#include "report.h"
#include "input.h"
/* Multitouch controller ("Grape", TI 343S0538) on serial port 2 (NanoEmu
 * nano7_touch.c, protocol of RetailOS's driver, workers/codex-touch.txt):
 *   query  16 bytes  EA 01 00 0.. EB 00 -> EA, LE16 L (0x24 when a report
 *                                     waits, else 0), LE16 sum at 14
 *   fetch  L+5 bytes EA 01 01 0.. EC 00 -> 5-byte header (sums to 0), payload,
 *                                     LE16 payload sum; one report each
 * Both requests end in the LE16 sum of their bytes 0..13 (RetailOS
 * 0x0817db98 / 0x08179b18).
 * Contact record at payload 0x12 (rx 23): id, state (3 down, 4 move,
 * 5 up), type, pad, s16 raw X, s16 raw Y. Panel pixels (portrait 240x432):
 *   px = (rawX + 75) * 239 / 2845,  py = 431 - (rawY + 75) * 431 / 5250
 * Round 2 (device: "TOUCH ERR n 0 er 3249", every read failed; SPI0's
 * codec read through spi_xfer() times out the same way):
 *  - Transfers: RetailOS's 0x083f4ee8 takes a polled per-byte path when
 *    IRQs are masked (as here) and an IRQ path otherwise. spi_xfer() is the
 *    IRQ path's START handshake without its TX/RX enables (+0x04 0x200000 /
 *    0x1080, which the IRQ handler services), so "done" never came.
 *    spi_xfer() is now the polled path, shared with the codec (spi.c):
 *    CTRL bits 5/6 cleared (bit 5 = IRQ mode, left set by RetailOS's
 *    set-up), FIFOs flushed and drained, then per byte +0x34 = 1, TX
 *    empty, +0x10 = byte, +0x4c = 1, RX non-empty, pop.
 *  - Ready line (ATN, GPIO 0x26, level 0x3cf00084 bit 6, active low):
 *    RetailOS queries only after it fell (event 0x43), then re-arms it as
 *    0x083f6f3c(0x26, 1): group 1 bit 30 in 0x397000e4 (type, kept) / c4
 *    (enable) / a4 (pending, W1C). No wake GPIO or command exists (0x27 is
 *    reset, only in the power-on). Polls here wait for ATN low, plus one
 *    unconditional query every 128 polls (~1 s) in case the level reads
 *    wrong; the IRQ itself stays off (VIC masked by the chainloader).
 *  - Clock: gate 0x1e (CLKCON +0x4c bit 15, +0x6c bit 15) is port 2's
 *    (0x08010cb2), left on by RetailOS; cleared again here. Round 1's
 *    "gate 0x1c" is a device-power request id (0x08426e80), not this port.
 * The controller keeps running from RetailOS: the chainloader does not touch
 * its supply (PMU 0x73 reg 0x10 bit 5 / 0x51), reset (GPIO 0x27) or pins.
 * Diagnostics (touch.h, DevMode stats "T diag bad", and R84.txt below):
 * diag = failure (bits 0-3, the "ct" index: 0 query timeout, 3 bad sum,
 * 4 bad L, 5 fetch timeout, 6 bad fetch) | ATN level now
 * << 4 | port 2 CTRL as found & 0xff << 8 | ATN-low polls << 16; bad = first
 * 4 bytes of the last failed reply (byte 0 in the top). */
/* Round 4 (device R84 v2: spi_timeouts 0, every failure stage 2 with reply
 * 00000000, ATN low on entry and 22 ATN-low polls; port 2 +0x04/+0x30..+0x44
 * equal RetailOS's set-up 0x08010bec(2, 26, 12000, 1); SPI0's codec answers
 * through the same spi_xfer): the transfers work. RetailOS (0x0817dbe0)
 * checks only the query's LE16 sum and takes L from rx[1..2]; it does not
 * test rx[0], so an all-zero reply (sum 0) is a valid "nothing waits".
 * touch.c demanded rx[0] == 0xEA and counted every idle reply as an
 * error. Now as RetailOS: sum only, L 0 = idle, up to 5 tries 1 ms apart
 * (0x081847e8).
 * R84.txt v4 (report.h mailbox): tagged lines of hex words, see
 * touch_report() and /tmp/touch_fix2_report.txt for the legend; v4 (round
 * 7): "rg" CLKCON +0x14 / I2C1 +10 +0c in place of the GPIO bank 12/14
 * words (FM radio pins, not I2C1), the i2 layout changed (input.h) and
 * "ib" = input.c's boot I2C1 log (input_i2c_recover). */
#define POLL_US 8000u
#define BYTE_US 500u
#define FETCH_MAX 64u
#define TRIES 5u               /* RetailOS: 5, 1 ms apart */
#define BLIND_L 0x36u          /* L of the report in snap6's RX buffer */
#define BLIND_MAX 3u
#define CAPTURE_US 3000000u    /* "touch the screen now" window on entry */
#define CLK(o) (*(volatile uint32_t *)(0x3c500000u + (o)))
#define SPI(o) (*(volatile uint32_t *)(SPI_TOUCH + (o)))
#define GPIC(o) (*(volatile uint32_t *)(0x39700000u + (o)))
#define READY (*(volatile uint32_t *)0x3cf00084u)   /* GPIO 0x26: bank 4 bit 6 */
#define ATN (1u << 30)                              /* its IRQ group 1 bit */
#define RPT_TOUCH_ID 84u
struct touch_state touch;
uint32_t touch_extra[TOUCH_EXTRA];
static uint32_t last, ctrl0;
static unsigned idle, force;
/* R84 "ct" counters: query timeout, all-zero reply, L 0 with a header,
 * bad sum, bad L, fetch timeout, bad fetch, report, ATN-low polls, ATN-low
 * polls answered idle, blind fetches (ATN low but L 0: fetch BLIND_L
 * anyway, at most BLIND_MAX), queries */
enum { C_QTO, C_ZERO, C_IDLE, C_SUM, C_LEN, C_FTO, C_FBAD, C_OK, C_ATN,
       C_ATNIDLE, C_BLIND, C_QUERY, C_N };
static uint32_t cnt[C_N];
/* raw bytes, 4 per word, first byte on top: last query reply with L != 0,
 * last failed reply, last fetch reply (bytes 0-31) */
static uint32_t raw_q[4], raw_bad[4], raw_f[8];
static void pack(uint32_t *d, const uint8_t *s, unsigned n) {
    for (unsigned i = 0; i < n; ++i) d[i >> 2] = d[i >> 2] << 8 | s[i];
}
static int clamp(int v, int hi) { return v < 0 ? 0 : v > hi ? hi : v; }
static unsigned sum(const uint8_t *p, unsigned n) {
    unsigned s = 0;
    while (n--) s += *p++;
    return s;
}
/* RetailOS's polled path, now spi.c's spi_xfer(); 1 = done */
static int xfer(const uint8_t *tx, uint8_t *rx, unsigned n) {
    return spi_xfer(SPI_TOUCH, tx, rx, n, BYTE_US);
}
/* blind: fetch BLIND_L bytes even when the query says nothing waits */
static int read_once(unsigned blind) {
    uint8_t tx[FETCH_MAX], rx[FETCH_MAX];
    for (unsigned i = 0; i < FETCH_MAX; ++i) tx[i] = rx[i] = 0;   /* no memset */
    tx[0] = 0xea;
    tx[1] = 0x01;
    tx[14] = 0xeb;
    unsigned l, c = C_QTO;
    ++cnt[C_QUERY];
    if (!xfer(tx, rx, 16)) goto err;
    c = C_SUM;
    unsigned s = sum(rx, 14);
    if ((s & 0xffff) != (unsigned)(rx[14] | rx[15] << 8)) goto err;
    l = rx[1] | rx[2] << 8;
    if (l) pack(raw_q, rx, 16);
    else if (blind && cnt[C_BLIND] < BLIND_MAX) { ++cnt[C_BLIND]; l = BLIND_L; }
    else { ++cnt[s ? C_IDLE : C_ZERO]; return 0; }   /* nothing waits */
    c = C_LEN;
    if (l + 5 > FETCH_MAX || l < 0x1a) goto err;
    c = C_FTO;
    tx[2] = 0x01;
    tx[14] = 0;
    tx[l + 3] = 0xec;
    if (!xfer(tx, rx, l + 5)) goto err;
    pack(raw_f, rx, 32);
    c = C_FBAD;
    if (rx[0] != 0xea || (sum(rx, 5) & 0xff) ||
        (sum(rx + 5, l - 2) & 0xffff) != (unsigned)(rx[l + 3] | rx[l + 4] << 8) ||
        rx[7] + 8u > l - 2)
        goto err;
    const uint8_t *rec = rx + 5 + rx[7];   /* payload[2] = record offset */
    int px = clamp(((int16_t)(rec[4] | rec[5] << 8) + 75) * 239 / 2845, LCD_PANEL_WIDTH - 1);
    int py = clamp(431 - ((int16_t)(rec[6] | rec[7] << 8) + 75) * 431 / 5250,
                   LCD_PANEL_HEIGHT - 1);
    /* lcd.c: logical (x, y) -> panel (239 - y, x) */
    touch.x = py;
    touch.y = LCD_PANEL_WIDTH - 1 - px;
    touch.state = rec[1];
    ++touch.reports;
    ++cnt[C_OK];
    return 1;
err:
    ++cnt[c];
    touch.diag = (touch.diag & ~0xfu) | c;
    pack(raw_bad, rx, 16);
    touch.bad = raw_bad[0];
    return -1;
}
/* up to TRIES, 1 ms apart, as RetailOS (0x081847e8); then re-arm ATN as
 * 0x0801f008 */
static int read_one(unsigned blind) {
    int r;
    for (unsigned k = 0; (r = read_once(blind)) < 0 && ++k < TRIES; ) delay_us(1000);
    GPIC(0xc4) |= ATN;
    GPIC(0xa4) = ATN;
    return r;
}
#if TOUCH_DIAG
/* a free DevMode slot or the last R84, searched from slot 3 down; none free
 * = no report. One out-of-line FNV (report.h's inline helpers inline it 3x) */
static __attribute__((noinline)) uint32_t fnv(const volatile void *p, uint32_t n) {
    return rpt_sum(p, n);
}
#define HDR_LEN __builtin_offsetof(struct rpt_hdr, hdr_sum)
/* registers in the "rg" lines, in order */
static const uint32_t regs[] = {
    0x3c50004cu, 0x3c50006cu, 0x3cf00084u,          /* CLKCON, GPIO bank 4 */
    SPI_TOUCH + 0x00, SPI_TOUCH + 0x04, SPI_TOUCH + 0x08, SPI_TOUCH + 0x0c,
    SPI_TOUCH + 0x30, SPI_TOUCH + 0x34, SPI_TOUCH + 0x38, SPI_TOUCH + 0x3c,
    SPI_TOUCH + 0x40, SPI_TOUCH + 0x44, SPI_TOUCH + 0x4c,
    0x397000a4u, 0x397000c4u, 0x397000e4u, 0x3cf00080u,
    /* Home search: GPIO bank 10 config (PMU event line), CLKCON +0x14,
     * I2C1 +10 +0c +00 +04 +08 +14 +18 +20 +28, I2C0 +00 +04 +20 (I2C1's
     * clock stays on since round 7; bt.c gates it off after its own) */
    0x3cf00140u, 0x3c500014u, 0x3c900010u, 0x3c90000cu,
    0x3c900000u, 0x3c900004u, 0x3c900008u, 0x3c900014u, 0x3c900018u,
    0x3c900020u, 0x3c900028u, 0x3c600000u, 0x3c600004u, 0x3c600020u,
};
#define NREGS (sizeof regs / sizeof regs[0])
/* tag, then the words 4 per line (fmt_line caps at 53) */
static void group(const char *tag, const volatile uint32_t *p, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
        if (!(i & 3)) fmt_str(tag);
        fmt_hex(p[i], 8);
        fmt_str(" ");
        if ((i & 3) == 3 || i + 1 == n) fmt_line(0, 0, 0);
    }
}
/* CAPTURE_US of step() with the title and a countdown on screen */
void touch_window(const char *title, int (*step)(void)) {
    uint32_t t0 = USEC, left;
    while ((left = USEC - t0) < CAPTURE_US) {
        step();
        if (lcd_ready()) {
            lcd_clear(0xff1a1a1a);
            lcd_center(90, title, WHITE);
            fmt_str("CAPTURE  "); fmt_dec(((CAPTURE_US - left) >> 20) + 1);
            fmt_line(150, 120, WHITE);
            lcd_present();
        }
        lcd_service();
    }
}
void touch_report(void) {
    volatile struct rpt_hdr *h = RPT_HDR;
    unsigned s = 4;
    if (h->magic[0] != RPT_MAGIC0 || h->magic[1] != RPT_MAGIC1 ||
        h->slot_size != RPT_SLOT_SIZE) {
        for (uint32_t i = 0; i < sizeof *h / 4; ++i) ((volatile uint32_t *)h)[i] = 0;
        h->slot_size = RPT_SLOT_SIZE;
    }
    while (--s && h->id[s] && h->id[s] != RPT_TOUCH_ID) ;
    if (h->id[s] && h->id[s] != RPT_TOUCH_ID) return;
    char *p0 = RPT_SLOT(s);
    uint32_t v[NREGS > 6 ? NREGS : 6];
    fmt_out = p0;
    fmt_end = p0 + 4096;
    fmt_str("== TOUCH R84 v4 =="); fmt_line(0, 0, 0);
    v[0] = touch.diag; v[1] = touch.bad; v[2] = touch.reports; v[3] = touch.errors;
    v[4] = spi_timeouts; v[5] = ctrl0;
    group("st ", v, 6);
    group("ct ", cnt, C_N);
    group("rq ", raw_q, 4);
    group("rb ", raw_bad, 4);
    group("rf ", raw_f, 8);
    for (unsigned i = 0; i < NREGS; ++i) v[i] = *(volatile uint32_t *)regs[i];
    group("rg ", v, NREGS);
    group("i2 ", (const volatile uint32_t *)&input_debug, sizeof input_debug / 4);
    group("ib ", input_i2c_log, INPUT_I2C_LOG);
    group("hm ", touch_extra, TOUCH_EXTRA);
    fmt_str("== END R84 =="); fmt_line(0, 0, 0);
    uint32_t len = (uint32_t)(fmt_out - p0);
    fmt_out = 0;
    h->id[s] = RPT_TOUCH_ID;
    h->len[s] = len;
    h->sum[s] = fnv(p0, len);
    h->magic[0] = RPT_MAGIC0;
    h->magic[1] = RPT_MAGIC1;
    h->hdr_sum = fnv(h, HDR_LEN);
}
#endif
void touch_start(void) {
    unsigned n = 0;
    int r;
    if (!ctrl0) ctrl0 = SPI(0x04);      /* as RetailOS left it: the first call */
    touch.diag = (ctrl0 & 0xff) << 8;
    CLK(0x4c) &= ~0x8000u;              /* gate 0x1e on */
    CLK(0x6c) &= ~0x8000u;
    __asm__ volatile("dsb sy" ::: "memory");
    do r = read_one(0); while (r > 0 && ++n < 40);
    touch.errors = r < 0;   /* a failed first query shows as ERR */
    touch.reports = 0;
    touch.state = 0;
    last = USEC;
}
void touch_init(void) {
    touch_start();
#if TOUCH_DIAG
    /* one device run must show what a finger does: queries every POLL_US
     * whatever ATN says */
    force = 1;
    touch_window("TOUCH THE SCREEN NOW", touch_poll);
    force = 0;
#endif
    touch_report();
}
int touch_poll(void) {
    if (USEC - last < POLL_US) return 0;
    last = USEC;
    uint32_t hi = READY & 0x40;
    touch.diag = (touch.diag & ~0x10u) | hi >> 2;
    if (!hi) { touch.diag += 1u << 16; ++cnt[C_ATN]; }  /* ATN low: a report waits */
    else if (!force && (++idle & 15)) return 0;
    int r = read_one(!hi);
    if (!hi && !r) ++cnt[C_ATNIDLE];
    /* R84 again at 1, 2, 4, 8.. reports or errors */
    unsigned n = r > 0 ? touch.reports : r < 0 ? ++touch.errors : 3;
    if (TOUCH_DIAG && !force && !(n & (n - 1))) touch_report();
    return r > 0;
}
