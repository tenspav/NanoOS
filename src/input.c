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
static unsigned fail_run;  /* failed Home polls in a row */
static uint32_t last_rec;
#define HOME_BIT 0x10u  /* PMU reg7 */
/* the I2C path is polled (<= 50 Hz), not timing-critical: Thumb -Os
 * (input.c is otherwise ARM -O2, kernel.bin must fit 64 KB) */
#define COLD __attribute__((target("thumb"), optimize("Os"), noinline))
#define SMALL __attribute__((target("thumb"), optimize("Os")))
struct input_debug input_debug;
/* round 15: PMU I2C register reads put on the bus (each one gate-cycled
 * CLKCON +0x4c/+0x6c until round 7); ViceNano counts the ones that land
 * mid-scene */
unsigned input_i2c_n;
/* I2C bus 1 to the PMU (slave 0x73), polled the way RetailOS's IRAM driver
 * sequences it (0x22000652 start, ISR 0x22001214, init 0x080017b4):
 *   +0x00 IICCON  bit7 ACK, bit4 pending (0 = resume), bits 8/13 enable the
 *                 byte-done / stop-done events in +0x20
 *   +0x04 IICSTAT 0xd0/0xf0 master TX stop/start, 0x90/0xb0 master RX,
 *                 bit5 busy, bit0 last ACK (0 = ACK)
 *   +0x0c IICDS   +0x20 events: bit8 byte done, bit13 stop done (W1C)
 * I2C1 has dedicated pads: no GPIO mux (pins 97/98/119, muxed by an
 * earlier round, are the BT/FM radio's I2S pins: 0x08158e1c is the FM
 * power routine, "BT_KEY_FM_RADIO_POWER" in its pool, AGY 2026-10-17).
 *
 * Round 7 (2026-10-17, AGY + own check of IRAM 0x22001380..0x2200142c):
 * the device went from ~1 in 4 good reads (kernel f5b9a0db: clock ungated
 * once, never gated again) to 0 of 533 after round 11 added RetailOS's
 * per-transfer gate cycle, while STOP-done never came within its 1 ms:
 * the clock was cut with the STOP in flight, and IICCON bit4 (pending,
 * which holds SCL) was never cleared before a START. Now: the clock stays
 * on; START clears bit4; resume = IICCON bit4 clear, +0x20 W1C, IICCON
 * again (the ISR's order); STOP clears 0x2010 as the ISR and is re-issued
 * every ms up to I2C_STOP_US; a lost STOP re-initialises the controller;
 * prescaler 1 as RetailOS's init 0x080017b4 (IICCON 0x181; the 6 came
 * from the FM routine); at boot (input_i2c_recover) a controller left
 * busy or pending by the handoff gets its read byte NACKed and a STOP.
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
#define I2C_RDY_US 200u    /* +0x10 = 0 before a START (measured) */
#define I2C_STOP_US 5000u  /* RetailOS: 100 ms, STOP re-issued */
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
/* RetailOS's pre/post hooks (0x220004a8 / 0x220004e4) gate gate id 0x27
 * on and off around every transfer, after a STOP that completed. Here the
 * STOP may not complete, so the clock is ungated before each transfer
 * and never gated off (bt.c fix3 likewise never gates it off). */
static SMALL void i2c_ungate(void) {
    CLKCON(0x4c) &= ~0x40u;
    CLKCON(0x6c) &= ~0x1000u;
    __asm__ volatile("dsb sy" ::: "memory");
}
static COLD void i2c_init(void) {
    i2c_ungate();
    I2C(0x08) = 0x40; I2C(0x14) = 1; I2C(0x18) = 0;
    I2C(0x04) = 0x80; I2C(0x00) = 0; I2C(0x04) = 0; I2C(0x0c) = 0x40;
    I2C(0x00) = 0x181; I2C(0x04) = 0x10; I2C(0x28) = 0;
    I2C(0x20) = I2C_BYTE_DONE | I2C_STOP_DONE;
}
/* Byte done: +0x20 bit8 only, as RetailOS's ISR. Accepting IICCON bit4 (the
 * classic Samsung pending flag) as well ended waits early on a stale bit:
 * the device then "read" the address byte 0xE7 as data (BT round 2), so
 * Home's reg7 level was garbage. */
