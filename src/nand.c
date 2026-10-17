#pragma GCC optimize("Os")   /* register pokes, no hot path: size first */
#include "nand.h"
#include "timer.h"
#include "fmt.h"
#include "report.h"
#include "clk.h"
/* FMC 0x38a00000, PPN ("managed") NAND, CPU register path. Every sequence
 * is RetailOS's own (snap6 disassembly), not the NanoEmu shortcut:
 *   command  +0x08 = cmd (issued on the write), wait +0x48 bit 1 (0x0840b1fc)
 *   address  +0x0c/+0x10 bytes LSB first, +0x2c = n - 1, +0x04 = 1, wait
 *            bit 2 (0x0810157c)
 *   data in  +0x30 = len - 1, +0x14 = 1, +0x24 = 0, +0x04 = 0x22, wait bit 3;
 *            PIO: +0x30 = 3, +0x14 = 0x100, +0x00 &= ~(bit 8 | bit 26),
 *            +0x28 = 0, +0x04 = 0x1e0, pop +0x80 until +0x28's low byte
 *            counts the word, +0x48 = bit 20, +0x04 = 0x20; <= 1 KB per
 *            call (0x084095f0, 0x080d33fc)
 *   status   +0x48 = bit 23, +0x04 = 0xea, wait bit 23 (then +0x04 = 0x20),
 *            +0x4c = status byte, +0x48 = bit 24 (0x0840c3e8)
 *   waits    W1C the bit once set; 20 ms (0x081810b8)
 * Chip select: +0x00 = timing | (1 << ce) << 1 | 1, 1 = none. READ ID
 * (0x084d13d8) uses the runtime timing [0x0896e8e0 + 0x24] = 0x20010000;
 * reset (0x08010b4c) uses 0xff000 (slowest). The old code read +0x80
 * without a data phase: the FIFO was empty, hence "id 00000000".
 * Clock: RetailOS never gates the FMC (no gate call in its NAND code), so
 * nothing is ungated here; the report logs the CLKCON gate words.
 * Sector read = the FMSS read program (0x0896eb00) as CPU steps: 0x07 + 8
 * address bytes (byte 0 = count << 4, bits 23:20 = first sector, +0x10 =
 * page), 0x37, status (0x77 0x7d poll, bit 4 = error), 0x7a, then the
 * stream: 16 B meta + 4 x 1 KB data, here by PIO instead of the +0x34 DMA.
 *
 * No FTL: in NanoEmu raw reads come back 0xff ("blank"; the disk image is
 * served only to RetailOS's FMSS/FTL path). RetailOS's sftl (metadata
 * type 1 = LBA) is the next, read-only step. */
#define FMC(o) (*(volatile uint32_t *)(0x38a00000u + (o)))
#define CLK(o) (*(volatile uint32_t *)(0x3c500000u + (o)))
#define WAIT_US 20000u
#define POLL 0x800000u
#define RPT_NAND_ID 81u
struct nand_info nand;
/* first failure of a probe: the wait's +0x48 bit (2 cmd, 4 addr, 8 data
 * arm, 0x100 PIO stall, POLL status), +0x48 and +0x00 read then */
