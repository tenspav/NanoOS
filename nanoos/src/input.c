#include <stdint.h>
#include "input.h"
#include "timer.h"
#include "trace.h"
#define I2C(o) (*(volatile uint32_t *)(0x3c900000u + (o)))
#define CLKCON(o) (*(volatile uint32_t *)(0x3c500000u + (o)))
#define GPIO(a) (*(volatile uint32_t *)(a))
static unsigned previous, chord, home_level, home_candidate, home_votes;
static unsigned latched;   /* key events input_service() saw, for input_poll() */
static unsigned home_idle = 0x10;
static uint32_t last_poll, last_events;
static int last_failed;
#define HOME_BIT 0x10u  /* PMU reg7 */
struct input_debug input_debug;
/* round 15: PMU I2C register reads put on the bus (each one gate-cycles
 * CLKCON +0x4c/+0x6c); ViceNano counts the ones that land mid-scene */
unsigned input_i2c_n;
/* I2C bus 1 to the PMU (slave 0x73), polled the way RetailOS's IRAM driver
 * sequences it (0x22000652 start, ISR 0x22001214, init 0x080017b4):
 *   +0x00 IICCON  bit7 ACK, bit4 pending (0 = resume), bits 8/13 enable the
 *                 byte-done / stop-done events in +0x20
 *   +0x04 IICSTAT 0xd0/0xf0 master TX stop/start, 0x90/0xb0 master RX,
 *                 bit5 busy, bit0 last ACK (0 = ACK)
 *   +0x0c IICDS   +0x20 events: bit8 byte done, bit13 stop done (W1C)
 * RetailOS gates the I2C clock off after every transfer, so after the
 * handoff it is off: ungate it once (gate id 0x27) and keep it on.
 *
 * Device DevMode readings (kernel f5b9a0db, boot-progress sessions 78/79):
 * only ~1 in 4 register reads succeeded, failing at an address byte that
 * was either NACKed or completed after the old 2 ms timeout; IICSTAT bit5
 * was always set, +0x20 never showed bit13, and there were ~6 timeouts
 * per read. So: wait long enough for a late byte, never wait long for
 * bus-free / STOP-done (counted per site, to confirm on the device), retry
 * the whole register read, and poll the PMU at a bounded rate so a sick
 * bus cannot eat the frame rate. NanoEmu --real-ipod models all of this. */
#define I2C_BYTE_DONE 0x100u
#define I2C_STOP_DONE 0x2000u
#define I2C_BYTE_US 5000u   /* late address bytes completed after 2+ ms */
/* bus-free / STOP-done: never seen within 100 us on the device. RetailOS
 * waits up to 100 ms for STOP-done (wait 0x22000588, round 11 RE); a START
 * issued into an unfinished STOP is the likely cause of the address NACKs,
 * so wait 1 ms (bounded: the PMU is polled at most every 20 ms). */
#define I2C_FREE_US 1000u
/* RetailOS spaces transfers by 5 ticks of timer 0x3c700084, most likely
 * 1 us each (round 11 RE); 50 us keeps a margin. */
#define I2C_GAP_US 50u
#define I2C_TRIES 2u        /* register-read attempts per poll */
/* Each register read costs ~1.6 ms of blocked CPU on the device's bus (two
 * 500 us gaps, 90 us bytes, retries): at a steady 50 Hz that was a third
 * of the CPU. Poll fast only while a Home change waits for its second
 * read or the last read failed. The PMU event line (GPIO 0x56) cannot
 * gate it: it is shared with the accelerometer, whose FIFO watermark
 * holds it low (NanoEmu shows it low all the time). */
#define PMU_POLL_US 20000u  /* 50 Hz */
#define PMU_IDLE_US 60000u  /* ~17 Hz */
#define EVENT_POLL_US 1000000u
/* RetailOS's pre/post hooks (0x220004a8 / 0x220004e4, round 11 RE) run
 * around EVERY transfer: ungate gate id 0x27 and OR the saved IICCON bits
 * 0x2100 back; afterwards save IICCON & 0x2100, clear them and gate the
 * clock off. The gate cycle resets the controller state the device showed
 * stuck (IICSTAT bit5 always set, STOP-done never seen). */