static COLD int byte_done(void) {   /* 3 callers */
    uint32_t t0 = USEC;
    for (uint32_t polls = 0; polls < I2C_BYTE_US * 64u; ++polls) {
        if (I2C(0x20) & I2C_BYTE_DONE) return 1;
        if (USEC - t0 >= I2C_BYTE_US) break;
    }
    ++input_debug.to_byte;
    return 0;
}
/* where the last failed transfer stopped, and the controller state then */
static SMALL void failed(unsigned stage) {
    input_debug.stage = stage;
    input_debug.con = I2C(0x00);
    input_debug.stat = I2C(0x04);
    input_debug.ev = I2C(0x20);
}
/* the ISR's order (0x220013e2..0x2200141a): IICCON with bit4 clear (and
 * ack bits), +0x20 W1C, IICCON written again; ack 0x80 = ACK, 0 = NACK
 * the byte being read */
static COLD void resume_ack(uint32_t ack) {
    uint32_t c = (I2C(0x00) & ~0x90u) | ack;
    I2C(0x00) = c;
    I2C(0x20) = I2C_BYTE_DONE;
    I2C(0x00) = c;
}
static SMALL void resume(void) { resume_ack(0x80u); }
static SMALL int acked(void) { return !(I2C(0x04) & 1); }
/* mode 0xd0 (TX) or 0x90 (RX). STOP re-issued every ms until done:
 * +0x20 bit13 (the ISR's test, 0x220013a6) or IICSTAT bit5 clear (the
 * polled wait 0x22000588 accepts that too); then IICCON &= ~0x2010 as the
 * ISR. Never done: the controller is initialised again, and after 4 such
 * STOPs in a row only one 1 ms try is made (older device runs never saw
 * STOP-done: 5 ms per STOP would eat the frame rate). */
static unsigned stops_lost;
static SMALL int stop_done(void) { return (I2C(0x20) & I2C_STOP_DONE) || !(I2C(0x04) & 0x20u); }
static COLD void stop(unsigned mode) {
    unsigned n = stops_lost >= 4 ? 1 : I2C_STOP_US / 1000u, done = 0;
    I2C(0x20) = I2C_STOP_DONE;
    I2C(0x00) |= 0x2000u;
    while (n-- && !done) {
        I2C(0x04) = mode;
        resume();
        uint32_t t0 = USEC, spins = 0;
        while (!(done = stop_done()) && USEC - t0 < 1000u && ++spins < 1000u * 64u) ;
    }
    if (!done) {
        ++input_debug.to_stop;
        ++stops_lost;
        i2c_init();
        return;
    }
    stops_lost = 0;
    uint32_t c = I2C(0x00) & ~0x2010u;
    I2C(0x00) = c;
    I2C(0x20) = I2C_BYTE_DONE | I2C_STOP_DONE;
    I2C(0x00) = c;
}
static uint32_t *start_log;
static COLD int start(unsigned mode, unsigned addr) {
    delay_us(I2C_GAP_US);
    /* bus free = IICSTAT bit5 clear; it never cleared on the device */
    uint32_t t0 = USEC;
    for (uint32_t polls = 0; I2C(0x04) & 0x20u; ++polls)
        if (USEC - t0 >= I2C_FREE_US || polls >= I2C_FREE_US * 64u) {
            ++input_debug.to_free;
            break;
        }
    /* +0x10 (Rockbox s5l87xx "IICUNK10") 0 before an access: measured,
     * bounded, a competing hypothesis (Codex 4/6) */
    t0 = USEC;
    for (uint32_t polls = 0; I2C(0x10) && USEC - t0 < I2C_RDY_US && polls < I2C_RDY_US * 64u; ++polls) ;
    if (I2C(0x10)) ++input_debug.to_rdy;
    /* no stale byte-done, and IICCON bit4 (pending: holds SCL) clear */
    I2C(0x20) = I2C_BYTE_DONE;
    I2C(0x00) = (I2C(0x00) & ~0x10u) | 0x180u;
    I2C(0x04) = mode;
    I2C(0x0c) = addr;
    I2C(0x04) = mode | 0x20;
    int ok = byte_done() && acked();
    if (start_log) {                    /* the boot read's first START */
        *start_log = I2C(0x04) | (I2C(0x20) & 0xffffu) << 8 | I2C(0x10) << 24;
        start_log = 0;
    }
    return ok;
}
/* START, address, then each byte; STOP in every case. 0, or the failed
 * stage (1 address, 2 data). */
