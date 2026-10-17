#include "lcd.h"
#include "timer.h"
#include "trace.h"
#define REG(o) (*(volatile uint32_t *)(0x38900000u + (o)))
#define BRIDGE(o) (*(volatile uint32_t *)(0x38300000u + (o)))
#define DSI(o) (*(volatile uint32_t *)(0x3d800000u + (o)))
/* round 8 size: the push state machine and setup are register sequences
 * paced by the hardware, not by the CPU: Thumb -Os (as input.c's COLD);
 * the drawing primitives stay ARM -O2 */
#define COLD __attribute__((target("thumb"), optimize("Os")))
/* Two scan-out buffers beyond the kernel/stack (never a RetailOS buffer,
 * which might overlap the relocated kernel), drawn in place in the panel's
 * native portrait layout: logical landscape (x, y) -> panel (239 - y, x),
 * i.e. rotated clockwise; view with the panel turned counterclockwise.
 * With the MMU off every access is uncached, and copying a private buffer
 * cost two of the three accesses per pixel (DevMode 6-7 FPS on the device
 * and in NanoEmu --real-ipod). Instead, a push flips window 0 to the frame
 * just drawn and drawing moves to the other buffer, as RetailOS does
 * (commit 0x08080b70 writes +0x24, then the push 0x080a06c8): the buffer
 * being transferred is never written, and the next frame is drawn while
 * the current one is pushed. */
#define BUF0 0x08200000u
#define BUF1 0x08280000u    /* 0x65400 bytes each */
static uint32_t *fb = (uint32_t *)BUF1;    /* drawing (back) buffer */
#define FB_WORDS (LCD_PANEL_WIDTH * LCD_PANEL_HEIGHT)
/* Scan-out window: format, stride, origin, size, then enable. Also used
 * to recover a controller that stopped answering (lcd_stats.recovered). */
static COLD void window(void) {
    REG(0x20) = 0x002107ffu;
    REG(0x28) = LCD_PANEL_WIDTH;
    REG(0x2c) = 0;
    REG(0x30) = (LCD_PANEL_WIDTH << 16) | LCD_PANEL_HEIGHT;
    REG(0x34) = 0;
    /* window0 on; window1 (bit 5, an overlay RetailOS may leave on over
     * reclaimed memory) off; bit 8 untouched */
    REG(0x04) = (REG(0x04) & ~0x20u) | 0x10u;
}
COLD void lcd_init(void) {
    REG(0x24) = BUF0;
    fb = (uint32_t *)BUF1;
    window();
}
void lcd_pixel(int x, int y, uint32_t c) {
    if ((unsigned)x < LCD_WIDTH && (unsigned)y < LCD_HEIGHT)
        fb[x * LCD_PANEL_WIDTH + LCD_PANEL_WIDTH - 1 - y] = c;
}
void lcd_clear(uint32_t c) {
    for (unsigned i = 0; i < FB_WORDS; ++i) fb[i] = c;
}
/* logical rows [y0, y1): one contiguous run per panel row (logical x) */
void lcd_fill_rows(int y0, int y1, uint32_t c) {
    for (int x = 0; x < LCD_WIDTH; ++x) {
        uint32_t *p = fb + x * LCD_PANEL_WIDTH + LCD_PANEL_WIDTH - y1;
        for (int y = y0; y < y1; ++y) *p++ = c;
    }
}
/* The pixels with dx*dx + dy*dy <= r*r (as the old square scan), drawn as
 * one contiguous run per logical column: h = floor(sqrt(r*r - dx*dx)). */
void lcd_circle(int x, int y, int r, uint32_t c) {
    for (int dx = -r, h = 0; dx <= r; ++dx) {
        int rr = r * r - dx * dx, px = x + dx;
        while ((h + 1) * (h + 1) <= rr) ++h;
        while (h * h > rr) --h;
        if ((unsigned)px >= LCD_WIDTH) continue;
        int y0 = y - h < 0 ? 0 : y - h, y1 = y + h >= LCD_HEIGHT ? LCD_HEIGHT - 1 : y + h;
        uint32_t *p = fb + px * LCD_PANEL_WIDTH + LCD_PANEL_WIDTH - 1 - y1;
        for (int k = y0; k <= y1; ++k) *p++ = c;
    }
}
/* Each character and glyph row is read once into a register: the string
 * lives in uncached DRAM, and as the pixel stores may alias it the compiler
 * used to reload the character for every one of the 64 cells. */
