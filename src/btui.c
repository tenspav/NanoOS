#pragma GCC optimize("Os")   /* settings page, no hot path: kernel.bin must fit 64 KB */
#include "btui.h"
#include "bt.h"
#include "lcd.h"
#include "fmt.h"
#include "input.h"
#include "touch.h"
/* Settings > Bluetooth (launcher scene 3; bt.h's manual mode), round 11:
 * one state line, one action, the devices by name (Portuguese, ASCII).
 *   y 12      title (the strip above 34: tap = back)
 *   y 40..63  the state's action (Ligar / Procurar / Desconectar; none
 *             while starting or connecting) and, while on, Desligar
 *   y 72      the state: Desligado; Ligando... <step>; Procurando... N;
 *             Ligado - N encontrados; Conectando: <name> <phase>;
 *             Falhou: <phase> <code> (+ Tentando 2/3 while bt.c's retry
 *             waits); Conectado: <name>
 *   y 86      grey: the step and the last failure (step, code) as before
 *   y 98..217 found devices, 5 rows of 24 px: the name (else the address,
 *             bt.c); the list scrolls with the focus, the focused row says
 *             what Home / a tap does (Conectar, Desconectar)
 * Touch: tap the action, or a device (= Conectar). Buttons: Vol-/Vol+
 * move the focus (0 action, 1 Desligar, then the devices), Home acts on
 * it, Vol- + Vol+ goes back. While starting only back works; while
 * connecting no other connect is taken. */
#define TOP 34          /* main.c TOP_H */
#define ACT_Y 40
#define OFF_X 296       /* Desligar: x 296..427 */
#define LIST_Y 98
#define ROW 24
#define ROWS 5u
#define BLUE 0xff2684ffu
#define GREY 0xff404040u
enum { S_OFF, S_FATAL, S_START, S_SCAN, S_READY, S_FAILED, S_RETRY, S_CONN, S_UP };
static unsigned focus;
/* round 10b: what the screen shows (focus, devices, step, phase, failure,
 * retry; HCI events: names, inquiry results), so a frame is drawn only
 * when it changed; btui_enter: none. Pad reports are not shown (round 11):
 * they no longer redraw. */
static uint32_t shown[3];
static unsigned state(void) {
    unsigned s = bt.step;
    if (s == BT_OFF) return S_OFF;
    if (s == BT_ERR) return S_FATAL;
    if (s < BT_INQ) return S_START;
    if (s == BT_INQ) return S_SCAN;
    if (s == BT_HID) return S_UP;
    if (s < BT_HID) return S_CONN;
    return bt.waiting ? S_RETRY : bt.fphase ? S_FAILED : S_READY;
}
/* the first visible device row */
static unsigned top(void) { return focus >= 2 + ROWS ? focus - 1 - ROWS : 0; }
static void box(int x, int y, int w, uint32_t c) {
    for (int j = 0; j < 22; ++j)
        for (int i = 0; i < w; ++i) lcd_pixel(x + i, y + j, c);
}
static const char *action(unsigned st) {
    return st <= S_FATAL ? "Ligar" : st == S_UP ? "Desconectar" :
           st >= S_READY && st <= S_RETRY ? "Procurar" : "";
}
/* "Falhou: <phase> <code>": the phase (the step before any phase), then
 * timeout, L2CAP xx (bt.c 0x1xx), HCI xx (a link step), else the raw code
 * (bring-up: patch FB, pmu E1..E3) */