static COLD unsigned pmu_send(const uint8_t *bytes, unsigned n) {
    unsigned stage = start(0xd0, 0xe6) ? 0 : 1;
    for (unsigned i = 0; !stage && i < n; ++i) {
        I2C(0x0c) = bytes[i];
        resume();
        /* RetailOS ignores the ACK of the last byte */
        if (!byte_done() || (i + 1 < n && !acked())) stage = 2;
    }
    if (stage) failed(stage);
    stop(0xd0);
    return stage;
}
static SMALL unsigned pmu_write(const uint8_t *bytes, unsigned n) {
    i2c_ungate();
    return pmu_send(bytes, n);
}
/* register pointer write + STOP, then a 1-byte read; -1 if any stage failed (stages 3 read
 * address, 4 read data) */
static COLD int pmu_read_once(unsigned reg) {
    uint8_t r = (uint8_t)reg;
    int result = -1;
    ++input_i2c_n;
    i2c_ungate();
    if (!pmu_send(&r, 1)) {
        if (start(0x90, 0xe7)) {
            resume_ack(0);              /* NACK the only byte */
            if (byte_done()) result = I2C(0x0c) & 0xff;
            else failed(4);
        } else failed(3);
        stop(0x90);
    }
    return result;
}
/* A failure at any stage ends the transfer with a STOP and the whole read
 * starts over: on the device about 3 in 4 address bytes failed. */
static COLD int pmu_read(unsigned reg, unsigned tries) {
    for (unsigned attempt = 0; attempt < tries; ++attempt) {
        if (attempt) ++input_debug.retry;
        int v = pmu_read_once(reg);
        if (v >= 0) return v;
    }
    return -1;
}
static SMALL void pmu_ack(unsigned reg, unsigned bits) {
    uint8_t b[2] = {(uint8_t)reg, (uint8_t)bits};
    pmu_write(b, 2);
}
/* Home level from PMU reg7 bit4, polled every PMU_POLL_US. It changes only
 * after two good reads in a row agree, so one glitched read can neither
 * press nor release Home (a phantom Home once launched StockOS right after
 * the confirm); failed reads keep the level. */
