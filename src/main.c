#include "lcd.h"
#include "input.h"
#include "devmode.h"
#include "version.h"
#include "timer.h"
#include "trace.h"
#include "drivers.h"
#include "mbx.h"
#include "clk.h"
#include "fmt.h"
#include "touch.h"
#include "btui.h"
void reset_stockos(void) __attribute__((noreturn));
#define APPS 5
#define SETTINGS 3   /* Bluetooth Central (btui.c) */
#define DEVMODE 4
static const char *const names[APPS] = {"StockOS", "ViceNano", "PS1Emu", "Settings", "DevMode"};
static const char *const labels[APPS] = {"iP", "VC", "P1", "ST", "DV"};
static const uint32_t colors[APPS] = {0xff2684ff, 0xffff8800, 0xff28b848, 0xff808080, 0xffa04ce0};
#define BG 0xff1a1a1au
#define GRAY 0xff808080u
/* CPU: PLL-A retuned to a firmware-table rate, buses kept at or under
 * stock; GPU: only the rate field CLKCON+0x1c [29:16] (gates and the
 * memory domain [15:0] kept). Both in clk.c, logged to R86. SAFE's GPU
 * (PLL-A/4) becomes PLL-A 432 / 3 = 144 once the CPU retune applies.
 * chip: the profile's colour on the selector and the launcher's top bar. */
static const struct {
    const char *name;
    uint32_t gpu, chip;
    uint16_t cpu_mhz, gpu_mhz;   /* the targets, shown on the selector */
} profiles[] = {
    {"STOCK", 0x20010000u, 0xff606060u, 342, 133},
    {"SAFE", 0x10030000u, 0xff20a050u, 432, 144},
    {"MAX", 0x20000000u, 0xffd03838u, 532, 266},
};
static unsigned active_profile, selected;
static void rect(int x, int y, int w, int h, uint32_t c) {
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) lcd_pixel(x + i, y + j, c);
}
/* font8x8 with k x k pixels per dot, centred on cx */
static void text_k(int cx, int y, const char *s, int k, uint32_t c) {
    int n = 0;
    while (s[n]) ++n;
    for (int x = cx - n * 4 * k; *s; ++s, x += 8 * k)
        for (int row = 0; row < 8; ++row)
            for (int col = 0; col < 8; ++col)
                if (FONT_GLYPH((unsigned char)*s)[row] >> col & 1) rect(x + col * k, y + row * k, k, k, c);
}
#define BOX 144   /* selector: profile i is x in [i * BOX, (i + 1) * BOX) */
static void clock_selector(unsigned selected) {
    lcd_clear(BG);
    lcd_center(16, NANOOS_TITLE, WHITE);
    lcd_center(56, "Choose clock profile", WHITE);
    for (unsigned i = 0; i < 3; ++i) {
        rect((int)i * BOX + 12, 80, BOX - 24, 44, i == selected ? profiles[i].chip : 0xff303030u);
        text_k((int)i * BOX + BOX / 2, 94, profiles[i].name, 2, WHITE);
    }
    fmt_dec(profiles[selected].cpu_mhz); fmt_str(" MHz CPU / ");
    fmt_dec(profiles[selected].gpu_mhz); fmt_str(" MHz GPU");
    fmt_line((LCD_WIDTH - 25 * 8) / 2, 140, WHITE);   /* 25 characters */
    lcd_center(196, "tap or Volume: choose - tap again or Home: start", WHITE);
    lcd_present();
}
static void apply_profile(unsigned selected) {
    clk_apply(selected, profiles[selected].gpu);
    active_profile = selected;
}
/* Launcher: top bar (title, applied clocks, profile chip; a tap on it opens
 * DevMode) over a horizontal carousel of circles. The selected icon is
 * centred and biggest; after a step every icon slides in from where it
 * was (slide decays to 0, one pushed frame at a time). */
#define TOP_H 34
#define CX (LCD_WIDTH / 2)
#define CY 116
#define SPACING 100
static int slide, drawn;   /* drawn: slide * 8 + selected on screen */
/* Partial redraw (round 9): an animation frame (anim set by the caller)
 * redraws only the carousel/name band BAND_Y0..BAND_Y1 (icons y 63..169,
 * name 178..193; full width = the union of old and new extents) into a
 * buffer whose top bar this launcher session drew in full (bars: up to
 * both buffers; the back buffer is two frames old). Any other call (entry
 * from the selector or a scene: profile, DevMode, ViceNano, ... drew
 * since) forgets both and draws the whole screen. */
