/*
 * loader.c -- NanoOS Loader: a RetailOS raw-surface app that shows the NanoOS
 * launcher (docs/nanoos-launcher-spec.md) and boots the bare-metal kernel.
 *
 *   Volume Down / Up    move the selection left / right (wraps)
 *   Home                select. RetailOS owns Home, so on StockOS the app does
 *                       nothing and lets the OS return to its Home screen. On
 *                       the other icons the boot starts the moment Home is
 *                       seen and hands off a few frames later, before the OS
 *                       tears the app down. If the boot fails (no kernel) the
 *                       error shows until that teardown takes the app home.
 *   tap                 best effort: tap an icon to move to it, tap the selected
 *                       icon again to launch it. RetailOS gives apps no exit
 *                       call, so a tapped StockOS shows "press Home". Taps come
 *                       from the SDK's surface touch read, which needs the
 *                       resident's touch mailbox; with the snap6 disk's
 *                       resident it saw no taps in NanoEmu (OS list drained
 *                       first), so the buttons are the supported input.
 * Play/Pause is not used: RetailOS starts music and opens Now Playing on it.
 * RetailOS also draws its volume overlay (and changes the volume) on Vol -/+,
 * and while it does, it can stall this app's frame callback for 100-400 ms in
 * NanoEmu. Buttons are level-sampled once per frame (SDK GPIO read), so a very
 * short volume tap can fall inside a stall and be missed; press a bit longer.
 *
 * ViceNano / PS1Emu / Settings all boot NanoOS; inside it they are the
 * placeholders of the v0.0.1 beta spec. The boot runs over several frames so the
 * screen always shows where it is (CLAUDE.md "BUSY..." rule):
 *   1. BUSY drawn the frame the button is seen
 *   2. read NANOOS_KERNEL_PATH whole into an OS-heap staging buffer
 *   3. let the filesystem settle a few frames (file closed, FMC idle)
 *   4. probe/chainloader: hw_shutdown_all(), then handoff_to_payload(), which
 *      escalates through the SVC hook, turns the MMU and caches off, copies the
 *      kernel to 0x08000000 and jumps there. The display stops being refreshed
 *      by shutdown, so the "handing off" frame stays up until NanoOS draws.
 *
 * The launcher is drawn as a 432x240 landscape frame rotated into the 240x432
 * portrait framebuffer exactly like nanoos/src/lcd.c, with the same font, so
 * it matches the NanoOS screen pixel for pixel (except the hint line).
 */
#include <stdint.h>
#include "hb_sdk.h"
#include "hb_heap.h"
#include "hb_raw_surface.h"
#include "shutdown.h"
#include "handoff.h"

#ifndef NANOOS_KERNEL_PATH
#define NANOOS_KERNEL_PATH "/Apps/NanoOS/kernel.bin"
#endif
/* nanoos/kernel.ld: code + data + BSS + stack must stay below 0x08200000 */
#define KERNEL_MAX_BYTES (2u * 1024u * 1024u)
/* Frames between the kernel read and the shutdown: the file is closed by then
 * and hw_shutdown_all() also waits out an in-flight FMC operation. Kept short
 * on purpose: RetailOS acts on the same Home press and tears the app down a
 * moment later, so the handoff must win that race. */
#define SETTLE_FRAMES 4

#define W 432
#define H 240
#define BG 0xff1a1a1au
#define WHITE 0xffffffffu
#define RED 0xffff5050u

extern const uint8_t font8x8[128][8];        /* nanoos/src/font.c, bit 0 left */
extern const uint8_t loader_font8x8[96][8];  /* font_full.c, bit 7 left */

static const char *const names[] = {"StockOS", "ViceNano", "PS1Emu", "Settings"};
static const char *const labels[] = {"iP", "VC", "P1", "ST"};
static const uint32_t colors[] = {0xff2684ff, 0xffff8800, 0xff28b848, 0xff808080};

