/*
 * loader.c -- NanoOS Loader: a RetailOS raw-surface app that boots the
 * bare-metal NanoOS kernel the moment it is opened.
 *
 * There is no menu here: NanoOS has its own launcher (clock profile
 * selector, StockOS / ViceNano / PS1Emu / Settings / DevMode). The v0.0.0
 * loader had a copy of that launcher and started the boot on Home, but
 * RetailOS acts on the same Home press and tears the app down; on the real
 * iPod that teardown won the race, so NanoOS never ran. Booting on open keeps
 * Home out of the path. Home during the boot still takes RetailOS home (the
 * way out), and NanoOS's StockOS icon resets back to RetailOS.
 *
 * The boot runs over several frames so the screen always shows where it is
 * (CLAUDE.md "BUSY..." rule):
 *   1. BUSY drawn in hb_raw_init(), then START_FRAMES frames for RetailOS to
 *      finish opening the app (a handoff during its launch transition made
 *      RetailOS reset itself in NanoEmu); then, if NanoOS's GPU test left a
 *      valid RAM report (nanoos/src/report.h), one file per frame goes to
 *      REPORT_DIR and the magic is cleared
 *   2. read NANOOS_KERNEL_PATH whole into an OS-heap staging buffer
 *   3. let the filesystem settle a few frames (file closed, FMC idle)
 *   4. probe/chainloader: hw_shutdown_all(), then handoff_to_payload(), which
 *      escalates through the SVC hook, turns the MMU and caches off, copies the
 *      kernel to 0x08000000 and jumps there. The display stops being refreshed
 *      by shutdown, so the "handing off" frame stays up until NanoOS draws.
 * A failure (no kernel, ...) shows an error until Home takes the app home.
 *
 * Frames are 432x240 landscape, rotated into the 240x432 portrait
 * framebuffer exactly like nanoos/src/lcd.c, with the same font.
 */
#include <stdint.h>
#include "hb_sdk.h"
#include "hb_heap.h"
#include "hb_raw_surface.h"
#include "shutdown.h"
#include "handoff.h"
#include "version.h"
#include "report.h"

#ifndef NANOOS_KERNEL_PATH
#define NANOOS_KERNEL_PATH "/Apps/NanoOS/kernel.bin"
#endif
#define REPORT_DIR "/Apps/NanoOS/reports"
/* nanoos/kernel.ld: code + data + BSS + stack must stay below 0x08200000 */
#define KERNEL_MAX_BYTES (2u * 1024u * 1024u)
/* Frames between the kernel read and the shutdown: the file is closed by then
 * and hw_shutdown_all() also waits out an in-flight FMC operation. */
#define SETTLE_FRAMES 4
/* Frames between the app opening and the kernel read (~1 s). */
#define START_FRAMES 60
/* Frames the "handing off" screen is up before the shutdown: RetailOS pushes
 * a drawn frame to the panel on a later TE pulse, so without them the glass
 * froze on the previous (80%) frame. */
#define HANDOFF_FRAMES 3

#define W 432
#define H 240
#define BG 0xff1a1a1au
#define WHITE 0xffffffffu
#define RED 0xffff5050u
#define GREEN 0xff28b848u

extern const uint8_t font8x8[128][8];        /* nanoos/src/font.c, bit 0 left */
extern const uint8_t loader_font8x8[96][8];  /* font_full.c, bit 7 left */

enum { BOOT_START, BOOT_EXPORT, BOOT_READ, BOOT_SETTLE, BOOT_HANDOFF, BOOT_ERROR };
static int state, frames, no_fb;
static uint8_t *kernel;
static uint32_t kernel_len;
static uint32_t export_i, export_n, export_bad;
/* Heap copies of the report slots, taken before any filesystem call.
 * Round 14: the corrupt tails (R34, R43, R53, R80) were never RAM. Each one
 * is the LAST file of its export batch, and its bytes from a sector-aligned
 * offset on (R80: 0x800) are stale disk data (R80's equal gata_only.mp3
 * at 0x74800 byte for byte). hb_fs_write's sync leaves the final sectors
 * in a lower write-back cache, which the next file write commits; the
 * shutdown/handoff dropped it. So the batch now ends with flush_disk(). */
static uint8_t *rpt_copy;
static uint32_t rpt_off[RPT_SLOTS], rpt_ok[RPT_SLOTS];
/* Round 13: the header (id, len) is snapshotted with the text. export_step
 * used to re-read len from 0x0ae10000 AFTER hb_fs_write had started reusing
 * that RetailOS block, so a grown len wrote OS-heap bytes past the copy:
 * the "...container_seed CREATE TABLE..." tail of R80.txt. */
static uint32_t rpt_id[RPT_SLOTS], rpt_len[RPT_SLOTS];
static char export_msg[40];

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