static void failed(uint32_t m) {
    ++nand.errors;
    if (nand.ph) return;
    nand.ph = m;
    nand.ph48 = FMC(0x48);
    nand.ph00 = FMC(0x00);
}
static int wt(uint32_t m) {
    if (wait_any(&FMC(0x48), m, WAIT_US) != WAIT_OK) { failed(m); return -1; }
    if (m == POLL) FMC(0x04) = 0x20;
    FMC(0x48) = m;
    return 0;
}
static int cmd(unsigned c) { FMC(0x08) = c; return wt(2); }
static int addr(unsigned n, uint32_t a0, uint32_t a1) {
    FMC(0x0c) = a0;
    FMC(0x10) = a1;
    FMC(0x2c) = n - 1;
    FMC(0x04) = 1;
    return wt(4);
}
static int poll(void) {
    FMC(0x48) = POLL;
    FMC(0x04) = 0xea;
    int r = wt(POLL);
    nand.st = FMC(0x4c) & 0xff;
    FMC(0x48) = 0x1000000u;
    return r;
}
static int data_in(uint32_t *buf, unsigned len) {
    FMC(0x30) = len - 1;
    FMC(0x14) = 1;
    FMC(0x24) = 0;
    FMC(0x04) = 0x22;
    int r = wt(8);
    if (r) return r;                    /* arming failed: no FIFO pops */
    FMC(0x30) = 3;
    FMC(0x14) = 0x100;
    FMC(0x00) &= 0xfbfffeffu;
    FMC(0x28) = 0;
    FMC(0x04) = 0x1e0;
    for (unsigned i = 0, stall = 0; i < len / 4;) {
        buf[i] = FMC(0x80);
        if ((FMC(0x28) & 0xff) == ((i * 4 + 4) & 0xff)) { ++i; stall = 0; }
        else if (++stall > 4096) { failed(0x100); r = -1; break; }
    }
    FMC(0x48) = 0x100000u;
    FMC(0x04) = 0x20;
    return r;
}
static void sel(unsigned ce) { FMC(0x00) = nand.tm | (1u << ce) << 1 | 1u; }
static void desel(void) { FMC(0x00) = 1; }
/* Round 7. Device: round 5 "NAND OK PPN 16448B 128pg rd p0 data", round 6
 * "NAND ERR id 00000000 cs 00000000 st 00 er 18" = every wait expired (9
 * per probe, two Drivers entries) and FMC +0x00 read 0: the FMC did not
 * complete even a command phase. No round-6 code touches the FMC's clock,
 * reset, pins or power before the probe (the gate 0x29 that buttons.c
 * cycles is I2C0's by RetailOS's own I2C hook, IRAM 0x220004cc; the pads
 * input.c muxes are I2C1's by RetailOS's I2C1 init), so the handoff state
 * itself is the suspect: a sequencer program or transfer left in flight,
 * or a clock domain left off (the device hands over with +0x14 bit 31
 * set). Not reproducible here. Hence: the probe runs first in
 * drivers_enter, a third attempt follows an FMC quiesce (RetailOS's own
 * end-of-phase steps: +0x04 = 0x20, W1C +0x48, transfer config cleared,
 * deselect) and a longer settle, and R81 dumps the raw FMC, sequencer,
 * CLKCON and I2C1-pad GPIO registers every run for the next hardware
 * reading. No gate or domain is written: nothing is known to be wrong. */
static void quiesce(void) {
    desel();
    FMC(0x04) = 0x20;
    FMC(0x48) = 0xffffffffu;
    FMC(0x14) = 0;
    FMC(0x24) = 0;
    FMC(0x28) = 0;
    delay_us(1000);
}
/* Round 8: RetailOS's FMC/FMSS controller reset 0x080ff698, which its
 * detection (0x084cce2c -> 0x0812a760) runs before any NAND reset or READ
 * ID (Codex RE). Every wait bounded (timer.c). 0 = done, else the step
 * that timed out: 1 sequencer stop (+0xc08), 2 +0xc60/+0xc64 handshake,
 * 3 +0x04 bit 31. */