enum { MENU, STOCK_HINT, BOOT_READ, BOOT_SETTLE, BOOT_HANDOFF, BOOT_ERROR };
static int state, selected, settle, no_fb;
static unsigned prev_buttons;
static int touch_was_down, touch_x, touch_y;
static uint8_t *kernel;
static uint32_t kernel_len;

/* ---------------------------------------------------------------- drawing */

static void pixel(int x, int y, uint32_t c)
{
    if ((unsigned)x < W && (unsigned)y < H)
        hb_raw_fb()[x * 240 + 239 - y] = c;
}

static void clear(uint32_t c)
{
    uint32_t *fb = hb_raw_fb();
    for (int i = 0; i < 240 * 432; ++i) fb[i] = c;
}

static void circle(int x, int y, int r, uint32_t c)
{
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx)
            if (dx * dx + dy * dy <= r * r) pixel(x + dx, y + dy, c);
}

static void rect(int x, int y, int w, int h, uint32_t c)
{
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) pixel(x + i, y + j, c);
}

/* NanoOS glyphs where it has them (so shared text matches), SDK ones otherwise */
static void text(int x, int y, const char *s, uint32_t c)
{
    for (; *s; ++s, x += 8) {
        unsigned ch = (unsigned char)*s & 127;
        const uint8_t *g = font8x8[ch];
        int own = g[0] | g[1] | g[2] | g[3] | g[4] | g[5] | g[6] | g[7];
        for (int row = 0; row < 8; ++row)
            for (int col = 0; col < 8; ++col) {
                int on = own ? g[row] & (1u << col)
                             : ch >= 32 && (loader_font8x8[ch - 32][row] & (0x80u >> col));
                if (on) pixel(x + col, y + row, c);
            }
    }
}

static int len(const char *s) { int n = 0; while (s[n]) ++n; return n; }
static void center(int y, const char *s, uint32_t c) { text((W - len(s) * 8) / 2, y, s, c); }

static char *put_uint(char *p, uint32_t v)
{
    char tmp[10];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = tmp[--n];
    *p = 0;
    return p;
}

static void draw_launcher(void)
{
    clear(BG);
    center(16, "NanoOS v0.0.1 beta by Tenspav", WHITE);
    for (int i = 0; i < 4; ++i) {
        int x = W / 8 + i * (W / 4);
        if (i == selected) circle(x, 112, 41, WHITE);
        circle(x, 112, i == selected ? 38 : 32, colors[i]);
        text(x - 8, 108, labels[i], WHITE);
    }
    center(180, names[selected], WHITE);
    center(220, "Vol -/+ move   Home select", 0xff808080u);
}

static void draw_busy(int percent, const char *line1, const char *line2)
{
    clear(BG);
    center(40, "BUSY...", WHITE);
    center(64, "Booting NanoOS", WHITE);
    center(100, line1, WHITE);
    if (line2) center(116, line2, WHITE);
    rect(56, 150, 320, 16, 0xff404040u);
    rect(56, 150, 320 * percent / 100, 16, colors[2]);
    char pct[8], *e = put_uint(pct, (uint32_t)percent);
    e[0] = '%';
    e[1] = 0;
    center(180, pct, WHITE);
}

static void draw_message(const char *title, const char *line1, const char *line2, uint32_t c)
{
    clear(BG);
    center(80, title, c);
    center(116, line1, WHITE);
    center(132, line2, WHITE);
    center(220, "Vol -/+ back to the launcher", 0xff808080u);
}

/* ------------------------------------------------------------------ input */

static unsigned buttons(void)
{
    unsigned b = 0;
    if (hb_button_pressed(HB_BTN_VOL_DOWN)) b |= 1;
    if (hb_button_pressed(HB_BTN_VOL_UP)) b |= 2;
    if (hb_button_pressed(HB_BTN_HOME)) b |= 4;
    return b;
}

/* Icon hit by a tap released at portrait (px, py); -1 for none. */
static int icon_at(int px, int py)
{
    int x = py, y = 239 - px;
    for (int i = 0; i < 4; ++i) {
        int dx = x - (W / 8 + i * (W / 4)), dy = y - 112;
        if (dx * dx + dy * dy <= 44 * 44) return i;
    }
    return -1;
}

