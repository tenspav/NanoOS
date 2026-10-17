#include "lcd.h"
#include "input.h"
#include "devmode.h"
#include "version.h"
#include "timer.h"
#include "trace.h"
#include "drivers.h"
#include "mbx.h"
void reset_stockos(void) __attribute__((noreturn));
#define APPS 5
#define DEVMODE 4
static const char *const names[APPS] = {"StockOS", "ViceNano", "PS1Emu", "Settings", "DevMode"};
static const char *const labels[APPS] = {"iP", "VC", "P1", "ST", "DV"};
static const uint32_t colors[APPS] = {0xff2684ff, 0xffff8800, 0xff28b848, 0xff808080, 0xffa04ce0};
/* Only the GPU rate selector changes: CLKCON+0x1c [29:16] (source, both
 * dividers). Its gates [31:30] and the memory domain in [15:0] are kept, and
 * the CPU/system word +0x00 stays stock: raising it also raises the buses
 * hanging off it, and writing whole words zeroed the DRAM clock (hang). */
#define GPU_RATE 0x3fff0000u
static const struct {
    const char *name, *title, *speeds;
    uint32_t gpu;
} profiles[] = {
    {"STOCK", NANOOS_TITLE " | STOCK", "342 MHz CPU / 133 MHz GPU", 0x20010000u},
    {"SAFE", NANOOS_TITLE " | SAFE", "342 MHz CPU / 171 MHz GPU", 0x10030000u},
    {"MAX", NANOOS_TITLE " | MAX", "342 MHz CPU / 266 MHz GPU", 0x20000000u},
};
static unsigned active_profile;
/* Diagnostic line on the selector: PMU reg7 as last read, the Home bit's
 * idle level and the read counts, so a device report shows why Home does
 * or does not register. */