static unsigned ctl_init(void) {
    FMC(0x00) = 1;                      /* deselect */
    FMC(0xc00) = 8;
    uint32_t t0 = USEC, v;
    for (uint32_t n = 0; (v = FMC(0xc08)) != 0; ++n) {
        if (v == 4) FMC(0xc00) = 2;
        if (USEC - t0 >= WAIT_US || n >= WAIT_US * 64u) return 1;
    }
    if (FMC(0xc6c) != 0x10) {
        FMC(0xc60) = 1;
        int r = wait_until(&FMC(0xc64), ~0u, 1, WAIT_US);
        FMC(0xc60) = 0;                 /* on the timeout too: never left at 1 */
        if (r != WAIT_OK) return 2;
    }
    FMC(0xc00) = 0x00080000u;
    FMC(0x04) = 0x00000800u;
    if (wait_any(&FMC(0x04), 0x80000000u, WAIT_US) != WAIT_OK) return 3;
    FMC(0x04) = 0x80000000u;
    FMC(0x81c) = 1;
    return 0;
}
/* attempts: 1 controller reset (ctl_init, even if a step timed out: the
 * probe still tells more), 0xff reset and READ ID at the slow timing
 * 0xff000 as RetailOS's detection; 2 READ ID at the runtime timing
 * 0x20010000 without a reset (the round-5 path that answered); 3 FMC
 * quiesce, slow 0xff reset, 100 ms. Each phase stops the attempt at its
 * first failure (no FIFO pops after a failed arm). */
int nand_probe(void) {
    uint32_t p[14];
    ++nand.probes;
    nand.errors = 0;
    nand.ph = 0;
    nand.cs = FMC(0x00);
    nand.st48 = FMC(0x48);
    /* An init timeout is only logged (nand.init) and the probe continues,
     * for diagnostics: on the device the runtime-timing attempt answered
     * without this reset before. Once NAND is confirmed OK on hardware with
     * ctl_init in place, make this fail-early. */
    nand.init = ctl_init();
    for (unsigned t = 0; t < 3; ++t) {
        nand.attempt = t + 1;
        nand.tm = t == 1 ? 0x20010000u : 0xff000u;
        if (t == 2) quiesce();
        if (t != 1) {   /* reset CE0 like the boot path */
            sel(0); delay_us(10); cmd(0xff); delay_us(t ? 100000 : 50000);
        }
        p[0] = p[1] = 0;
        sel(0);
        if (!cmd(0x90) && !addr(1, 0, 0)) data_in(p, 8);
        desel();
        nand.id = p[0]; nand.id2 = p[1];
        nand.err_at[t] = nand.errors;
        if ((p[0] & 0xffffff) == 0x4e5050) break;   /* "PPN" */
    }
    if ((p[0] & 0xffffff) != 0x4e5050) return 0;
    /* parameter page (0x0812a214): only the geometry words are read */
    sel(0);
    cmd(0x92); addr(1, 0, 0); cmd(0x97); cmd(0x77); cmd(0x7d);
    int ok = !poll() && !cmd(0x7a) && !data_in(p, sizeof p);
    if (ok) {
        nand.caus = p[4];           /* +0x10 CAUs per channel */
        nand.blocks = p[6];         /* +0x18 blocks per CAU */
        nand.pages = p[8];          /* +0x20 pages per block */
        nand.page_size = p[13];     /* +0x34 */
    }
    cmd(0x77);
    desel();
    return ok;
}
int nand_read_sector(unsigned ce, uint32_t page, unsigned sector,
                     void *buf, uint32_t meta[4]) {
    int r;
    sel(ce);
    r = cmd(0x07);
    r |= addr(8, 1u << 4 | (sector & 15) << 20, page);
    r |= cmd(0x37);
    r |= cmd(0x77);
    r |= cmd(0x7d);
    r |= poll();
    if (!r && (nand.st & 0x10)) r = -2;
    if (!r) {
        r = cmd(0x7a);
        r |= data_in(meta, 16);
        for (unsigned k = 0; k < 4; ++k)
            r |= data_in((uint32_t *)buf + 256 * k, 1024);
    }
    desel();
    return r;
}
/* R81 text (report.h mailbox, exported by NanoOSLoader as R81.txt): a
 * free DevMode slot (id 0) or the last R81, searched from slot 3 down */
