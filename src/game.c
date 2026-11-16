#include "game.h"
#include "gameabi.h"
#include "memmap.h"
#include "mmu.h"
#include "asset.h"
#include "lcd.h"
#include "input.h"
#include "touch.h"
#include "timer.h"
#include "mbx.h"
#include "nand.h"
#include "../runtime/storage/mount.h"
#include "../runtime/storage/disk.h"
#include "../runtime/storage/partition.h"
#include "../runtime/storage/hfs.h"
static const struct ngame_hdr *game;
static uint32_t usec(void) { return USEC; }
static void dcache_clean(void) { dcache_clean_all(); }
/* v4 NAND wrappers: read only, passive mode forced, arguments bounded. No
 * write/erase function is reachable from here. */
static int nand_args(unsigned ce, uint32_t page, void *buf, uint32_t meta[4]) {
    if (!nand.pages || !nand.blocks || !nand.caus || nand.pages > 128u || !buf || !meta || ce >= 2u) return -3;
    uint32_t cau = page >> 19, blk = (page >> 7) & 0xfffu, pg = page & 0x7fu;
    if (cau >= nand.caus || blk >= nand.blocks || pg >= nand.pages || (page >> 31)) return -3;
    return 0;
}
/* nand_info is a copy (struct nand_info has cs between errors and st) */
static struct ngame_nand_info ninfo;
static void ninfo_sync(void) {
    ninfo.id = nand.id; ninfo.id2 = nand.id2; ninfo.caus = nand.caus; ninfo.blocks = nand.blocks;
    ninfo.pages = nand.pages; ninfo.page_size = nand.page_size; ninfo.errors = nand.errors; ninfo.st = nand.st;
    ninfo.cs = nand.cs; ninfo.cs0 = nand.cs0; ninfo.st48 = nand.st48; ninfo.rcv = nand.rcv;
    ninfo.probes = nand.probes; ninfo.attempt = nand.attempt; ninfo.init = nand.init; ninfo.ph = nand.ph;
    ninfo.passive = (uint32_t)nand_passive;
    ninfo.ppn_select = nand.ppn_select; ninfo.ppn_start = nand.ppn_start;
    ninfo.ppn_end = nand.ppn_end; ninfo.ppn_bytes = nand.ppn_bytes;
    ninfo.ppn_stage = nand.ppn_stage;
    ninfo.ppn_power1 = nand.ppn_power1;
}
static int g_nand_probe(void) { nand_passive = 1; int r = nand_probe(); ninfo_sync(); return r; }
static int g_nand_read(unsigned ce, uint32_t page, void *buf, uint32_t meta[4]) {
    int r = nand_args(ce, page, buf, meta);
    if (r) return r;
    nand_passive = 1;
    r = nand_read_sector(ce, page, 0, buf, meta);
    ninfo_sync();
    return r;
}
static int g_nand_read_ext(unsigned ce, uint32_t page, void *buf, uint32_t meta[4], uint32_t *extra, uint32_t n) {
    int r = nand_args(ce, page, buf, meta);
    if (r || (n && !extra)) return -3;
    nand_passive = 1;
    r = nand_read_ext(ce, page, buf, meta, extra, n);
    ninfo_sync();
    return r;
}
/* v5 read-only address experiment. Deliberately does NOT decode or bound
 * raw_addr: the point is to pass arbitrary 4-byte page addresses to the
 * driver. Guards are only passive mode, probed geometry, CE < 2 and the two
 * buffers. No write/erase is reachable from here. */
static int g_nand_read_raw(unsigned ce, uint32_t raw_addr, void *buf, uint32_t meta[4]) {
    if (!nand.pages || !nand.blocks || !nand.caus || !buf || !meta || ce >= 2u) return -3;
    nand_passive = 1;
    int r = nand_read_raw(ce, raw_addr, buf, meta);
    ninfo_sync();
    return r;
}
/* v6 read with a recovery pre-action. Same guards and the same deliberately
 * unvalidated raw_addr as v5; the mode is passed through so nand_read_mode()
 * can reject an unknown one with its own code (-4). Passive mode forced. */