static uint32_t con_saved = 0x100u;
static void i2c_ungate(void) {
    CLKCON(0x4c) &= ~0x40u;
    CLKCON(0x6c) &= ~0x1000u;
    __asm__ volatile("dsb sy" ::: "memory");
}
static void i2c_pre(void) {
    i2c_ungate();
    I2C(0x00) |= con_saved;
}
static void i2c_post(void) {
    con_saved = I2C(0x00) & 0x2100u;
    I2C(0x00) &= ~0x2100u;
    CLKCON(0x4c) |= 0x40u;
    CLKCON(0x6c) |= 0x1000u;
    __asm__ volatile("dsb sy" ::: "memory");
}
static void i2c_init(void) {
    i2c_ungate();
    I2C(0x08) = 0x40; I2C(0x14) = 1; I2C(0x18) = 0;
    I2C(0x04) = 0x80; I2C(0x00) = 0; I2C(0x04) = 0; I2C(0x0c) = 0x40;
    I2C(0x00) = 0x181; I2C(0x04) = 0x10; I2C(0x28) = 0;
    I2C(0x20) = I2C_BYTE_DONE | I2C_STOP_DONE;
    con_saved = I2C(0x00) & 0x2100u;
}
/* Wait for (*r & mask) != 0 for at most us; a frozen USEC ends it too. */
static int wait_set(volatile uint32_t *r, uint32_t mask, uint32_t us,
                    unsigned *timeouts) {
    uint32_t t0 = USEC;
    for (uint32_t polls = 0; polls < us * 64u; ++polls) {
        if (*r & mask) return 1;
        if (USEC - t0 >= us) break;
    }
    ++*timeouts;
    return 0;
}
/* Byte done: RetailOS's ISR reads +0x20 bit8; IICCON bit4 is the classic
 * Samsung pending flag. On the device +0x20 alone timed out on nearly every
 * wait, so accept either. */
static int byte_done(void) {
    uint32_t t0 = USEC;
    for (uint32_t polls = 0; polls < I2C_BYTE_US * 64u; ++polls) {
        if ((I2C(0x20) & I2C_BYTE_DONE) || (I2C(0x00) & 0x10u)) return 1;
        if (USEC - t0 >= I2C_BYTE_US) break;
    }
    ++input_debug.to_byte;
    return 0;
}
/* where the last failed transfer stopped, and the controller state then */
static void failed(unsigned stage) {
    input_debug.stage = stage;
    input_debug.con = I2C(0x00);
    input_debug.stat = I2C(0x04);
    input_debug.ev = I2C(0x20);
}
static void resume(void) {
    I2C(0x00) = (I2C(0x00) & ~0x10u) | 0x80u;
    I2C(0x20) = I2C_BYTE_DONE;
}
static int acked(void) { return !(I2C(0x04) & 1); }
/* mode 0xd0 (TX) or 0x90 (RX) */
static void stop(unsigned mode) {
    I2C(0x20) = I2C_STOP_DONE;
    I2C(0x00) = (I2C(0x00) & ~0x10u) | 0x2000u;
    I2C(0x04) = mode;
    resume();
    wait_set(&I2C(0x20), I2C_STOP_DONE, I2C_FREE_US, &input_debug.to_stop);
    I2C(0x00) &= ~0x2010u;
    resume();
    I2C(0x20) = I2C_BYTE_DONE | I2C_STOP_DONE;
}
static int start(unsigned mode, unsigned addr) {
    delay_us(I2C_GAP_US);
    /* bus free = IICSTAT bit5 clear; it never cleared on the device */
    uint32_t t0 = USEC;
    for (uint32_t polls = 0; I2C(0x04) & 0x20u; ++polls)
        if (USEC - t0 >= I2C_FREE_US || polls >= I2C_FREE_US * 64u) {
            ++input_debug.to_free;
            break;
        }
    I2C(0x20) = I2C_BYTE_DONE;          /* no stale byte-done */
    I2C(0x04) = mode;
    I2C(0x00) |= 0x80u;
    I2C(0x0c) = addr;
    I2C(0x04) = mode | 0x20;
    return byte_done() && acked();
}
/* START, address, then each byte; STOP in every case. 0, or the failed
 * stage (1 address, 2 data). */
static unsigned pmu_write(const uint8_t *bytes, unsigned n) {
    i2c_pre();
    unsigned stage = start(0xd0, 0xe6) ? 0 : 1;
    for (unsigned i = 0; !stage && i < n; ++i) {
        I2C(0x0c) = bytes[i];
        resume();
        if (!byte_done() || !acked()) stage = 2;
    }
    if (stage) failed(stage);
    stop(0xd0);
    i2c_post();
    return stage;
}
/* register pointer write + STOP, then a 1-byte read; -1 if any stage
 * failed (stages 3 read address, 4 read data) */
static int pmu_read_once(unsigned reg) {
    uint8_t r = (uint8_t)reg;
    int result = -1;
    ++input_i2c_n;
    if (pmu_write(&r, 1)) return -1;
    i2c_pre();
    if (start(0x90, 0xe7)) {
        I2C(0x00) &= ~0x90u;            /* NACK the only byte, resume */
        I2C(0x20) = I2C_BYTE_DONE;
        if (byte_done()) result = I2C(0x0c) & 0xff;
        else failed(4);
    } else failed(3);
    stop(0x90);
    i2c_post();
    return result;
}
/* A failure at any stage ends the transfer with a STOP and the whole read
 * starts over: on the device about 3 in 4 address bytes failed. */