static void hexes(const volatile uint32_t *w, unsigned n) {
    for (unsigned i = 0; i < n; ++i) { fmt_str(" "); fmt_hex(w[i], 8); }
    fmt_line(0, 0, 0);
}
/* tagged register rows, 4 words a line (fmt_line caps at 63 chars) */
static void rows(const char *tag, const volatile uint32_t *w, unsigned n) {
    for (unsigned i = 0; i < n; i += 4) {
        fmt_str(tag);
        hexes(w + i, n - i < 4 ? n - i : 4);
    }
}
#define GP(a) (*(volatile uint32_t *)(a))
/* dg: probes, attempt that answered, failed waits after attempts 1/2/3,
 * +0x48 as found, timer wait timeouts and spins, clk_apply result (R86),
 * sequencer +0xc00 kick/status, +0xc04 base, +0xc38 pc; ctl_init result,
 * first failed phase (wt bit), +0x48 and +0x00 then, +0xc08 +0xc60 +0xc64
 * +0xc6c now (round 8); fmc: +0x00..+0x7c
 * (not the FIFO); clk: CLKCON +0x00..+0x1c; ck: +0x40 +0x44 +0x64, GPIO
 * banks 12/14 config and level (I2C1 pads), sequencer +0xc10 irq enable */
static void raw_dump(void) {
    uint32_t w[20] = { nand.probes, nand.attempt, nand.err_at[0], nand.err_at[1],
                       nand.err_at[2], nand.st48, wait_stats.timeouts, wait_stats.spins,
                       (uint32_t)clk_result, FMC(0xc00), FMC(0xc04), FMC(0xc38),
                       nand.init, nand.ph, nand.ph48, nand.ph00,
                       FMC(0xc08), FMC(0xc60), FMC(0xc64), FMC(0xc6c) };
    rows("dg", w, 20);
    rows("fmc", &FMC(0x00), 32);
    rows("clk", &CLK(0x00), 8);
    w[0] = CLK(0x40); w[1] = CLK(0x44); w[2] = CLK(0x64); w[3] = GP(0x3cf00180u);
    w[4] = GP(0x3cf00184u); w[5] = GP(0x3cf001c0u); w[6] = GP(0x3cf001c4u); w[7] = FMC(0xc10);
    rows("ck", w, 8);
}
#if NANOOS_NAND_WRITE
static void write_selftest(uint32_t *buf);
#endif
int nand_selftest(uint32_t *buf, int *rd, uint32_t meta[4]) {
    volatile struct rpt_hdr *h = RPT_HDR;
    int ok = nand_probe();
    unsigned s = 4;   /* none free: slot 3 (the loader frees all on export) */
    if (!rpt_valid()) {
        for (uint32_t i = 0; i < sizeof *h / 4; ++i) ((volatile uint32_t *)h)[i] = 0;
        h->slot_size = RPT_SLOT_SIZE;
    }
    while (--s && h->id[s] && h->id[s] != RPT_NAND_ID) ;
    if (h->id[s] && h->id[s] != RPT_NAND_ID) s = 3;
    char *p0 = RPT_SLOT(s);
    fmt_out = p0;
    fmt_end = p0 + 4096;
    /* id id2 / caus blocks pages page_size / errors cs st tm / CLKCON
     * +0x48..+0x6c (gate words +0x48 +0x4c +0x58 +0x68 +0x6c) */
    fmt_str("== NAND R81 =="); fmt_line(0, 0, 0);
    hexes(&nand.id, 2);
    hexes(&nand.caus, 4);
    hexes(&nand.errors, 4);
    hexes(&CLK(0x48), 5);
    hexes(&CLK(0x5c), 5);
    raw_dump();
    *rd = -1;
    /* page 0, then page 0 of the middle block of CAU 0 (FTL data area);
     * page address = block << log2(pages per block) */
    for (unsigned t = 0; ok && t < 2; ++t) {
        uint32_t m[4], pg = t ? (nand.blocks / 2) << (31 - __builtin_clz(nand.pages | 1)) : 0;
        buf[0] = 0x5a5a5a5au;
        int r = nand_read_sector(0, pg, 0, buf, t ? m : meta);
        if (!t) *rd = r;
        fmt_str("page "); fmt_hex(pg, 8); fmt_str(" rd "); fmt_sdec(r);
        fmt_str(" st "); fmt_hex(nand.st, 2); fmt_str(" er "); fmt_dec(nand.errors);
        fmt_line(0, 0, 0);
        hexes(t ? m : meta, 4);
        hexes(buf, 4);
        hexes(buf + 4, 4);
    }
#if NANOOS_NAND_WRITE
    write_selftest(buf);
#endif
    fmt_str("== END R81 =="); fmt_line(0, 0, 0);
    uint32_t len = (uint32_t)(fmt_out - p0);
    fmt_out = 0;
    h->id[s] = RPT_NAND_ID;
    h->len[s] = len;
    h->sum[s] = rpt_sum(p0, len);
    h->magic[0] = RPT_MAGIC0;
    h->magic[1] = RPT_MAGIC1;
    h->hdr_sum = rpt_hdr_sum();
    return ok;
}
/* ---- raw writes ------------------------------------------------------
 * Default build (NANOOS_NAND_WRITE 0): the hard stub below, nothing else.
 * This flash is wholly owned by RetailOS's Whimory sftl/VFL except CE0 CAU0
 * rows 0..0x31 and 0x100..0x131 (LLB + backup, a brick if touched). Every
 * other block is either mapped user/firmware data, an FTL context/BTOC/VFL
 * block, an open block the sftl scans page by page at mount, or a spare the
 * FTL will erase and reuse. No block is known to be outside the FTL, the
 * physical<->LBA map is not in our hands, and RetailOS was never seen
 * issuing an erase (300 s of FMC traces: 0xb0/0xb8 are page lists), so the
 * PPN erase sequence for this part is unknown. Program (nano7_fmc.c:
 * 0x8a/0x87 + 4 address bytes, meta at +0x60..+0x6c with go 0x34, 4 x 1 KB
 * via the +0x34 DMA engine, confirm 0x17) is known only from the emulator
 * trace and the +0x34 DMA path has not run on hardware. ECC is on-die for
 * PPN main storage (docs/research/ecc.md), so the host never computes it;
 * that part is not the blocker, the FTL ownership is.
 * Hence the path below is a skeleton: the guards, the blank check and the
 * readback verify are real; erase and program refuse. Filling the
 * allowlist needs a block proven FTL-free on the real device first. */
