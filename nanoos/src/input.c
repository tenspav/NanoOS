#include <stdint.h>
#include "input.h"
#define I2C(o) (*(volatile uint32_t *)(0x3c900000u + (o)))
#define GPIO(a) (*(volatile uint32_t *)(a))
static unsigned previous;
/* Bounded polling: a missing PMU must not freeze volume navigation. */
static int done(void) {
    for (unsigned i = 0; i < 100000; ++i)
        if (I2C(0) & 0x10) return !(I2C(4) & 1);
    return 0;
}
static void stop(unsigned mode) {
    I2C(4) = mode;
    I2C(0) = 0x81;
    I2C(0x20) = 0x2100;
}
static int begin(unsigned mode, unsigned addr) {
    I2C(0x20) = 0x2100;
    I2C(0) = 0x81;
    I2C(4) = mode;
    I2C(0xc) = addr;
    I2C(4) = mode | 0x20;
    return done();
}
static int pmu_read(unsigned reg) {
    int result = -1;
    if (!begin(0xd0, 0xe6)) goto out_tx;
    I2C(0xc) = reg;
    I2C(0) = 0x81;
    if (!done()) goto out_tx;
    stop(0xd0);
    if (!begin(0x90, 0xe7)) goto out_rx;
    I2C(0) = 1; /* resume RX, NACK final byte */
    if (done()) result = I2C(0xc) & 0xff;
out_rx:
    stop(0x90);
    return result;
out_tx:
    stop(0xd0);
    return -1;
}
static int pmu_ack(unsigned reg, unsigned bits) {
    int ok = 0;
    if (!begin(0xd0, 0xe6)) goto out;
    I2C(0xc) = reg; I2C(0) = 0x81;
    if (!done()) goto out;
    I2C(0xc) = bits; I2C(0) = 0x81;
    ok = done();
out:
    stop(0xd0);
    return ok;
}
static unsigned sample(void) {
    unsigned buttons = 0;
    unsigned volume = GPIO(0x3cf000a4u);
    if (!(volume & 2)) buttons |= BUTTON_LEFT;
    if (!(volume & 1)) buttons |= BUTTON_RIGHT;
    /* GPIO 0x56 is shared/latching, not a Home level. Read PMU level
     * (reg7 bit4 active-low) and acknowledge events to rearm the line. */
    int level = pmu_read(7);
    if (level >= 0 && !(level & 0x10)) buttons |= BUTTON_HOME;
    if (!(GPIO(0x3cf00144u) & (1u << 6))) {
        for (unsigned r = 1; r <= 4; ++r) {
            int pending = pmu_read(r);
            if (pending > 0) pmu_ack(r, (unsigned)pending);
        }
    }
    return buttons;
}
void input_init(void) { previous = sample(); }
unsigned input_poll(void) {
    unsigned now = sample();
    unsigned pressed = now & ~previous;
    previous = now;
    return pressed;
}