static int pmu_read(unsigned reg, unsigned tries) {
    for (unsigned attempt = 0; attempt < tries; ++attempt) {
        if (attempt) ++input_debug.retry;
        int v = pmu_read_once(reg);
        if (v >= 0) return v;
    }
    return -1;
}
static void pmu_ack(unsigned reg, unsigned bits) {
    uint8_t b[2] = {(uint8_t)reg, (uint8_t)bits};
    pmu_write(b, 2);
}
/* Home level from PMU reg7 bit4, polled every PMU_POLL_US. It changes only
 * after two good reads in a row agree, so one glitched read can neither
 * press nor release Home (a phantom Home once launched StockOS right after
 * the confirm); failed reads keep the level. */
static void poll_home(void) {
    uint32_t now = USEC;
    int line = !(GPIO(0x3cf00144u) & (1u << 6));
    int fast = home_votes || last_failed;
    if (now - last_poll < (fast ? PMU_POLL_US : PMU_IDLE_US)) return;
    last_poll = now;
    trace(6);
    int level = pmu_read(7, I2C_TRIES);
    trace(7);
    last_failed = level < 0;
    if (level < 0) { ++input_debug.err; return; }
    ++input_debug.ok;
    input_debug.r7 = (unsigned)level;
    /* pressed = bit4 away from its idle level (polarity not assumed) */
    unsigned pressed = ((unsigned)level & HOME_BIT) != home_idle;
    if (pressed == home_level) { home_votes = 0; }
    else if (home_votes && pressed == home_candidate) {
        home_level = pressed;
        home_votes = 0;
    } else { home_candidate = pressed; home_votes = 1; }
    /* GPIO 0x56 is shared/latching, not a Home level: acknowledge the PMU
     * event registers (reg3 bit0 Home) now and then to rearm the line;
     * one attempt each, the level above does not depend on it */
    if (line && now - last_events >= EVENT_POLL_US) {
        last_events = now;
        for (unsigned r = 1; r <= 4; ++r) {
            int pending = pmu_read(r, 1);
            if (pending > 0) pmu_ack(r, (unsigned)pending);
        }
    }
}
static unsigned sample(void) {
    unsigned buttons = 0;
    unsigned volume = GPIO(0x3cf000a4u);
    if (!(volume & 2)) buttons |= BUTTON_LEFT;
    if (!(volume & 1)) buttons |= BUTTON_RIGHT;
    poll_home();
    if (home_level) buttons |= BUTTON_HOME;
    return buttons;
}
int input_pmu_read(unsigned reg) { return pmu_read(reg, I2C_TRIES); }
unsigned input_home_level(void) { return home_level; }
void input_init(void) {
    i2c_init();
    /* Idle level of the Home bit, read before any press (the app was
     * opened by touch). Emulator and firmware notes say active-low. */
    int level = pmu_read(7, 8);
    if (level >= 0) home_idle = (unsigned)level & HOME_BIT;
    input_debug.idle = home_idle;
    last_poll = last_events = USEC - EVENT_POLL_US;
    home_level = home_votes = 0;
    previous = sample();
    chord = latched = 0;
}
/* Volume acts on release: alone it is Left/Right. Both held together are
 * a Home press, the universal exit that needs no PMU/I2C (Home itself has
 * never registered on the user's device). Round 11: the chord fires the
 * moment both are seen held in one sample, not on the release, so a long
 * frame cannot lose it; nothing more fires until both are released. Home
 * itself acts on press. */
static unsigned key_edges(void) {
    unsigned now = sample();
    unsigned vol = now & (BUTTON_LEFT | BUTTON_RIGHT);
    unsigned pressed = (now & ~previous) & BUTTON_HOME;
    if (vol == (BUTTON_LEFT | BUTTON_RIGHT) && !chord) {
        chord = 1;
        pressed |= BUTTON_HOME;
    }
    if (!vol && (previous & (BUTTON_LEFT | BUTTON_RIGHT))) {
        if (!chord) pressed |= previous & (BUTTON_LEFT | BUTTON_RIGHT);
        chord = 0;
    }
    previous = now;
    return pressed;
}
/* A long frame (ViceNano world: 100+ ms per pass) sampled Home once per
 * pass; with about 1 in 4 PMU reads good on the device, a press was never
 * confirmed. Long work calls this to sample in between (the PMU reads stay
 * rate-limited); the events wait for the next input_poll(). */
void input_service(void) { latched |= key_edges(); }
unsigned input_poll(void) {
    unsigned keys = latched | key_edges();
    latched = 0;
    return keys;
}
