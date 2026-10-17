#include "buttons.h"
#include "input.h"
#include "timer.h"
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
#define PMU_US 50000u
#define TEMP_US 1000000u
struct buttons_state buttons;
static unsigned r7_idle, r8_idle, idle_known;
static uint32_t last, last_temp;
static void read_pmu(void) {
    int r7 = input_pmu_read(7), r8 = input_pmu_read(8);
    if (r7 < 0 || r8 < 0) { ++buttons.pmu_err; return; }
    if (!idle_known) { r7_idle = (unsigned)r7; r8_idle = (unsigned)r8; idle_known = 1; }
    buttons.r7 = (unsigned)r7;
    buttons.r8 = (unsigned)r8;
}
static void read_temp(void) {
    int lo = input_pmu_read(0x3c), hi = input_pmu_read(0x3d);
    if (lo < 0 || hi < 0) return;
    int r = hi << 2 | ((lo >> 2) & 3);
    buttons.temp_c = (r - 500) * 4 / 5;
}
void buttons_init(void) {
    buttons.seen = 0;
    idle_known = 0;
    read_pmu();
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
}