static int g_nand_read_mode(unsigned ce, uint32_t raw_addr, void *buf,
                            uint32_t meta[4], unsigned mode) {
    if (!nand.pages || !nand.blocks || !nand.caus || !buf || !meta || ce >= 2u) return -3;
    nand_passive = 1;
    int r = nand_read_mode(ce, raw_addr, buf, meta, mode);
    ninfo_sync();
    return r;
}
/* v6 read-only register snapshot. Only reads FMC registers, so it does not
 * require a successful probe (that is exactly when the dump is wanted);
 * passive mode is still forced and a null output is ignored. */
static void g_nand_regs(uint32_t out[16]) {
    if (!out) return;
    nand_passive = 1;
    nand_regs_snapshot(out);
    ninfo_sync();
}
/* v7 read-only data-phase experiment. Same guards and deliberately
 * unvalidated raw_addr as v5/v6; the variant is passed through so
 * nand_read_phase() can reject an unknown one with its own code (-6).
 * Passive mode forced; no write/erase is reachable. */
static int g_nand_read_phase(unsigned ce, uint32_t raw_addr, void *buf,
                             uint32_t meta[4], unsigned variant) {
    if (!nand.pages || !nand.blocks || !nand.caus || !buf || !meta || ce >= 2u) return -3;
    nand_passive = 1;
    int r = nand_read_phase(ce, raw_addr, buf, meta, variant);
    ninfo_sync();
    return r;
}
/* v8 read-only transfer-shape experiment. Same guards and the same
 * deliberately unvalidated raw_addr as v5/v6/v7; the shape is passed through
 * so nand_read_shape() can reject an unknown one with its own code (-7).
 * Passive mode forced; no write/erase is reachable. */
static int g_nand_read_shape(unsigned ce, uint32_t raw_addr, void *buf,
                             uint32_t meta[4], unsigned shape) {
    if (!nand.pages || !nand.blocks || !nand.caus || !buf || !meta || ce >= 2u) return -3;
    nand_passive = 1;
    int r = nand_read_shape(ce, raw_addr, buf, meta, shape);
    ninfo_sync();
    return r;
}
/* v9 read-only read-ORDERING experiment. Same guards and the same
 * deliberately unvalidated raw_addr as v5..v8; the order is passed through so
 * nand_read_order() can reject an unknown one with its own code (-9).
 * Passive mode forced; no write/erase is reachable. meta may be NULL only for
 * the orders that never read metadata (the driver enforces that). */
static int g_nand_read_order(unsigned ce, uint32_t raw_addr, void *buf,
                             uint32_t meta[4], unsigned order) {
    if (!nand.pages || !nand.blocks || !nand.caus || !buf || ce >= 2u) return -3;
    nand_passive = 1;
    int r = nand_read_order(ce, raw_addr, buf, meta, order);
    ninfo_sync();
    return r;
}
/* v9 "DMA-ish" experiment. The driver has no evidenced DMA drain to run (see
 * nand.c): it returns the distinct unsupported code -8 and touches no
 * hardware. Guards are the same as the other NAND wrappers. */
static int g_nand_read_dmaish(unsigned ce, uint32_t raw_addr, void *buf,
                              uint32_t meta[4], unsigned variant) {
    if (!nand.pages || !nand.blocks || !nand.caus || !buf || ce >= 2u) return -3;
    nand_passive = 1;
    int r = nand_read_dmaish(ce, raw_addr, buf, meta, variant);
    ninfo_sync();
    return r;
}
/* v10 read-only BEAT-SELECT experiment. Same guards and the same deliberately
 * unvalidated raw_addr as v5..v9; the variant (base + sub-flags) is passed
 * through so nand_read_beat() can reject an unknown one with its own code
 * (-11). meta may be NULL only when the chosen variant never reads metadata,
 * which the driver enforces. Passive mode forced; no write/erase is
 * reachable. */