static void failure(void) {
    unsigned c = bt.status, f = bt.failed;
    fmt_str("Falhou: ");
    fmt_str(bt.fphase & 0x7f ? bt_phase_name(bt.fphase & 0x7f) : bt_step_name(f));
    if (c == 0xff) { fmt_str(" timeout"); return; }
    fmt_str(c >> 8 ? " L2CAP " : f >= BT_INQ && f <= BT_HID ? " HCI " : " ");
    fmt_hex(c & 0xff, 2);
}
static void act(unsigned f) {
    unsigned st = state();
    if (st == S_START) return;
    if (f == 1) { if (st != S_OFF) bt_power(0); return; }
    if (!f) {
        if (st <= S_FATAL) bt_power(1);
        else if (st == S_UP) bt_disconnect();
        else if (*action(st)) bt_scan();
        return;
    }
    if (f - 2 >= bt_ndev) return;
    const uint8_t *a = bt_devs[f - 2].addr;
    if (st == S_UP) {
        unsigned k = 0;
        while (k < 6 && a[k] == bt.peer[k]) ++k;
        if (k == 6) bt_disconnect();
    } else if (st == S_SCAN || (st >= S_READY && st <= S_RETRY)) bt_connect(a);
}
static void draw(void) {
    unsigned st = state(), t = top();
    const char *a = action(st);
    lcd_clear(0xff1a1a1au);
    lcd_center(12, "Bluetooth", WHITE);
    if (*a) {
        box(4, ACT_Y, OFF_X - 12, focus == 0 ? BLUE : GREY);
        lcd_text(14, ACT_Y + 7, a, WHITE);
    }
    if (st != S_OFF) {
        box(OFF_X, ACT_Y, LCD_WIDTH - 4 - OFF_X, focus == 1 ? BLUE : GREY);
        lcd_text(OFF_X + 10, ACT_Y + 7, "Desligar", WHITE);
    }
    if (st == S_OFF) fmt_str("Desligado");
    else if (st == S_START) {
        fmt_str("Ligando... ");
        fmt_str(bt.phase ? bt_phase_name(bt.phase) : bt_step_name(bt.step));
    } else if (st == S_SCAN) { fmt_str("Procurando... "); fmt_dec(bt_ndev); }
    else if (st == S_READY) { fmt_str("Ligado - "); fmt_dec(bt_ndev); fmt_str(" encontrados"); }
    else if (st == S_CONN || st == S_UP) {
        fmt_str(st == S_UP ? "Conectado: " : "Conectando: "); fmt_str(bt_name(bt.peer));
        if (st == S_CONN) { fmt_str(" "); fmt_str(bt_phase_name(bt.phase)); }
    } else {
        failure();
        if (st == S_RETRY) { fmt_str("  Tentando "); fmt_dec(bt.again + 1u); fmt_str("/3"); }
    }
    fmt_line(8, 72, st == S_FAILED || st == S_RETRY || st == S_FATAL ? 0xffff8060u : 0xff60d0ffu);
    fmt_str(bt_step_name(bt.step));             /* the old step + last line */
    if (bt.status) { fmt_str("  last "); fmt_str(bt_step_name(bt.failed)); fmt_str(" "); fmt_hex(bt.status, 2); }
    fmt_line(8, 86, 0xffa0a0a0u);
    for (unsigned r = 0; r < ROWS && t + r < bt_ndev; ++r) {
        unsigned i = t + r;
        int y = LIST_Y + (int)(r * ROW);
        if (focus == i + 2) {
            box(4, y, LCD_WIDTH - 8, BLUE);
            const char *ra = st == S_UP ? "Desconectar" :
                             st == S_SCAN || (st >= S_READY && st <= S_RETRY) ? "Conectar" : "";
            unsigned n = 0;
            while (ra[n]) ++n;
            lcd_text(LCD_WIDTH - 12 - (int)n * 8, y + 7, ra, WHITE);
        }
        lcd_text(12, y + 7, bt_devs[i].name, WHITE);
    }
    lcd_center(228, "Vol: mover  Home: ok  Vol-+Vol+: voltar", 0xffa0a0a0u);
    lcd_present();
}
void btui_enter(void) { focus = 0; shown[0] = ~0u; }
unsigned btui_tick(unsigned keys, unsigned tap) {
    unsigned n = 2 + bt_ndev, y = (unsigned)touch.y;
    bt_service();
    if ((tap && y < TOP) || (keys & BUTTON_CHORD)) return BUTTON_HOME;
    if (focus >= n || (focus == 1 && bt.step == BT_OFF)) focus = 0;   /* list cleared / no Desligar */
    if (state() != S_START) {
        if (keys & BUTTON_LEFT) focus = (focus + n - 1) % n;
        if (keys & BUTTON_RIGHT) focus = (focus + 1) % n;
        if (keys & BUTTON_HOME) act(focus);
        if (tap && y >= ACT_Y && y < ACT_Y + 24) act(focus = (unsigned)touch.x >= OFF_X);
        else if (tap && y >= LIST_Y && y < LIST_Y + ROWS * ROW) {   /* a device (draw()'s view) */
            unsigned i = top() + (y - LIST_Y) / ROW;
            if (i < bt_ndev) act(focus = i + 2);
        }
    }
    uint32_t now[3] = {focus | bt_ndev << 8 | bt.step << 16 | (uint32_t)bt.phase << 24,
                       bt.status | bt.failed << 12 | (uint32_t)bt.fphase << 16 |
                       (uint32_t)bt.again << 24 | (uint32_t)bt.waiting << 28, bt.events};
    if (lcd_ready() && (now[0] != shown[0] || now[1] != shown[1] || now[2] != shown[2])) {
        shown[0] = now[0]; shown[1] = now[1]; shown[2] = now[2];
        draw();
    }
    return 0;
}