static char *put_hex2(char *p, unsigned v) {
    p[0] = "0123456789ABCDEF"[(v >> 4) & 15];
    p[1] = "0123456789ABCDEF"[v & 15];
    return p + 2;
}
static char *put_dec(char *p, unsigned v) {
    char t[10];
    unsigned n = 0;
    do { unsigned q = 0, r = v;  /* no hardware divide: v / 10 by subtraction */
         while (r >= 10) { r -= 10; ++q; }
         t[n++] = (char)('0' + r); v = q; } while (v && n < sizeof t);
    while (n) *p++ = t[--n];
    return p;
}
static char *put_str(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static void pmu_line(void) {
    char line[54], *p = line;
    p = put_str(p, "PMU r7 "); p = put_hex2(p, input_debug.r7);
    p = put_str(p, " idle "); p = put_hex2(p, input_debug.idle);
    p = put_str(p, " ok "); p = put_dec(p, input_debug.ok & 0xffff);
    p = put_str(p, " err "); p = put_dec(p, input_debug.err & 0xffff);
    *p = 0;
    lcd_center(228, line, 0xff808080);
}
static void clock_selector(unsigned selected) {
    lcd_clear(0xff1a1a1a);
    lcd_center(16, NANOOS_TITLE, WHITE);
    lcd_center(56, "Choose clock profile", WHITE);
    for (unsigned i = 0; i < 3; ++i) {
        int x = 48 + (int)i * 144;
        lcd_text(x, 100, profiles[i].name, WHITE);
        if (i == selected) lcd_text(x - 16, 100, ">", WHITE);
    }
    lcd_center(140, profiles[selected].speeds, WHITE);
    lcd_center(184, "Volume / Left-Right to choose", WHITE);
    lcd_center(208, "Home or Vol- + Vol+ to confirm", WHITE);
    pmu_line();
    lcd_present();
}
static void apply_profile(unsigned selected) {
    volatile uint32_t *gpu = (volatile uint32_t *)0x3c50001cu;
    uint32_t v = *gpu;
    if ((v & GPU_RATE) != profiles[selected].gpu)
        *gpu = (v & ~GPU_RATE) | profiles[selected].gpu;
    __asm__ volatile("dsb sy" ::: "memory");
    active_profile = selected;
}
static void launcher(unsigned selected) {
    lcd_clear(0xff1a1a1a);
    lcd_center(16, profiles[active_profile].title, WHITE);
    for (unsigned i = 0; i < APPS; ++i) {
        int x = LCD_WIDTH / (2 * APPS) + (int)i * (LCD_WIDTH / APPS);
        if (i == selected) lcd_circle(x, 112, 41, WHITE);
        lcd_circle(x, 112, i == selected ? 38 : 32, colors[i]);
        lcd_text(x-8, 108, labels[i], WHITE);
    }
    lcd_center(180, names[selected], WHITE);
    lcd_present();
}
static void placeholder(unsigned target) {
    lcd_clear(0xff1a1a1a);
    lcd_center(100, names[target], WHITE);
    lcd_center(132, "press Home to go back", WHITE);
    lcd_present();
}
void kernel_main(void) {
    unsigned selected = 0, scene = 0, profile = 0;
    unsigned choosing_clock = 1, shown = USEC;
    unsigned drivers_page = 0;     /* DevMode: 0 stats, 1 drivers, 2 GPU */
    struct input_debug pmu_shown;
    lcd_init();
    input_init();
    clock_selector(profile);
    pmu_shown = input_debug;
    for (;;) {
        /* Input every pass: lcd_service() only does a bounded slice of a
         * frame (a copy chunk or one DSI condition), so a short Home press
         * is not lost behind a frame push. */
        trace(8);
        unsigned keys = input_poll();
        lcd_service();
        devmode_tick();
        if (choosing_clock) {
            unsigned old = profile;
            if (keys & BUTTON_LEFT) profile = (profile + 2) % 3;
            if (keys & BUTTON_RIGHT) profile = (profile + 1) % 3;
            if (keys & BUTTON_HOME) {
                apply_profile(profile);
                choosing_clock = 0;
                launcher(selected);
            } else if (profile != old ||
                       /* PMU line: on a new reg7 value at once, counters at
                        * most once a second; a static screen is not redrawn */
                       input_debug.r7 != pmu_shown.r7 ||
                       ((input_debug.ok != pmu_shown.ok || input_debug.err != pmu_shown.err) &&
                        USEC - shown > 1000000u)) {
                clock_selector(profile);
                pmu_shown = input_debug;
                shown = USEC;
            }
        } else if (scene) {
            if (keys & BUTTON_HOME) { scene = 0; drivers_page = 0; launcher(selected); }
            else if (scene == DEVMODE) {
                /* Vol+ opens the Drivers page (probes run on entry),
                 * Vol+ again the MBX GPU test, Vol- goes back to the stats;
                 * on the GPU test Vol+/Vol- select the variant */
                if (drivers_page == 2) mbx_test_key(keys);   /* GPU: variant */
                else {
                    if (keys & BUTTON_RIGHT) {
                        if (++drivers_page == 1) drivers_enter(); else mbx_test_enter();
                    }
                    if (keys & BUTTON_LEFT) drivers_page = 0;
                }
                if (drivers_page == 1) drivers_service();
                /* live screen: draw the next frame once the last one is
                 * queued no more (it is drawn while that one is pushed) */
                if (lcd_ready()) {
                    if (drivers_page == 2) mbx_test_draw();
                    else if (drivers_page) drivers_draw(); else devmode_draw();
                }
            }
        } else {
            unsigned old = selected;
            if (keys & BUTTON_LEFT) selected = (selected + APPS - 1) % APPS;
            if (keys & BUTTON_RIGHT) selected = (selected + 1) % APPS;
            if (selected != old) launcher(selected);
            if (keys & BUTTON_HOME) {
                if (!selected) reset_stockos();
                scene = selected;
                if (scene == DEVMODE) { devmode_enter(); devmode_draw(); }
                else placeholder(scene);
            }
        }
    }
}