static int g_nand_read_beat(unsigned ce, uint32_t raw_addr, void *buf,
                            uint32_t meta[4], unsigned variant) {
    if (!nand.pages || !nand.blocks || !nand.caus || !buf || ce >= 2u) return -3;
    nand_passive = 1;
    int r = nand_read_beat(ce, raw_addr, buf, meta, variant);
    ninfo_sync();
    return r;
}
/* v11 read-only BULK-DRAIN experiment. Same guards and the same deliberately
 * unvalidated raw_addr as v5..v10; `words`/`flags` go to nand_read_bulk(),
 * which owns their bounds and rejection codes (-12/-13) BEFORE any hardware
 * access. meta4 may be NULL unless NAND_BULK_META_FIRST is set, which the
 * driver enforces. Passive mode forced; no write/erase is reachable. */
static int g_nand_read_bulk(unsigned ce, uint32_t raw_addr, void *buf,
                            uint32_t words, uint32_t *meta4, unsigned flags) {
    if (!nand.pages || !nand.blocks || !nand.caus || !buf || ce >= 2u) return -3;
    nand_passive = 1;
    int r = nand_read_bulk(ce, raw_addr, buf, words, meta4, flags);
    ninfo_sync();
    return r;
}
/* v11 read-only ECC-ARMING + beat experiment. Same guards and the same
 * deliberately unvalidated raw_addr as v5..v10; the variant is passed through
 * so nand_read_ecc() rejects an unknown one with -14. Passive mode forced; no
 * write/erase is reachable. */
static int g_nand_read_ecc(unsigned ce, uint32_t raw_addr, void *buf,
                           uint32_t meta[4], unsigned variant) {
    if (!nand.pages || !nand.blocks || !nand.caus || !buf || !meta || ce >= 2u) return -3;
    nand_passive = 1;
    int r = nand_read_ecc(ce, raw_addr, buf, meta, variant);
    ninfo_sync();
    return r;
}
/* v12 read-only SINGLE-ARMED-PHASE page read (nandscan9). Same guards and the
 * same deliberately unvalidated raw_addr as v5..v11; `words`/`variant` go to
 * nand_read_single(), which owns their bounds and rejection codes (-15/-16)
 * BEFORE any hardware access. The variant's framing (plain / +0x77 / +0x77+0x7d
 * / no deselect / full page) is the driver's. nand_single_delivered() reports
 * the words the last drain actually delivered. Passive mode forced; no
 * write/erase is reachable. */
static int g_nand_read_single(unsigned ce, uint32_t raw_addr, void *out,
                              uint32_t words, unsigned variant) {
    if (!nand.pages || !nand.blocks || !nand.caus || !out || ce >= 2u) return -3;
    nand_passive = 1;
    int r = nand_read_single(ce, raw_addr, out, words, variant);
    ninfo_sync();
    return r;
}
static uint32_t g_nand_single_delivered(void) { return nand_single_got; }
/* v13 read-only COUNTER/LENGTH page read (nandscan10). Same guards and the same
 * deliberately unvalidated raw_addr as v5..v12; `words`/`variant` go to
 * nand_read_words(), which owns their bounds and rejection codes (-17/-18)
 * BEFORE any hardware access. The drain's counter interaction (low-byte gate /
 * unconditional pop / 32-bit gate / discard-and-retry / half / double) is the
 * driver's. nand_words_delivered() reports the words the last drain actually
 * delivered. Passive mode forced; no write/erase is reachable. */
static int g_nand_read_words(unsigned ce, uint32_t raw_addr, void *buf,
                             uint32_t words, unsigned variant) {
    if (!nand.pages || !nand.blocks || !nand.caus || !buf || ce >= 2u) return -3;
    nand_passive = 1;
    int r = nand_read_words(ce, raw_addr, buf, words, variant);
    ninfo_sync();
    return r;
}
static uint32_t g_nand_words_delivered(void) { return nand_words_got; }
static uint32_t g_nand_words_popped(void) { return nand_words_popped; }
static uint32_t g_nand_words_confirmed(void) { return nand_words_confirmed; }
/* v13 read-only spare-record capture (nandscan10). The REC_LATCH path reads the
 * 16-byte sector record from SPR0..3 instead of the PIO stream; it performs the
 * page-read setup plus recorder setup and never drains the payload. Same guards
 * and deliberately unvalidated raw_addr as the other read wrappers. Passive
 * mode forced; no write/erase is reachable. */