#define BAND_Y0 60
#define BAND_Y1 196
static int anim;
static uint32_t *bars[2];
static void step(int dir) {
    selected = (selected + APPS + dir) % APPS;
    slide += dir * SPACING;
}
static void launcher(unsigned selected) {
    static const signed char order[] = {-2, 2, -1, 1, 0};   /* centre on top */
    uint32_t *b = lcd_back();
    if (!anim) bars[0] = bars[1] = 0;
    anim = 0;
    if (b == bars[0] || b == bars[1]) lcd_fill_rows(BAND_Y0, BAND_Y1, BG);
    else {
        bars[bars[0] != 0] = b;
        lcd_clear(BG);
        lcd_text(6, 6, NANOOS_TITLE, WHITE);
        /* applied clocks read back from CLKCON; "*" = PLL unreadable (stock
         * rate shown). A refused CPU retune shows as 342 (R86 says why). */
        int nominal = 0;
        fmt_str("CPU "); fmt_dec(clk_cpu_khz(&nominal) / 1000);
        fmt_str(" MHz  GPU "); fmt_dec(clk_gpu_khz(&nominal) / 1000);
        fmt_str(nominal ? " MHz *" : " MHz");
        fmt_line(6, 20, 0xff60d0ffu);
        rect(LCD_WIDTH - 92, 4, 88, 26, profiles[active_profile].chip);
        text_k(LCD_WIDTH - 48, 9, profiles[active_profile].name, 2, WHITE);
    }
    for (unsigned n = 0; n < 5; ++n) {
        int k = order[n], x = CX + k * SPACING + slide;
        int d = x < CX ? CX - x : x - CX, r = 50 - d * 18 / SPACING;
        unsigned i = (selected + APPS + k) % APPS;
        if (r < 8) r = 8;
        int s = (unsigned)r / 16;   /* label scale: 3 centred, 2 beside, far ones none */
        if (!k) lcd_circle(x, CY, r + 3, WHITE);
        lcd_circle(x, CY, r, colors[i]);
        text_k(x, CY - 4 * s, labels[i], s, WHITE);
    }
    text_k(CX, 178, names[selected], 2, WHITE);
    drawn = slide * 8 + (int)selected;
    lcd_present();
}
static void placeholder(unsigned target) {
    lcd_clear(BG);
    lcd_center(100, names[target], WHITE);
    lcd_center(132, "tap or Home: back", WHITE);
    lcd_present();
}
/* Touch as buttons, one contact at a time: a swipe (SWIPE_PX or more left
 * or right of where it went down) is Vol- / Vol+ as the content follows
 * the finger, sent as soon as a MOVE (or the UP) gets that far, once per
 * contact (touch_x0 -2 = consumed until the UP); an UP short of it is a
 * TAP at touch.x / .y. A DOWN restarts the contact, so a lost UP report
 * costs nothing; an UP with no DOWN or MOVE before it (queued from
 * before) is no tap. */
#define TAP 16u
#define SWIPE_PX 18
static int touch_x0 = -1;   /* -1 no contact, -2 swipe sent */
static unsigned gesture(void) {
    if (!touch_poll()) return 0;
    int up = touch.state == TOUCH_UP, was = touch_x0;
    if (!up && (was == -1 || touch.state == TOUCH_DOWN)) { touch_x0 = touch.x; return 0; }
    if (up) touch_x0 = -1;
    if (was < 0) return 0;
    int dx = touch.x - was;
    unsigned e = dx >= SWIPE_PX ? BUTTON_LEFT : dx <= -SWIPE_PX ? BUTTON_RIGHT : up ? TAP : 0;
    if (e && !up) touch_x0 = -2;
    return e;
}
void kernel_main(void) {
    unsigned scene = 0, profile = 0;
    unsigned choosing_clock = 1;
    unsigned drivers_page = 0;     /* DevMode: 0 stats, 1 drivers, 2 GPU */
    lcd_init();
    input_init();
    touch_start();
    clock_selector(profile);
    for (;;) {
        /* Input every pass: lcd_service() only does a bounded slice of a
         * frame (a copy chunk or one DSI condition), so a short Home press
         * is not lost behind a frame push. */
        trace(8);
        /* touch first, before the PMU I2C poll that can block for ms.
         * Everywhere but ViceNano (it reads touch itself: drag = camera,
         * the gesture is dropped, so no touch queued in a game comes out as
         * a tap on the launcher); in a scene a tap goes back like Home:
         * anywhere on a placeholder, on DevMode's title strip (a swipe
         * pages DevMode like the volume keys) */
        unsigned tap = gesture();
        unsigned keys = input_poll();
        lcd_service();
        devmode_tick();
        if (scene == 1) tap = 0;
        if (tap != TAP) keys |= tap, tap = 0;
        if (tap && scene && (scene < SETTINGS || touch.y < TOP_H)) keys |= BUTTON_HOME;
        if (scene == SETTINGS) keys = btui_tick(keys, tap);
        if (choosing_clock) {
            unsigned old = profile;
            if (keys & BUTTON_LEFT) profile = (profile + 2) % 3;
            if (keys & BUTTON_RIGHT) profile = (profile + 1) % 3;
            /* tap a profile to choose it, the chosen one again to confirm */
            if (tap && touch.y >= 70 && touch.y < 134) {
                unsigned i = (unsigned)touch.x / BOX;
                if (i == profile) keys |= BUTTON_HOME;
                profile = i;
            }
            if (keys & BUTTON_HOME) {
                apply_profile(profile);
                choosing_clock = 0;
                launcher(selected);
            } else if (profile != old) clock_selector(profile);
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
            if (keys & BUTTON_LEFT) step(-1);
            if (keys & BUTTON_RIGHT) step(1);
            /* top bar: DevMode; the centre icon: open; a side one:
             * bring it to the centre */
            if (tap && touch.y < TOP_H) { selected = DEVMODE; keys |= BUTTON_HOME; }
            else if (tap && touch.y < CY + 80) {
                int k = (touch.x + 2 * SPACING + SPACING / 2 - CX) / SPACING - 2;
                if (k) step(k); else keys |= BUTTON_HOME;
            }
            if (keys & BUTTON_HOME) {
                slide = 0;
                if (!selected) reset_stockos();
                scene = selected;
                if (scene == DEVMODE) { devmode_enter(); devmode_draw(); }
                else if (scene == SETTINGS) btui_enter();
                else placeholder(scene);
            } else if (lcd_ready()) {
                /* one animation step per pushed frame */
                slide = slide * 2 / 3;
                if (slide < 4 && slide > -4) slide = 0;
                if (slide * 8 + (int)selected != drawn) { anim = 1; launcher(selected); }
            }
        }
    }
}
