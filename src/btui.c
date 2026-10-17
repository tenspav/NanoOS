#pragma GCC optimize("Os")   /* settings page, no hot path: kernel.bin must fit 64 KB */
#include "btui.h"
#include "bt.h"
#include "lcd.h"
#include "fmt.h"
#include "input.h"
#include "touch.h"
/* Settings > Bluetooth Central (launcher scene 3; bt.h's manual mode).
 *   y 12      title (the strip above 34: tap = back)
 *   y 40..66  four buttons: Power, Scan, Connect, Drop (108 px each)
 *   y 76      state (bt_step_name: off, reset .. ssp, scanning + devices
 *             found, ready, conn .. intr, connected + mac + name, ERR)
 *             and the last failure (step, HCI status, FF = timeout)
 *   y 94..206 found devices, 8 rows of 14 px: mac, class of device, name;
 *             the list scrolls with the focus
 *   y 212     the pad while connected: sticks, buttons (bt.h PAD_*), reports
 * Touch: tap a button; tap a device to select it, again to connect.
 * Buttons: Vol-/Vol+ move the focus over the 4 buttons then the devices,
 * Home acts on it (a device: connect), Vol- + Vol+ goes back. */
#define TOP 34          /* main.c TOP_H */
#define BTN_Y 40
#define BTN_W 108
#define LIST_Y 94
#define ROW 14
#define ROWS 8u
#define BLUE 0xff2684ffu
static const char *const labels[4] = {"Power", "Scan", "Connect", "Drop"};
static unsigned focus, sel;
static void box(int x, int y, int w, uint32_t c) {
    for (int j = 0; j < 26; ++j)
        for (int i = 0; i < w; ++i) lcd_pixel(x + i, y + j, c);
}
static void mac(const uint8_t *a) {
    for (unsigned i = 6; i--;) { fmt_hex(a[i], 2); if (i) fmt_str(":"); }
}
static void act(unsigned f) {
    if (f >= 4) sel = f - 4, f = 2;             /* a device: connect it */
    if (!f) bt_power(bt.step == BT_OFF || bt.step == BT_ERR);
    else if (f == 1) bt_scan();
    else if (f == 3) bt_disconnect();
    else if (sel < bt_ndev) bt_connect(bt_devs[sel].addr);
}
static void draw(void) {
    unsigned s = bt.step, top = focus >= 4 + ROWS ? focus - 3 - ROWS : 0;
    lcd_clear(0xff1a1a1au);
    lcd_center(12, "Bluetooth Central", WHITE);
    for (unsigned i = 0; i < 4; ++i) {
        box((int)(i * BTN_W) + 4, BTN_Y, BTN_W - 8, focus == i ? BLUE : 0xff404040u);
        lcd_text((int)(i * BTN_W) + 14, BTN_Y + 9, labels[i], WHITE);
    }
    fmt_str(s == BT_INQ ? "scanning" : s == BT_HID ? "connected" : bt_step_name(s));   /* no q glyph */
    if (s == BT_INQ) { fmt_str(" found "); fmt_dec(bt_ndev); }
    if (s == BT_HID) {
        fmt_str(" "); mac(bt.peer);
        if (sel < bt_ndev) { fmt_str(" "); fmt_str(bt_devs[sel].name); }
    }
    if (bt.status) { fmt_str("  last "); fmt_str(bt_step_name(bt.failed)); fmt_str(" "); fmt_hex(bt.status, 2); }
    fmt_line(8, 76, 0xff60d0ffu);
    for (unsigned r = 0; r < ROWS && top + r < bt_ndev; ++r) {
        unsigned i = top + r;
        int y = LIST_Y + (int)(r * ROW);
        if (i == sel || focus == i + 4) box(4, y - 3, LCD_WIDTH - 8, focus == i + 4 ? BLUE : 0xff303050u);
        mac(bt_devs[i].addr); fmt_str(" ");
        for (unsigned k = 3; k--;) fmt_hex(bt_devs[i].cod[k], 2);
        fmt_str(" "); fmt_str(bt_devs[i].name);
        fmt_line(8, y, WHITE);
    }
    if (s == BT_HID) {                          /* the pad, live */
        fmt_str("pad x "); fmt_sdec(bt.pad.lx); fmt_str(" y "); fmt_sdec(bt.pad.ly);
        fmt_str(" btn "); fmt_hex(bt.pad.buttons, 5); fmt_str(" rep "); fmt_dec(bt.reports);
        fmt_line(8, 212, 0xff60ff80u);
    }
    lcd_center(228, "Vol move  Home do  Vol-+Vol+ back", 0xffa0a0a0u);
    lcd_present();
}
void btui_enter(void) { focus = 0; }
unsigned btui_tick(unsigned keys, unsigned tap) {
    unsigned n = 4 + bt_ndev, y = (unsigned)touch.y;
    bt_service();
    if ((tap && y < TOP) || (keys & BUTTON_CHORD)) return BUTTON_HOME;
    if (focus >= n) focus = 0;                  /* the list was cleared */
    if (keys & BUTTON_LEFT) focus = (focus + n - 1) % n;
    if (keys & BUTTON_RIGHT) focus = (focus + 1) % n;
    if (keys & BUTTON_HOME) act(focus);
    if (tap && y >= BTN_Y && y < BTN_Y + 26) act(focus = (unsigned)touch.x / BTN_W);
    else if (tap && y >= LIST_Y - 3) {         /* a device row (the view of draw()) */
        unsigned i = (focus >= 4 + ROWS ? focus - 3 - ROWS : 0) + (y - LIST_Y + 3) / ROW;
        if (i < bt_ndev) {
            if (i == sel) act(i + 4);
            sel = i;
            focus = i + 4;
        }
    }
    if (lcd_ready()) draw();
    return 0;
}