static int g_nand_read_record(unsigned ce, uint32_t raw_addr, uint32_t rec[4][4]) {
    if (!nand.pages || !nand.blocks || !nand.caus || !rec || ce >= 2u) return -3;
    nand_passive = 1;
    int r = nand_read_record(ce, raw_addr, rec);
    ninfo_sync();
    return r;
}
static int g_nand_trace_enable(int on) { nand_passive = 1; return nand_trace_enable(on); }
static uint32_t g_nand_trace_fetch(uint32_t *dst, uint32_t max_entries) {
    nand_passive = 1;
    return nand_trace_fetch(dst, max_entries);
}
static void g_nand_trace_stats(uint32_t out[9]) { nand_passive = 1; nand_trace_stats(out); }
static int g_nand_read_ppn(unsigned ce, uint32_t row, unsigned sector,
                           void *buf, uint32_t meta[4]) {
    nand_passive = 1;
    int r = nand_read_ppn(ce, row, sector, buf, meta);
    ninfo_sync();
    return r;
}
static int g_nand_read_ppn_page(unsigned ce, uint32_t row, void *buf, uint32_t meta[16]) {
    nand_passive = 1;
    int r = nand_read_ppn_page(ce, row, buf, meta);
    ninfo_sync();
    return r;
}
static int g_nand_read_ppn_context(unsigned block, unsigned page, unsigned view, unsigned count,
                                  void *buf, uint32_t meta[16]) {
    nand_passive = 1;
    int r = nand_read_ppn_context(block, page, view, count, buf, meta);
    ninfo_sync();
    return r;
}
static int g_nand_inspect_ppn_page(unsigned ce, uint32_t row, void *buf, uint32_t meta[16]) {
    nand_passive = 1;
    int r = nand_inspect_ppn_page(ce, row, buf, meta);
    ninfo_sync();
    return r;
}

static const struct nanoos_api api = {
    NGAME_ABI, NGAME_API_VERSION << 16 | (NANOOS_MMU ? NGAME_API_MMU : 0),
    lcd_back, lcd_present, lcd_service, lcd_ready, lcd_flush, lcd_clear,
    lcd_fill_rows, lcd_text, lcd_center, input_poll, touch_poll,
    (const struct ngame_touch *)&touch, delay_us,
    usec, dcache_clean, input_held, input_service,
    mbx_init, mbx_begin, mbx_submit, mbx_set_background, mbx_render, mbx_wait_done,
    mbx_halt, mbx_set_target,
    g_nand_probe, g_nand_read, &ninfo, g_nand_read_ext,
    g_nand_read_raw,
    g_nand_read_mode, g_nand_regs,
    g_nand_read_phase,
    g_nand_read_shape,
    g_nand_read_order,
    g_nand_read_dmaish,
    g_nand_read_beat,
    g_nand_read_bulk,
    g_nand_read_ecc,
    g_nand_read_single, g_nand_single_delivered,
    g_nand_read_words, g_nand_words_delivered,
    g_nand_words_popped, g_nand_words_confirmed, g_nand_read_record,
    g_nand_trace_enable, g_nand_trace_fetch, g_nand_trace_stats,
    g_nand_read_ppn,
    g_nand_read_ppn_page,
    g_nand_read_ppn_context,
    g_nand_inspect_ppn_page,
    mbx_timings,
    mbx_submit_parts,
    mbx_queue_capacity, mbx_render_async,
    lcd_present_buffer, lcd_buffer_busy,
};

static int outside(uint32_t a, uint32_t n, uint32_t lo, uint32_t hi) {
    return a + n <= lo || a >= hi;
}

static int valid_header(const struct ngame_hdr *g, uint32_t size) {
    uint32_t load = g->load;
    return size >= sizeof *g && g->magic == NGAME_MAGIC && g->abi == NGAME_ABI && g->size == size && !(size & 3u) &&
        load - MM_GAME < MM_GAME_END - MM_GAME && !(load & 3u) && size <= MM_GAME_END - load &&
        g->bss_end >= load + size && g->bss_end <= MM_GAME_END &&
        g->entry - load - sizeof *g < size - sizeof *g;
}
/* Existing simple chainloader embeds only a payload and does not supply game
 * chunks. An appended NPAK can safely carry a small game: startup relocates it
 * to the owned asset arena before BSS clear. Both asset and NGM1 CRCs are checked. */
