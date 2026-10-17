#pragma GCC optimize("Os", "no-tree-loop-distribute-patterns")   /* polled; kernel.bin must fit 64 KB; no memset */
#include "buttons.h"
#include "input.h"
#include "timer.h"
#include "touch.h"
/* Sources (NanoEmu nano7_pmu.c:5-43, nano7_gpio.c:16, codex-touch.txt):
 *   Home   PMU (I2C1 0x73) reg 0x07 bit4   active-low in the model
 *   Power  PMU reg 0x07 bit5                (sleep/wake)
 *   Play   PMU reg 0x08 bit1
 *   Vol+   GPIO 0x28: 0x3cf000a4 bit0, active-low
 *   Vol-   GPIO 0x29: 0x3cf000a4 bit1, active-low
 * Home's level comes from input.c (already polled every pass). PMU
 * polarity is not confirmed on the device (Home was not, session 74), so
 * Power and Play are "away from the level first read", as Home is.
 * Temperature: regs 0x3c/0x3d, r = reg3d << 2 | reg3c[3:2],
 * r = 500 + 1.25 * C (model formula; unconfirmed on the device, where
 * CLAUDE.md names reg 0x08). No Hold switch on this model. */
#define VOLUME (*(volatile uint32_t *)0x3cf000a4u)
/* Home search. Device R84 v2 (round 3): 0 good PMU reads of 533, the PMU
 * event line (bank 10 bit 6) low throughout. (GPIO banks 12-14, read 0,
 * are not I2C1: pins 97/98/119 are the BT/FM radio's I2S, round 7.)
 * RetailOS talks to the PMU on bus 1 = 0x3c900000 (ops 0x0890c0a8 ->
 * 0x0814c134 / 0x08146924, r0 = 1), as input.c and bt.c do, so the bus,
 * not the address, is the suspect. One device run must tell which:
 * buttons_init() runs a CAPTURE_US "HOLD HOME NOW" window that
 *  - sends a bare address byte (START, address, STOP) to the PMU (E6), the
 *    accelerometer (0x18 -> 30), Tristar (0x1a -> 34) on bus 1 and to
 *    slave 0x08 (10) on bus 0 (0x3c600000, gate 0x29), the 8-bit form;
 *    an ACK from 30/34 but not E6 = PMU only; none on bus 1 but 10 on
 *    bus 0 = bus 1 stuck; none at all = the controllers;
 *  - reads PMU regs 7/8 every PMU_WIN_US through input.c (bit AND/OR,
 *    good/failed);
 *  - diffs every GPIO bank against its level at entry.
 * touch_extra[] (R84 "hm" lines, touch.c also dumps input_debug and the
 * I2C/GPIO registers):
 *   0-3   bits toggled in the window (bank b = byte b%4 of word b/4)
 *   4-7   levels at entry, same layout
 *   8     reg7 AND | reg7 OR << 8 | reg8 AND << 16 | reg8 OR << 24
 *   9     good PMU reads << 16 | failed (window and after)
 *   10    buttons now | seen << 8 | input.c Home idle << 16 | reg7 << 24
 *   11    first good reg7 | PMU polls in the window << 8
 *   12-16 probes E6, 30, 34 (bus 1), 10, E6 (bus 0): IICSTAT << 24 |
 *         +0x20 events << 8 | IICCON low byte, 5 ms after the address or
 *         at byte done (event bit 8 = done; IICSTAT bit 0 = no ACK)
 *   17    what the Drivers-entry controller unstick did (input.c
 *         input_i2c_recover, after the probes, before bt.c's PMU writes;
 *         R84 "ib" has the boot run) */
#define GPIO_LEVEL(b) (*(volatile uint32_t *)(0x3cf00004u + (b) * 0x20u))
#define BANKS 15u
#define PMU_US 50000u
#define PMU_WIN_US 20000u
#define TEMP_US 1000000u
#define I2C1 0x3c900000u
#define I2C0 0x3c600000u
#define IIC(b, o) (*(volatile uint32_t *)((b) + (o)))
#define CLKCON(o) (*(volatile uint32_t *)(0x3c500000u + (o)))
struct buttons_state buttons;
static unsigned r7_idle, r8_idle, idle_known;
static uint32_t last, last_temp;
#if TOUCH_DIAG
static uint8_t *const tog = (uint8_t *)&touch_extra[0];
static uint8_t *const base = (uint8_t *)&touch_extra[4];
static uint8_t *const pmu_bits = (uint8_t *)&touch_extra[8];
static unsigned fresh;   /* a new PMU bit: report */
static uint32_t win;
/* START + one address byte on controller b, then STOP (input.c's
 * sequence, bounded) */