void lcd_text(int x, int y, const char *s, uint32_t c) {
    for (unsigned char ch; (ch = (unsigned char)*s) != 0; ++s, x += 8) {
        const uint8_t *glyph = FONT_GLYPH(ch);
        for (int row = 0; row < 8; ++row) {
            unsigned bits = glyph[row];
            for (int col = 0; bits; ++col, bits >>= 1)
                if (bits & 1) lcd_pixel(x+col, y+row, c);
        }
    }
}
void lcd_center(int y, const char *s, uint32_t c) {
    int n = 0;
    while (s[n]) ++n;
    lcd_text((LCD_WIDTH - n*8)/2, y, s, c);
}
/* The panel is command-mode MIPI DSI with its own frame memory: the LCD
 * controller's window only reaches the glass when a frame is pushed. After
 * the handoff RetailOS no longer does that, so replicate its push 0x080a06c8
 * (snap6 disassembly): bridge 0x38300080 = 1, DCS 0x2a / 0x2b window as long
 * writes, DCS 0x2c short packet starts the controller-fed pixel transfer,
 * then 0x38300080 = 0.
 *
 * lcd_present() only marks the frame; lcd_service(), called every main-loop
 * pass, flips to it once the previous push is over and steps the push one
 * hardware condition at a time, so input keeps being sampled while a frame
 * is in flight. Every wait has a deadline (NanoEmu does not model the
 * bridge; a stuck bit must not hang the kernel) and a poll-count backstop
 * for a frozen USEC; both are counted apart in lcd_stats. */
struct lcd_stats lcd_stats;
enum { IDLE, WAIT_IDLE = 2, WIN_COL, WIN_PAGE, MEMWRITE, BUSY_RISE,
       BUSY_FALL, SETTLE };         /* numbers as shown by DevMode "st" */
static unsigned state = IDLE, dirty, polls;
static unsigned expired_now, expired_run;  /* this push / pushes in a row */
#define RECOVER_AFTER 3u
static uint32_t t0, flipped, sent;   /* sent: DCS 0x2c went out */
#define SPIN_US 200u                /* poll in place this long, then yield */
#define POLL_BACKSTOP 100000u
#define BUSY_HOLD_US 17000u        /* one 60 Hz frame */
static void enter(unsigned s) { state = s; t0 = USEC; polls = 0; }
static COLD void dsi_long(uint32_t w0, uint32_t w1) {
    DSI(0x10) &= ~(1u << 28);
    DSI(0x38) = w0;
    DSI(0x38) = w1;
    DSI(0x34) = 0x0529;                     /* DCS long write, 5 bytes */
}
/* WAIT_OK when ok, WAIT_TIMEOUT / WAIT_SPIN when out of time, else 2
 * (keep polling on a later pass) */