static COLD void poll_home(void) {
    uint32_t now = USEC;
    int line = !(GPIO(0x3cf00144u) & (1u << 6));
    int fast = home_votes || last_failed;
    if (now - last_poll < (fast ? PMU_POLL_US : PMU_IDLE_US)) return;
    last_poll = now;
    trace(6);
    int level = pmu_read(7, I2C_TRIES);
    trace(7);
    last_failed = level < 0;
    if (level < 0) {
        ++input_debug.err;
        /* a bus that jams again: recover again, at most every second */
        if (++fail_run >= 4 && now - last_rec >= EVENT_POLL_US) {
            last_rec = now;
            fail_run = 0;
            input_i2c_recover();
        }
        return;
    }
    fail_run = 0;
    ++input_debug.ok;
    input_debug.r7 = (unsigned)level;
    /* pressed = bit4 clear: RetailOS 0x080fb34c -> 0x083a6f80(14) reads
     * reg7 bit4 active-low (snap6 RE). The first-read idle (home_idle, kept
     * for the debug line) was a guess that one garbage read could invert. */
    unsigned pressed = !((unsigned)level & HOME_BIT);
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
/* I2C1 controller unstick (2026-10-17, AGY rank 3). The handoff can stop
 * RetailOS's interrupt-driven transfer (ISR 0x22001214) mid-byte: the
 * controller is left busy (IICSTAT bit5) or pending (IICCON bit4, SCL
 * held), a PMU mid-read may hold SDA. The pads are dedicated, so the
 * controller itself clocks: in master RX the pending byte is NACKed (the
 * slave lets SDA go after it), then a STOP (re-issued), then the init.
 * The first run (boot, before any other PMU transfer) logs into
 * input_i2c_log (R84 v4 "ib", status and value kept apart):
 *   0-2   CLKCON +14 +4c +6c as found
 *   3-11  I2C1 +00 +04 +08 +0c +10 +14 +18 +20 +28 as found, clock on
 *   12    what was done: bit0 busy/pending found, bit1 RX byte NACKed
 *         (bit2: its byte-done came), bit3 STOP lost | to_stop << 8
 *   13-15 I2C1 +00 +04 +20 after the init
 *   16    first START: IICSTAT | +20 << 8 | +10 << 24
 *   17-21 PMU reg 07 (Home, 8 tries), 00, 4b, 52, 56: status (0 ok, else
 *         the failed stage) | value << 8 (0xff with status != 0 = none)
 * Later runs (Drivers entry, 4 failed Home polls, at most 1/s) only count
 * (input_debug.recs). Returns the done bits. */
uint32_t input_i2c_log[INPUT_I2C_LOG];
COLD uint32_t input_i2c_recover(void) {
    static const uint8_t regs[9] = {0, 4, 8, 0xc, 0x10, 0x14, 0x18, 0x20, 0x28};
    static int logged;
    uint32_t *lg = logged++ ? 0 : input_i2c_log, did = 0;
    if (lg) { lg[0] = CLKCON(0x14); lg[1] = CLKCON(0x4c); lg[2] = CLKCON(0x6c); }
    i2c_ungate();
    if (lg) for (unsigned i = 0; i < 9; ++i) lg[3 + i] = I2C(regs[i]);
    unsigned to = input_debug.to_stop, st = I2C(0x04);
    if ((st & 0x20u) || (I2C(0x00) & 0x10u)) {
        did = 1;
        if ((st & 0xc0u) == 0x80u) {    /* master RX: NACK the byte */
            did |= 2;
            resume_ack(0);
            uint32_t t0 = USEC, spins = 0;
            while (!(I2C(0x20) & I2C_BYTE_DONE) && USEC - t0 < I2C_BYTE_US &&
                   ++spins < I2C_BYTE_US * 64u) ;
            if (I2C(0x20) & I2C_BYTE_DONE) did |= 4;
        }
        stop((st & 0xc0u) | 0x10u);
        if (input_debug.to_stop != to) did |= 8;
    }
    i2c_init();
    if (lg) {
        lg[12] = did | (input_debug.to_stop - to) << 8;
        lg[13] = I2C(0x00); lg[14] = I2C(0x04); lg[15] = I2C(0x20);
        start_log = &lg[16];
    }
    ++input_debug.recs;
    return did;
}
int input_pmu_read(unsigned reg) { return pmu_read(reg, I2C_TRIES); }
int input_pmu_write(unsigned reg, unsigned val) {
    uint8_t b[2] = {(uint8_t)reg, (uint8_t)val};
    return pmu_write(b, 2) ? -1 : 0;
}
unsigned input_home_level(void) { return home_level; }
COLD void input_init(void) {
    /* controller unstick + init (logged), before the first PMU transfer
     * of the kernel (bt.c's come later, at Drivers entry) */
    input_i2c_recover();
    /* Idle level of the Home bit, read before any press (the app was
     * opened by touch); with PMU 00/4b/52/56 into the boot log */
    static const uint8_t regs[5] = {7, 0, 0x4b, 0x52, 0x56};
    int level = -1;
    for (unsigned i = 0; i < 5; ++i) {
        int v = pmu_read(regs[i], i ? 2 : 8);
        if (!i) level = v;
        input_i2c_log[17 + i] = v < 0 ? input_debug.stage | 0xff00u : (uint32_t)v << 8;
    }
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
        pressed |= BUTTON_HOME | BUTTON_CHORD;
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
unsigned input_held(void) { return previous; }
unsigned input_poll(void) {
    unsigned keys = latched | key_edges();
    latched = 0;
    return keys;
}