static int asset_game_boot(void) {
    uint32_t size = 0;
    const struct ngame_hdr *src = asset_find("game.bin", &size);
    if (!src || size < sizeof *src || !valid_header(src, size)) return 0;
    uint32_t *d = (uint32_t *)src->load; const uint32_t *s = (const uint32_t *)src;
    for (uint32_t i = 0; i < size / 4; ++i) d[i] = s[i];
    const struct ngame_hdr *g = (const struct ngame_hdr *)d;
    if (crc32((const uint8_t *)g + sizeof *g, size - sizeof *g) != g->crc) return 0;
    game = g; return 1;
}
#define NB_RUNS 8192u
#define NB_UPDATES 65536u
struct nand_boot_ws {
    struct ns_mount mount;
    struct ns_disk disk;
    struct ns_hfs fs;
    struct ns_run runs[NB_RUNS];
    struct ns_update updates[NB_UPDATES];
    uint8_t mask[2088], states[2088], leases[2088];
    uint32_t fg[2088], fk[2088];
    uint32_t page[4096], record[16], cache[4096], cache_record[16];
    unsigned char cat[32768], ext[32768];
};

static uint32_t nb_clock_us(void *p) { (void)p; return USEC; }
static int nb_inspect(void *p, uint32_t ce, uint32_t row, void *data, uint32_t *r) {
    (void)p; return g_nand_inspect_ppn_page(ce, row, data, r);
}
static int nb_context(void *p, unsigned b, unsigned pg, unsigned view, unsigned n, void *data, uint32_t *r) {
    (void)p; return g_nand_read_ppn_context(b, pg, view, n, data, r);
}
static int nb_read_bytes(void *p, uint64_t off, void *out, uint32_t n) {
    struct nand_boot_ws *ws = p;
    uint32_t done = 0;
    int rc = ns_disk_pread(&ws->disk, off, out, n, &done);
    return rc ? rc : (done == n ? 0 : NS_IO);
}

/* Load a standalone NGM1 executable directly from the HFS+ NAND volume into
 * MM_GAME (up to 10 MiB), using MM_GAME_HEAP+0x00400000 as temporary mount
 * scratch (clear of all report mirrors and MM_GAME). */
static int nand_game_boot(void) {
    if (!g_nand_probe()) return 0;
    if (ninfo.caus != 2u || ninfo.blocks != 2088u || ninfo.pages != 128u) return 0;

    struct nand_boot_ws *ws = (struct nand_boot_ws *)(uintptr_t)(MM_GAME_HEAP + 0x00400000u);
    uint8_t *z = (uint8_t *)ws;
    for (uint32_t i = 0; i < sizeof *ws; ++i) z[i] = 0;

    struct ns_mount_io io = { ws, nb_inspect, nb_context, nb_clock_us };
    uint32_t budget = 150000000u, began = USEC;
    int rc = ns_mount_n31(&ws->mount, &io, ws->mask, ws->states, ws->fg, ws->fk,
                          ws->page, ws->record, ws->runs, NB_RUNS,
                          ws->updates, NB_UPDATES, budget);
    if (rc == NS_IO || rc == NS_FORMAT) {
        uint32_t elapsed = USEC - began;
        if (elapsed >= budget) return 0;
        rc = ns_mount_n31_recover(&ws->mount, &io, ws->mask, ws->states, ws->fg, ws->fk,
                                  ws->page, ws->record, ws->runs, NB_RUNS,
                                  ws->updates, NB_UPDATES, budget - elapsed);
    }
    if (rc) return 0;
    if (ns_disk_init(&ws->disk, &ws->mount.map, ns_mount_page_read, &ws->mount, ws->cache, ws->cache_record)) return 0;
    uint64_t prefix = (uint64_t)0xc040u << 12;
    uint64_t size_bytes = (uint64_t)ws->mount.map.sectors << 12;
    uint64_t off = 0, bytes = 0;
    if (size_bytes <= prefix) return 0;
    if (ns_partition_hfs(nb_read_bytes, ws, prefix, size_bytes - prefix, &off, &bytes)) return 0;
    if (ns_hfs_mount(&ws->fs, nb_read_bytes, ws, off, bytes, ws->cat, ws->ext)) return 0;

    static const char *const paths[] = { "/Apps/Data/Game/game.bin", "/Apps/NanoOS/game.bin" };
    struct ns_file file;
    int opened = 0;
    for (unsigned i = 0; i < 2; ++i) {
        if (ns_hfs_open(&ws->fs, paths[i], &file) == 0) { opened = 1; break; }
    }
    if (!opened || file.fork.size < sizeof(struct ngame_hdr) || file.fork.size > MM_GAME_END - MM_GAME) return 0;

    struct ngame_hdr hdr;
    uint32_t got = 0;
    if (ns_hfs_pread(&ws->fs, &file, 0, &hdr, sizeof hdr, &got) || got != sizeof hdr) return 0;
    if (!valid_header(&hdr, (uint32_t)file.fork.size)) return 0;

    uint8_t *dst = (uint8_t *)(uintptr_t)hdr.load;
    uint32_t total = hdr.size, pos = 0;
    while (pos < total) {
        uint32_t chunk = total - pos;
        if (chunk > 65536u) chunk = 65536u;
        got = 0;
        if (ns_hfs_pread(&ws->fs, &file, pos, dst + pos, chunk, &got) || got == 0) return 0;
        pos += got;
    }

    const struct ngame_hdr *g = (const struct ngame_hdr *)dst;
    if (crc32((const uint8_t *)g + sizeof *g, total - sizeof *g) != g->crc) return 0;
    game = g;
    return 1;
}

