#include "devmode.h"
#include "lcd.h"
#include "mem.h"
#include "timer.h"
#include "input.h"
#include "trace.h"
#include "fmt.h"
#define CLKCON(o) (*(volatile uint32_t *)(0x3c500000u + (o)))
#define DRAM_BASE 0x08000000u
#define DRAM_SIZE 0x00200000u /* kernel.ld DRAM region */
extern uint8_t _stack_top[];
static uint32_t frames, window_start, fps_x10, draw_us, after_flip;  /* frames: lcd_stats at window start */
static int fps_valid;
/* Cortex-A5 has no hardware divide and we link without libgcc. */
static uint32_t udiv(uint32_t n, uint32_t d) {
    uint32_t q = 0, r = 0;
    if (!d) return 0;
    for (int i = 31; i >= 0; --i) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= 1u << i; }
    }
    return q;
}
/* Line formatting is fmt.c's (one shared, out-of-line copy: inlined here
 * it cost 3 KB, and the kernel must fit the chainloader slot). */
#define put fmt_str
#define putu fmt_dec
#define puthex_n fmt_hex
static void puthex(uint32_t v) { put("0x"); fmt_hex(v, 8); }
static void put_tenths(uint32_t x10) { putu(x10 / 10); put("."); putu(x10 % 10); }
static void row(int y, const char *label) { lcd_text(40, y, label, WHITE); }
static void flush(int x, int y) { fmt_line(x, y, WHITE); }
/* Source field: 0 = 24 MHz oscillator, 1..3 = PLL-A/B/C (PMS at +0x20/24/28,
 * Fout = 24 MHz * M / (P * 2^S)). An implausible PMS (NanoEmu leaves the
 * PLL registers near the all-ones regfile fill, e.g. 0xFFFFFFF8) falls back to the documented
 * stock rate and sets *nominal. */
static const char *const src_names[] = {"OSC", "PLL-A", "PLL-B", "PLL-C"};
static uint32_t src_khz(unsigned src, int *nominal) {
    static const uint32_t stock[] = {684000, 266000, 486000};
    if (!src) return 24000;
    uint32_t v = CLKCON(0x1c + 4 * src);
    uint32_t p = (v >> 23) & 63, m = (v >> 3) & 1023, s = v & 7;
    /* Plausible PMS: reserved [22:13] clear, 24 MHz / P >= 1 MHz. */
    uint32_t khz = p && p <= 24 && !(v & 0x007fe000u) ? udiv(24000u * m, p) >> s : 0;
    if (khz >= 24000 && khz <= 1200000) return khz;
    *nominal = 1;
    return stock[src - 1];
}
static void clock_row(int y, const char *label, unsigned src, uint32_t div, int *nominal) {
    int est = 0;
    uint32_t khz = udiv(src_khz(src, &est), div);
    *nominal |= est;
    row(y, label);
    put_tenths(khz / 100); put(" MHz");
    flush(112, y);
    put(src_names[src]); put("/"); putu(div);
    if (est) put(" *");
    flush(232, y);
}
/* FPS = frames that completed the panel push, not loop passes */
void devmode_tick(void) {
    uint32_t now = USEC, elapsed = now - window_start;
    if (elapsed >= 1000000u) {
        fps_x10 = udiv((lcd_stats.frames - frames) * 10000u, elapsed / 1000u);
        fps_valid = 1;
        frames = lcd_stats.frames;
        window_start = now;
    }
}
void devmode_enter(void) {
    frames = lcd_stats.frames;
    fps_valid = 0;
    window_start = USEC;
}
void devmode_draw(void) {
    uint32_t started = USEC; trace(4);
    after_flip = started - lcd_stats.flipped;
    uint32_t cpu = CLKCON(0x00), gpu = CLKCON(0x1c);
    uint32_t kernel = (uint32_t)_stack_top - DRAM_BASE;
    int nominal = 0;
    lcd_clear(0xff1a1a1a);
    lcd_center(16, "NanoOS DevMode", WHITE);
    /* CPU: CLKCON+0x00 source [13:12], divisor [3:0]+1.
     * GPU: CLKCON+0x1c source [29:28], divisors [19:16]+1 and [23:20]+1. */
    clock_row(48, "CPU", (cpu >> 12) & 3, (cpu & 15) + 1, &nominal);
    clock_row(68, "GPU", (gpu >> 28) & 3,
              (((gpu >> 16) & 15) + 1) * (((gpu >> 20) & 15) + 1), &nominal);
    row(88, "RAM");
    putu((kernel + heap_used()) >> 10); put(" / "); putu(DRAM_SIZE >> 10); put(" KiB");
    flush(112, 88);
    row(108, "Heap");
    putu(heap_used() >> 10); put(" / "); putu(heap_total() >> 10); put(" KiB");
    flush(112, 108);
    row(128, "FPS");
    if (fps_valid) put_tenths(fps_x10); else put("--");
    flush(112, 128);
    /* last frame: drawing time, flip-to-end of its push */
    put("draw "); putu(draw_us / 1000); put(" ms push "); putu(lcd_stats.push_us / 1000);
    put(" ms");
    flush(232, 128);
    /* expired waits of the LCD push: deadline / backstop, state of the
     * last one; pushes without busy bits / with busy bits held */
    put("D "); putu(after_flip); put(" R "); putu(lcd_stats.rise_us); put(" E "); putu(lcd_stats.end_us); put(" ");
    put("LCD to "); putu(lcd_stats.timeouts); put(" sp "); putu(lcd_stats.spins);
    put(" st "); putu(lcd_stats.last); put(" nr "); putu(lcd_stats.norise);
    put(" hd "); putu(lcd_stats.held);
    flush(8, 144);
    put("CLK+00 "); puthex(cpu); put("  CLK+1C "); puthex(gpu);
    flush(8, 160);
    put("CLK+04 "); puthex(CLKCON(0x04)); put("  PLL-A  "); puthex(CLKCON(0x20));
    flush(8, 176);
    /* "*" after a clock = PLL unreadable, stock rate shown */
    (void)nominal;
    /* PMU I2C: Home polls ok/failed, register reads retried, stage and
     * controller registers of the last failure */
    put("I2C ok "); putu(input_debug.ok & 0xffff); put(" er "); putu(input_debug.err & 0xffff);
    put(" rt "); putu(input_debug.retry & 0xffff);
    put(" st "); putu(input_debug.stage); put(" C "); puthex_n(input_debug.con, 4);
    put(" S "); puthex_n(input_debug.stat, 2); put(" E "); puthex_n(input_debug.ev, 4);
    flush(8, 192);
    /* I2C timeouts by site: byte done / bus free / STOP done (the device's
     * GATE words, the line here before, are recorded in boot-progress) */
    put("I2C TO byte "); putu(input_debug.to_byte & 0xffff);
    put(" free "); putu(input_debug.to_free & 0xffff);
    put(" stop "); putu(input_debug.to_stop & 0xffff);
    flush(8, 208);
    lcd_center(226, "Home: back", WHITE);
    draw_us = USEC - started; trace(5);
    lcd_present();
}