static int check(volatile uint32_t *r, uint32_t mask, uint32_t want, int any,
                 uint32_t limit_us) {
    uint32_t start = USEC;
    do {
        uint32_t v = *r & mask;
        if (any ? v != 0 : v == want) return WAIT_OK;
        if (USEC - t0 >= limit_us) return WAIT_TIMEOUT;
        if (++polls >= POLL_BACKSTOP) return WAIT_SPIN;
    } while (USEC - start < SPIN_US);
    return 2;
}
/* an expired wait proceeds anyway, as the blocking version did */
static COLD int expired(int r) {
    if (r == WAIT_TIMEOUT) { ++lcd_stats.timeouts; lcd_stats.last = state; expired_now = 1; }
    if (r == WAIT_SPIN) { ++lcd_stats.spins; lcd_stats.last = state; expired_now = 1; }
    return r != 2;
}
static COLD void finish(void) {
    trace(2);
    BRIDGE(0x80) = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    ++lcd_stats.frames;
    lcd_stats.push_us = USEC - flipped;
    lcd_stats.end_us = USEC - sent;
    state = IDLE;
    /* Several pushes in a row with an expired wait: the controller may
     * have lost its window setup (it is idempotent to write it again).
     * The DSI side is not reset: RetailOS's link setup is not known. */
    expired_run = expired_now ? expired_run + 1 : 0;
    expired_now = 0;
    if (expired_run >= RECOVER_AFTER) {
        window();
        expired_run = 0;
        ++lcd_stats.recovered;
    }
}
COLD void lcd_service(void) {
    for (;;) {
        switch (state) {
        case IDLE:
            if (!dirty) return;
            dirty = 0;
            /* the drawn frame becomes the scan-out, drawing moves on */
            __asm__ volatile("dsb sy" ::: "memory");
            REG(0x24) = (uint32_t)fb;
            fb = (uint32_t *)((uint32_t)fb == BUF0 ? BUF1 : BUF0);
            flipped = lcd_stats.flipped = USEC; trace(1);
            enter(WAIT_IDLE);
            break;
        case WAIT_IDLE:                     /* previous push done */
            if (!expired(check(&BRIDGE(0x8c), 3, 0, 0, 50000))) return;
            BRIDGE(0x80) = 1;
            dsi_long(0x0000002a, 0xef);     /* columns 0..239 */
            enter(WIN_COL);
            break;
        case WIN_COL:                       /* FIFOs empty */
            if (!expired(check(&DSI(0x44), 0x500000u, 0x500000u, 0, 50000))) return;
            dsi_long(0x0100002b, 0xaf);     /* pages 0..431 */
            enter(WIN_PAGE);
            break;
        case WIN_PAGE:
            if (!expired(check(&DSI(0x44), 0x500000u, 0x500000u, 0, 50000))) return;
            DSI(0x34) = 0x2c05;             /* DCS short: memory write */
            sent = USEC;
            enter(MEMWRITE);
            break;
        case MEMWRITE:
            if (!expired(check(&DSI(0x44), 0x400000u, 0x400000u, 0, 50000))) return;
            enter(BUSY_RISE);
            break;
        case BUSY_RISE: {
            /* RetailOS waits on an OS event here (source unresolved). Wait
             * for the bridge busy bits to rise and fall; if they never
             * rise, give the transfer one 60 Hz frame. */
            int r = check(&BRIDGE(0x8c), 3, 0, 1, 2000);
            if (r == 2) return;
            lcd_stats.rise_us = USEC - sent; trace(3);
            if (r == WAIT_OK) enter(BUSY_FALL);
            else { ++lcd_stats.norise; enter(SETTLE); }
            /* Both time the transfer from the 0x2c, not from when this
             * pass got here: the next pass may come a whole frame of
             * drawing later (NanoEmu --real-ipod: every push took two
             * frames, 17 FPS instead of 35). */
            t0 = sent;
            break;
        }
        case BUSY_FALL: {
            /* On the device the busy bits rise but never fall while
             * 0x38300080 is 1 (every push used to expire here, 50 ms, in
             * state 7): RetailOS ends the push on an OS event instead. Give
             * the transfer BUSY_HOLD_US, then end it; not a timeout. */
            int r = check(&BRIDGE(0x8c), 3, 0, 0, BUSY_HOLD_US);
            if (r == 2) return;
            if (r != WAIT_OK) ++lcd_stats.held;
            finish();
            return;
        }
        case SETTLE:
            if (USEC - t0 < 17000u && ++polls < POLL_BACKSTOP) return;
            finish();
            return;
        }
    }
}
int lcd_busy(void) { return dirty || state != IDLE; }
int lcd_ready(void) { return !dirty; }
uint32_t *lcd_back(void) { return fb; }
void lcd_present(void) { dirty = 1; }
/* Present and wait for the push to finish (before a reset or the like). */
#define FLUSH_US 200000u   /* > the longest push: 4 waits of 50 ms */
COLD void lcd_flush(void) {
    lcd_present();
    uint32_t t0 = USEC;
    for (uint32_t passes = 0; lcd_busy() && USEC - t0 < FLUSH_US &&
         passes < POLL_BACKSTOP; ++passes)
        lcd_service();
}