#if NANOOS_NAND_WRITE
#define PG_CAU(p) ((p) >> 19)
#define PG_BLK(p) (((p) >> 7) & 0xfffu)
#define PG_BASE(p) ((p) & ~0x7fu)
/* EMPTY ON PURPOSE (first > last). Fill only with a block proven unmapped
 * and unused by the sftl on this exact device, never the LLB blocks. */
static const struct nand_range nand_allow[1] = { {0, 0, 1, 0} };
int nand_write_allowed(unsigned ce, uint32_t page) {
    if (ce != 0) return NAND_W_DENY;    /* CE1: never probed, never read */
    /* LLB rows 0..0x131 = CAU0 blocks 0..2, plus one block of margin */
    if (PG_CAU(page) == 0 && PG_BLK(page) < 4) return NAND_W_DENY;
    for (unsigned i = 0; i < sizeof nand_allow / sizeof nand_allow[0]; ++i) {
        const struct nand_range *r = &nand_allow[i];
        if (r->ce == ce && r->cau == PG_CAU(page) &&
            PG_BLK(page) >= r->first && PG_BLK(page) <= r->last) return 0;
    }
    return NAND_W_DENY;
}
/* A sector is provably blank only if it reads clean (no status error) and
 * every data and meta word is 0xffffffff. A blank PPN page may answer with
 * status bit 4 (uECC on erased data): that is "not proven", refused. */