static void draw_busy(int percent, const char *line1, const char *line2)
{
    clear(BG);
    center(16, NANOOS_TITLE, WHITE);
    center(40, "BUSY...", WHITE);
    center(64, "Booting NanoOS", WHITE);
    center(100, line1, WHITE);
    if (line2) center(116, line2, WHITE);
    rect(56, 150, 320, 16, 0xff404040u);
    rect(56, 150, 320 * percent / 100, 16, GREEN);
    char pct[8], *e = put_uint(pct, (uint32_t)percent);
    e[0] = '%';
    e[1] = 0;
    center(180, pct, WHITE);
    if (export_msg[0]) center(204, export_msg, GREEN);
}

static void draw_message(const char *title, const char *line1, const char *line2, uint32_t c)
{
    clear(BG);
    center(80, title, c);
    center(116, line1, WHITE);
    center(132, line2, WHITE);
    center(220, "press Home to return to the iPod", 0xff808080u);
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
    frames = 0;
    state = BOOT_SETTLE;
}

/* RAM report export: write slot export_i - 1 (if it holds a valid case),
 * then show the next one so the screen always names the file in flight */
static void report_path(char *path, uint32_t id)
{
    const char *s = REPORT_DIR "/R";
    while ((*path++ = *s++)) ;
    path = put_uint(path - 1, id);
    s = ".txt";
    while ((*path++ = *s++)) ;
}

static int slot_valid(uint32_t i)
{
    return rpt_id[i] && rpt_len[i] && rpt_len[i] <= RPT_SLOT_SIZE;
}

/* bytes up to and including the last "== END R" line (a safety net: the
 * writers always end with it), or len when there is none */
static uint32_t text_end(const uint8_t *t, uint32_t len)
{
    static const char tag[] = "== END R";
    for (uint32_t k = len; k-- > 0;) {
        uint32_t j = 0;
        while (tag[j] && k + j < len && t[k + j] == (uint8_t)tag[j]) ++j;
        if (tag[j]) continue;
        while (k < len && t[k] != '\n') ++k;
        return k < len ? k + 1 : len;
    }
    return len;
}

/* remove first: the file is never left with an older, longer tail */
static int write_report(const char *path, const uint8_t *t, uint32_t len)
{
    hb_fs_remove(path);
    return hb_fs_write(path, t, text_end(t, len));
}

/* Round 14: commit the last report. A sacrificial FLUSH_BYTES file (zeros)
 * after it pushes the report's cached tail sectors to the disk, the way
 * every report followed by another one always came out intact (R30-R33,
 * R40-R42, R50-R52, about 130 KB each). Its own tail may stay stale, which
 * is harmless. */
#define FLUSH_PATH REPORT_DIR "/flush.bin"
#define FLUSH_CHUNK 0x4000u
#define FLUSH_PARTS 16
static void flush_disk(void)
{
    draw_busy(8, "committing reports to disk", FLUSH_PATH);
    uint8_t *z = hb_os_alloc(FLUSH_CHUNK);
    if (!z) return;
    for (uint32_t k = 0; k < FLUSH_CHUNK; ++k) z[k] = 0;
    void *parts[FLUSH_PARTS];
    uint32_t lens[FLUSH_PARTS];
    for (int k = 0; k < FLUSH_PARTS; ++k) { parts[k] = z; lens[k] = FLUSH_CHUNK; }
    hb_fs_write_parts(FLUSH_PATH, parts, lens, FLUSH_PARTS);
    hb_os_free(z);
}

/* copy every valid slot to the heap and check its sum there */
static void export_snapshot(void)
{
    volatile struct rpt_hdr *h = RPT_HDR;
    uint32_t total = 0;
    for (uint32_t i = 0; i < RPT_SLOTS; ++i) {
        rpt_id[i] = h->id[i];
        rpt_len[i] = h->len[i];
        rpt_off[i] = total;
        rpt_ok[i] = 0;
        if (slot_valid(i)) total += (rpt_len[i] + 3u) & ~3u;
    }
    rpt_copy = total ? hb_os_alloc(total) : 0;
    if (!rpt_copy) return;
    for (uint32_t i = 0; i < RPT_SLOTS; ++i) {
        if (!slot_valid(i)) continue;
        const volatile uint8_t *src = (const volatile uint8_t *)RPT_SLOT(i);
        uint8_t *dst = rpt_copy + rpt_off[i];
        uint32_t len = rpt_len[i];
        for (uint32_t k = 0; k < len; ++k) dst[k] = src[k];
        rpt_ok[i] = rpt_sum(dst, len) == h->sum[i];
    }
}

