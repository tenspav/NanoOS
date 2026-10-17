#pragma GCC optimize("Os")   /* status pages, no hot path: kernel.bin must fit 64 KB */
#include "drivers.h"
#include "lcd.h"
#include "fmt.h"
#include "mem.h"
#include "spi.h"
#include "touch.h"
#include "buttons.h"
#include "audio.h"
#include "nand.h"
#include "usb.h"
#include "bt.h"
#include "mbx.h"
/* Each line: NAME STATUS details. STATUS: OK, -- (running), ERR, STUB.
 * The font has no J Q Y Z j q = or comma (test_nanoemu.py OCRs this). */
#define X 8
#define Y0 30
#define DY 14   /* 13 lines incl. MBX diag must clear the footer at 226 */
#define SENTINEL 0x5a5a5a5au
static uint32_t *nand_buf;
static int nand_ok, nand_rd;
static uint32_t nand_meta[4];
static int mbx_ok, mbx_rc;
static void status(const char *name, const char *st) {
    fmt_str(name); fmt_str(st); fmt_str(" ");
}
void drivers_enter(void) {
    /* NAND first: the probe sees the handoff state before the Home/touch
     * capture windows (6 s of I2C, SPI and GPIO work) and long before the
     * audio DMA (round-6 device regression, nand.c) */
    if (!nand_buf) nand_buf = kalloc(NAND_SECTOR);
    nand_rd = -1;
    if (nand_buf) nand_ok = nand_selftest(nand_buf, &nand_rd, nand_meta);  /* + R81 */
    buttons_init();
    touch_init();
    usb_probe();
    mbx_ok = mbx_init();
    mbx_rc = mbx_ok ? mbx_empty_scene() : -1;
    bt_start();
    audio_init();   /* codec probe, 2 s test.pcm, R82; last: audio_service() follows */
}
void drivers_service(void) {
    touch_poll();
    buttons_poll();
    bt_service();
    audio_service();    /* keeps the DMA fed; ends by itself if not called */
}
static void keys(unsigned b) {
    static const char names[] = "HPL+-";
    char s[2] = {0, 0};
    for (unsigned i = 0; i < 5; ++i) { s[0] = b & (1u << i) ? names[i] : '.'; fmt_str(s); }
}
void drivers_draw(void) {
    int y = Y0;
    lcd_clear(0xff1a1a1a);
    /* last touch, under the text (the lines are OCR-checked) */
    if (touch.reports) lcd_circle(touch.x, touch.y, 3, 0xff00c8ff);
    lcd_center(16, "NanoOS Drivers", WHITE);

    status("LCD   ", lcd_stats.frames ? "OK" : "--");
    fmt_str("fr "); fmt_dec(lcd_stats.frames); fmt_str(" to "); fmt_dec(lcd_stats.timeouts);
    fmt_str(" hd "); fmt_dec(lcd_stats.held); fmt_str(" rc "); fmt_dec(lcd_stats.recovered);
    fmt_line(X, y, WHITE); y += DY;

    status("TOUCH ", touch.errors ? "ERR" : "OK");
    fmt_str("n "); fmt_dec(touch.reports);
    if (touch.reports) {
        fmt_str(" x "); fmt_dec((uint32_t)touch.x); fmt_str(" y "); fmt_dec((uint32_t)touch.y);
        fmt_str(touch.state == TOUCH_DOWN ? " down" : touch.state == TOUCH_MOVE ? " move" : " up");
    }
    if (touch.errors) { fmt_str(" er "); fmt_dec(touch.errors); }
    fmt_line(X, y, WHITE); y += DY;

    status("BTN   ", "OK");
    fmt_str("now "); keys(buttons.now); fmt_str(" seen "); keys(buttons.seen);
    fmt_str(" "); fmt_sdec(buttons.temp_c); fmt_str("C");
    fmt_line(X, y, WHITE); y += DY;

    /* codec reg 0x2f (FF = no answer; the other regs are in R82), w =
     * play-mode codec writes; then source frames queued and underruns of
     * the DMA stream (audio.h) */
    int c2f = audio.id[AUDIO_NREG - 1];
    status("AUDIO ", c2f < 0 || c2f == 0xff ? "ERR" : "OK");
    fmt_str("codec 2F:"); fmt_hex((uint32_t)c2f, 2);
    fmt_str(" w"); fmt_dec(audio.w);
    if (audio.passes) {
        fmt_str(" pcm "); fmt_dec(audio.frames);
        fmt_str(" u"); fmt_dec(audio.under);
    }
    fmt_line(X, y, WHITE); y += DY;

    status("NAND  ", nand_ok && !nand_rd ? "OK" : "ERR");
    if (nand_ok) {
        fmt_str("PPN "); fmt_dec(nand.page_size); fmt_str("B ");
        fmt_dec(nand.pages); fmt_str("pg rd p0 ");
        fmt_str(nand_rd ? "fail" : nand_buf[0] == SENTINEL ? "no dma" :
                nand_meta[0] == 0xffffffffu ? "blank" : "data");
        fmt_str(" t"); fmt_dec(nand.attempt);   /* probe attempt that answered */
    } else {    /* id, FMC +0x00 and +0x48 as found, failed waits (R81 has st) */
        fmt_str("id "); fmt_hex(nand.id, 8); fmt_str(" cs "); fmt_hex(nand.cs, 8);
        fmt_str(" 48 "); fmt_hex(nand.st48, 8); fmt_str(" er "); fmt_dec(nand.errors);
    }
    fmt_line(X, y, WHITE); y += DY;

    status("USB   ", usb.core ? "OK" : "STUB");
    fmt_str("id "); fmt_hex(usb.snpsid, 8); fmt_str(" vbus ");
    if (usb.vbus < 0) fmt_str("-"); else fmt_dec((uint32_t)usb.vbus);
    if (!usb.core) fmt_str(" no core");
    fmt_line(X, y, WHITE); y += DY;

    bt_line();
    fmt_line(X, y, WHITE); y += DY;

    status("MBX   ", mbx_ok && !mbx_rc ? "OK" : "ERR");
    fmt_str("id "); fmt_hex(mbx.id, 8); fmt_str(" scene ");
    fmt_dec(mbx.scenes); fmt_str(" in "); fmt_dec(mbx.done_us); fmt_str(" us");
    fmt_line(X, y, WHITE); y += DY;
    if (!(mbx_ok && !mbx_rc)) {
        fmt_str("  rev "); fmt_hex(mbx.rev, 8); fmt_str(" sig "); fmt_hex(mbx.sig, 8);
        fmt_str(" st "); fmt_hex(mbx.status, 8); fmt_str(" to "); fmt_dec(mbx.timeouts);
        fmt_line(X, y, WHITE); y += DY;
        /* 4 values per line (52 of 53 columns); b68/b58 = before the ungate.
         * Power state controller as before>after the domain wakes, and the
         * wake result (see mbx.c pd_try). p0/p4/p8, ck+0/4/8, c1c and c44
         * are still captured in mbx_diag but not shown: no room. */
        fmt_str(" c68 "); fmt_hex(mbx_diag[0], 8); fmt_str(" b68 "); fmt_hex(mbx_diag[11], 8);
        fmt_str(" c58 "); fmt_hex(mbx_diag[9], 8); fmt_str(" b58 "); fmt_hex(mbx_diag[12], 8);
        fmt_line(X, y, WHITE); y += DY;
        fmt_str(" 14 "); fmt_hex(mbx_diag[15], 8); fmt_str(">"); fmt_hex(mbx_diag[21], 8);
        fmt_str(" 100 "); fmt_hex(mbx_diag[18], 8); fmt_str(">"); fmt_hex(mbx_diag[24], 8);
        fmt_line(X, y, WHITE); y += DY;
        fmt_str(" pd "); fmt_hex(mbx_diag[25], 4);
        fmt_str(" 1c "); fmt_hex(mbx_diag[17], 8); fmt_str(">"); fmt_hex(mbx_diag[23], 8);
        fmt_str(" 18 "); fmt_hex(mbx_diag[16], 8); fmt_str(">"); fmt_hex(mbx_diag[22], 8);
        fmt_line(X, y, WHITE);
    }

    lcd_center(226, "Vol-: stats  Home: back", WHITE);
    lcd_present();
}