/* Invalid/missing handoff selects the explicit embedded-asset route first, then
 * falls back to loading an NGM1 executable from NAND HFS+ storage. */
int game_boot(void) {
    game = 0;
    const volatile uint32_t *h = (const volatile uint32_t *)N7_HANDOFF_ADDR;
    if (h[0] != N7_HANDOFF_MAGIC || h[1] != 1 || h[6] != sizeof(struct nboot_game)) {
        if (asset_game_boot()) return 1;
        return nand_game_boot();
    }
    const struct nboot_game *b = (const struct nboot_game *)h[5];
    if ((uint32_t)b < MM_STAGE || (uint32_t)b > MM_DRAM_END - sizeof *b ||
        b->magic != NBOOT_MAGIC || b->version != NBOOT_VERSION || b->nchunks > NBOOT_CHUNKS ||
        !b->nchunks || b->chunk[0].len < sizeof(struct ngame_hdr)) return 0;
    uint32_t total = 0;
    for (uint32_t i = 0; i < b->nchunks; ++i) {
        uint32_t a = b->chunk[i].addr, n = b->chunk[i].len;
        if (a < MM_STAGE || a & 3 || n > MM_DRAM_END - a || !outside(a, n, MM_GAME, MM_GAME_END) ||
            (n & 3 && i + 1 < b->nchunks)) return 0;
        total += n;
    }
    const struct ngame_hdr *g = (const struct ngame_hdr *)b->chunk[0].addr;
    uint32_t load = g->load, size = g->size;
    if (size != b->size || size != total || !valid_header(g, size)) return 0;
    /* no chunk overlaps the image: plain word copies in file order */
    uint32_t *d = (uint32_t *)load;
    for (uint32_t i = 0; i < b->nchunks; ++i) {
        const uint32_t *s = (const uint32_t *)b->chunk[i].addr;
        for (uint32_t n = (b->chunk[i].len + 3) / 4; n--;) *d++ = *s++;
    }
    g = (const struct ngame_hdr *)load;
    if (crc32((const uint8_t *)g + sizeof *g, size - sizeof *g) != g->crc) return 0;
    game = g;
    return 1;
}

void game_run(void) {
    const struct ngame_hdr *g = game;
    if (!g) return;
    for (uint32_t *p = (uint32_t *)(g->load + g->size); p < (uint32_t *)g->bss_end; ++p) *p = 0;
    /* the copied code is still in the D-cache: to DRAM, then fetch fresh */
    dcache_clean_all();
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 0\n mcr p15, 0, %0, c7, c5, 6\n dsb sy\n isb"
                     :: "r"(0) : "memory");
    ((int (*)(const struct nanoos_api *))g->entry)(&api);
}