static void export_step(void)
{
    volatile struct rpt_hdr *h = RPT_HDR;
    char path[40], msg[40] = "exporting report ", *p;
    if (export_i) {
        uint32_t i = export_i - 1;
        if (slot_valid(i)) {
            report_path(path, rpt_id[i]);
            if (rpt_ok[i] && write_report(path, rpt_copy + rpt_off[i], rpt_len[i])) ++export_n;
            else ++export_bad;
        }
    }
    while (export_i < RPT_SLOTS && !rpt_id[export_i]) ++export_i;
    if (export_i == RPT_SLOTS) {
        if (rpt_copy) { hb_os_free(rpt_copy); rpt_copy = 0; }
        if (export_n) flush_disk();
        /* a failed write keeps the magic, so the next launch tries again */
        if (!export_bad) { h->magic[0] = h->magic[1] = 0; rpt_mirror_clear(); }
        p = put_uint(export_msg + 0, export_n);
        const char *t = export_bad ? " reports exported, some failed" : " reports exported";
        while ((*p++ = *t++)) ;
        draw_busy(10, "reading kernel.bin", NANOOS_KERNEL_PATH);
        state = BOOT_READ;
        return;
    }
    report_path(path, rpt_id[export_i]);
    p = put_uint(msg + 17, export_i + 1);
    *p++ = '/';
    put_uint(p, RPT_SLOTS);
    draw_busy(5, msg, path);
    ++export_i;
}

/* Round 12: the main mailbox is gone but a mirror (report.h) survived:
 * copy its text to the OS heap, sum-check it there and write R80.txt from
 * the copy (the same rules as export_snapshot/export_step). */
static void export_mirror(uint32_t base)
{
    volatile struct rpt_hdr *m = (volatile struct rpt_hdr *)base;
    uint32_t len = m->len[RPT_M8_SLOT], ok = 0;
    char path[40], *p;
    uint8_t *copy = hb_os_alloc(len);
    if (copy) {
        const volatile uint8_t *src = (const volatile uint8_t *)(base + RPT_DATA);
        for (uint32_t k = 0; k < len; ++k) copy[k] = src[k];
        if (rpt_sum(copy, len) == m->sum[RPT_M8_SLOT]) {
            hb_fs_mkdir(REPORT_DIR);
            report_path(path, RPT_M8_ID);
            ok = write_report(path, copy, len) != 0;
        }
        hb_os_free(copy);
    }
    if (ok) { flush_disk(); rpt_mirror_clear(); }
    p = put_uint(export_msg, ok);
    const char *t = ok ? " report exported (mirror)" : " reports exported, mirror failed";
    while ((*p++ = *t++)) ;
}

/* ---------------------------------------------------------------- surface */

void hb_raw_init(int w, int h)
{
    /* Registered as a GL app (HBAppKind 2) the entry gets fb = NULL, and
     * drawing through it would wipe the IRAM vectors at address 0. */
    no_fb = !hb_raw_fb() || w != 240 || h != 432;
    if (no_fb) return;
    kernel = 0;
    rpt_copy = 0;
    export_msg[0] = 0;
    export_n = export_bad = 0;
    draw_busy(0, "starting", NANOOS_KERNEL_PATH);
    frames = 0;
    state = BOOT_START;
}

void hb_raw_frame(const hb_spoint_t *touch)
{
    (void)touch;
    if (no_fb) return;
    switch (state) {
    case BOOT_START:
        if (++frames < START_FRAMES) break;
        if (rpt_valid()) {
            export_snapshot();
            hb_fs_mkdir(REPORT_DIR);
            export_i = 0;
            state = BOOT_EXPORT;
            export_step();
        } else {
            uint32_t mirror = rpt_mirror_find();
            if (mirror) {
                draw_busy(5, "exporting report (mirror)", REPORT_DIR "/R80.txt");
                export_mirror(mirror);
            }
            draw_busy(10, "reading kernel.bin", NANOOS_KERNEL_PATH);
            state = BOOT_READ;
        }
        break;
    case BOOT_EXPORT:
        export_step();
        break;
    case BOOT_READ:
        boot_read();
        break;
    case BOOT_SETTLE:
        if (++frames < SETTLE_FRAMES) {
            draw_busy(50 + 40 * frames / SETTLE_FRAMES, "kernel loaded", "waiting for the disk to settle");
        } else {
            draw_busy(100, "handing off to NanoOS", "hardware shutdown, MMU off");
            frames = 0;
            state = BOOT_HANDOFF;
        }
        break;
    case BOOT_HANDOFF:
        /* the frame above is on screen now; shutdown freezes it there */
        if (++frames < HANDOFF_FRAMES) break;
        hw_shutdown_all();
        handoff_to_payload(kernel, kernel_len);
        break;
    case BOOT_ERROR:
        break;
    }
}