static int blank(unsigned ce, uint32_t page, unsigned sector, uint32_t *tmp) {
    uint32_t m[4];
    int r = nand_read_sector(ce, page, sector, tmp, m);
    if (r) return r == -2 ? NAND_W_NOTBLANK : r;
    for (unsigned i = 0; i < 4; ++i) if (m[i] != 0xffffffffu) return NAND_W_NOTBLANK;
    for (unsigned i = 0; i < NAND_SECTOR / 4; ++i) if (tmp[i] != 0xffffffffu) return NAND_W_NOTBLANK;
    return 0;
}
int nand_erase_block(unsigned ce, uint32_t page) {
    int r = nand_write_allowed(ce, page);
    if (r) return r;
    (void)PG_BASE(page);
    return NAND_W_NOERASE;   /* no erase command known for this part */
}
/* The program step: deliberately not ported. When it is, it goes here and
 * nothing else in this file changes. */
static int program(unsigned ce, uint32_t page, unsigned sector,
                   const void *buf, const uint32_t meta[4]) {
    (void)ce; (void)page; (void)sector; (void)buf; (void)meta;
    return NAND_W_NOPROG;
}
static int verify(unsigned ce, uint32_t page, unsigned sector,
                  const void *buf, const uint32_t meta[4], uint32_t *tmp) {
    uint32_t m[4];
    int r = nand_read_sector(ce, page, sector, tmp, m);
    if (r) return r;
    for (unsigned i = 0; i < 4; ++i) if (m[i] != meta[i]) return NAND_W_VERIFY;
    for (unsigned i = 0; i < NAND_SECTOR / 4; ++i)
        if (tmp[i] != ((const uint32_t *)buf)[i]) return NAND_W_VERIFY;
    return 0;
}
int nand_write_sector(unsigned ce, uint32_t page, unsigned sector,
                      const void *buf, const uint32_t meta[4]) {
    static uint32_t tmp[NAND_SECTOR / 4];   /* .bss, not the caller's buffer */
    int r = nand_write_allowed(ce, page);
    if (!r) r = blank(ce, page, sector & 3, tmp);
    if (!r) r = program(ce, page, sector & 3, buf, meta);
    if (!r) r = verify(ce, page, sector & 3, buf, meta, tmp);
    return r;
}
/* One sacrificial block: the first allowlisted one. Counts its blank pages
 * (sector 0 of each), then erase -> write pattern -> verify, each step's
 * code on the R81 line. With the allowlist empty it only says so. */
static void write_selftest(uint32_t *buf) {
    const struct nand_range *r = &nand_allow[0];
    fmt_str("write: on allow ");
    if (r->first > r->last) { fmt_str("none"); fmt_line(0, 0, 0); return; }
    uint32_t pg = (uint32_t)r->cau << 19 | (uint32_t)r->first << 7;
    unsigned nb = 0, np = nand.pages ? nand.pages : 128;
    fmt_hex(pg, 8);
    for (unsigned i = 0; i < np && i < 128; ++i) nb += blank(r->ce, pg + i, 0, buf) == 0;
    fmt_str(" blank "); fmt_dec(nb); fmt_str("/"); fmt_dec(np);
    int e = nand_erase_block(r->ce, pg);
    fmt_str(" er "); fmt_sdec(e);
    for (unsigned i = 0; i < NAND_SECTOR / 4; ++i) buf[i] = 0xa5000000u | i;
    uint32_t meta[4] = {0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu};
    int w = e ? e : nand_write_sector(r->ce, pg, 0, buf, meta);
    fmt_str(" wr "); fmt_sdec(w);
    fmt_line(0, 0, 0);
}
#else
/* Hard stub: NanoOS also runs on the user's iPod, and a write there changes
 * flash RetailOS's FTL owns. See the comment above for the guarded path. */
int nand_write_sector(unsigned ce, uint32_t page, unsigned sector,
                      const void *buf, const uint32_t meta[4]) {
    (void)ce; (void)page; (void)sector; (void)buf; (void)meta;
    return NAND_W_OFF;
}
#endif