/* ------------------------------------------------------------------- boot */

static void boot_fail(const char *why)
{
    if (kernel) { hb_os_free(kernel); kernel = 0; }
    state = BOOT_ERROR;
    draw_message("NanoOS boot failed", why, NANOOS_KERNEL_PATH, RED);
}

static void boot_read(void)
{
    uint32_t n = hb_fs_size(NANOOS_KERNEL_PATH);
    if (!n) { boot_fail("kernel not found"); return; }
    if (n > KERNEL_MAX_BYTES) { boot_fail("kernel larger than 2 MiB"); return; }
    kernel = hb_os_alloc(n);
    if (!kernel) { boot_fail("no memory for the kernel"); return; }
    kernel_len = hb_fs_read(NANOOS_KERNEL_PATH, kernel, n);
    if (kernel_len != n) { boot_fail("kernel read failed"); return; }
    char msg[40] = "loaded ", *p = put_uint(msg + 7, kernel_len);
    const char *tail = " bytes";
    while ((*p++ = *tail++)) ;
    draw_busy(50, msg, "waiting for the disk to settle");
    settle = 0;
    state = BOOT_SETTLE;
}

static void launch(void)
{
    if (selected == 0) {
        state = STOCK_HINT;
        draw_message("StockOS", "press Home to return", "to the RetailOS Home screen", WHITE);
        return;
    }
    draw_busy(0, "reading kernel.bin", NANOOS_KERNEL_PATH);
    state = BOOT_READ;
}

/* ---------------------------------------------------------------- surface */

void hb_raw_init(int w, int h)
{
    /* Registered as a GL app (HBAppKind 2) the entry gets fb = NULL, and
     * drawing through it would wipe the IRAM vectors at address 0. */
    no_fb = !hb_raw_fb() || w != 240 || h != 432;
    if (no_fb) return;
    state = MENU;
    selected = 0;
    kernel = 0;
    prev_buttons = buttons();      /* a button still held from launch is not a press */
    touch_was_down = 0;
    draw_launcher();
}

void hb_raw_frame(const hb_spoint_t *touch)
{
    if (no_fb) return;
    unsigned now = buttons(), pressed = now & ~prev_buttons;
    prev_buttons = now;
    int tapped = -2;               /* -2: no tap this frame */
    if (touch->down) {
        touch_was_down = 1;
        touch_x = touch->x;
        touch_y = touch->y;
    } else if (touch_was_down) {
        touch_was_down = 0;
        tapped = icon_at(touch_x, touch_y);
    }

    switch (state) {
    case MENU: {
        int old = selected, go = 0;
        if (pressed & 1) selected = (selected + 3) & 3;
        if (pressed & 2) selected = (selected + 1) & 3;
        if (tapped >= 0) {
            go = tapped == selected;
            selected = tapped;
        }
        /* Home on StockOS is left to RetailOS: that is its return-home path */
        if ((pressed & 4) && selected != 0) go = 1;
        if (selected != old) draw_launcher();
        if (go) launch();
        break;
    }
    case STOCK_HINT:
    case BOOT_ERROR:
        if (pressed & 3 || tapped != -2) { state = MENU; draw_launcher(); }
        break;
    case BOOT_READ:
        boot_read();
        break;
    case BOOT_SETTLE:
        if (++settle < SETTLE_FRAMES) {
            draw_busy(50 + 40 * settle / SETTLE_FRAMES, "kernel loaded", "waiting for the disk to settle");
        } else {
            draw_busy(100, "handing off to NanoOS", "hardware shutdown, MMU off");
            state = BOOT_HANDOFF;
        }
        break;
    case BOOT_HANDOFF:
        /* the frame above is on screen now; shutdown freezes it there */
        hw_shutdown_all();
        handoff_to_payload(kernel, kernel_len);
    }
}