static uint32_t probe(uint32_t b, unsigned addr) {
    IIC(b, 0x20) = 0x100;
    IIC(b, 0x04) = 0xd0;
    IIC(b, 0x00) = (IIC(b, 0x00) & ~0x10u) | 0x180;   /* ACK, byte-done event, not pending */
    IIC(b, 0x0c) = addr;
    IIC(b, 0x04) = 0xf0;
    for (uint32_t t0 = USEC; !(IIC(b, 0x20) & 0x100) && USEC - t0 < 5000u; ) ;
    uint32_t r = IIC(b, 0x04) << 24 | (IIC(b, 0x20) & 0xffff) << 8 | (IIC(b, 0x00) & 0xff);
    IIC(b, 0x04) = 0xd0;
    IIC(b, 0x20) = 0x2100;
    IIC(b, 0x00) &= ~0x10u;
    delay_us(1000);
    return r;
}
#endif
static void read_pmu(void) {
    int r7 = input_pmu_read(7), r8 = input_pmu_read(8);
    if (r7 < 0 || r8 < 0) { ++buttons.pmu_err; ++touch_extra[9]; return; }   /* R84 w33 */
#if TOUCH_DIAG
    touch_extra[9] += 1u << 16;
    unsigned old = touch_extra[8];
    pmu_bits[0] &= (uint8_t)r7; pmu_bits[1] |= (uint8_t)r7;
    pmu_bits[2] &= (uint8_t)r8; pmu_bits[3] |= (uint8_t)r8;
    if (touch_extra[9] >> 16 == 1) touch_extra[11] |= (uint8_t)r7;
    fresh |= old != touch_extra[8];
#endif
    if (!idle_known) { r7_idle = (unsigned)r7; r8_idle = (unsigned)r8; idle_known = 1; }
    buttons.r7 = (unsigned)r7;
    buttons.r8 = (unsigned)r8;
}
#if TOUCH_DIAG
/* the "HOLD HOME NOW" window: every GPIO bank against its entry level, the
 * PMU every PMU_WIN_US */
static int window(void) {
    for (unsigned k = 0; k < BANKS; ++k) tog[k] |= (uint8_t)(GPIO_LEVEL(k) ^ base[k]);
    if (USEC - win >= PMU_WIN_US) {
        win = USEC;
        touch_extra[11] += 0x100;
        read_pmu();
    }
    return 0;
}
#endif
static void read_temp(void) {
    int lo = input_pmu_read(0x3c), hi = input_pmu_read(0x3d);
    if (lo < 0 || hi < 0) return;
    int r = hi << 2 | ((lo >> 2) & 3);
    buttons.temp_c = (r - 500) * 4 / 5;
}
void buttons_init(void) {
    buttons.seen = 0;
    idle_known = 0;
#if TOUCH_DIAG
    for (unsigned i = 0; i < TOUCH_EXTRA; ++i) touch_extra[i] = 0;
    touch_extra[8] = 0x00ff00ffu;       /* AND halves start all-ones */
    for (unsigned b = 0; b < BANKS; ++b) base[b] = (uint8_t)GPIO_LEVEL(b);
    static const uint16_t probes[] = { 0xe6, 0x30, 0x34, 0x8010, 0x80e6 };
    CLKCON(0x4c) &= ~0x50u;             /* gates 0x27 / 0x29: both buses */
    CLKCON(0x6c) &= ~0x1800u;
    for (unsigned i = 0; i < 5; ++i)
        touch_extra[12 + i] = probe(probes[i] & 0x8000 ? I2C0 : I2C1, probes[i] & 0xff);
    CLKCON(0x4c) |= 0x10u;              /* bus 0 back off (input.c owns bus 1) */
    CLKCON(0x6c) |= 0x800u;
    touch_extra[17] = input_i2c_recover();
#else
    input_i2c_recover();
#endif
#if TOUCH_DIAG
    win = USEC - PMU_WIN_US;
    touch_window("HOLD HOME NOW", window);
    fresh = 0;
#else
    read_pmu();
#endif
    read_temp();
    last = last_temp = USEC;
}
void buttons_poll(void) {
    uint32_t now = USEC;
    if (now - last >= PMU_US) { last = now; read_pmu(); }
    if (now - last_temp >= TEMP_US) { last_temp = now; read_temp(); }
    unsigned b = 0, v = VOLUME;
    if (input_home_level()) b |= BTN_HOME;
    if (idle_known && ((buttons.r7 ^ r7_idle) & 0x20)) b |= BTN_POWER;
    if (idle_known && ((buttons.r8 ^ r8_idle) & 0x02)) b |= BTN_PLAY;
    if (!(v & 1)) b |= BTN_VOLUP;
    if (!(v & 2)) b |= BTN_VOLDOWN;
    buttons.now = b;
    buttons.seen |= b;
#if TOUCH_DIAG
    if (fresh) {
        touch_extra[10] = b | buttons.seen << 8 | input_debug.idle << 16 |
                          input_debug.r7 << 24;
        fresh = 0;
        touch_report();
    }
#endif
}
