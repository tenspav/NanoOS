#include "mbx.h"
#include "timer.h"
#include "memmap.h"
/* RetailOS ordinary path: TA setup, EOL, selector, ISP pairs, +680 kick.
 * PTE attributes and the complete hardware PB allocator remain unproven;
 * PTEs try an ARM-style small-page type with the existing linear mapping.
 * D-cache is off at startup; DSB publishes owned buffer/table stores. */
#define CORE(o) (*(volatile uint32_t *)(0x3b000000u + (o)))
#define TA_FIFO (*(volatile uint32_t *)0x3b800000u)
#define DOORBELL (*(volatile uint32_t *)0x3bc00000u)
#define RENDER_DONE 0x10u
#define JOB_DONE 0x44u
#define OVERFLOW 0x20u
#define WAIT_US 20000u
/* TA wait after the EOL: 20 ms timed out with +0x12c = 0 (round 7); wait
 * longer so a slow TA is told apart from a dead one (mbx_fail.ta_max_us) */
#define WAIT_TA_US 100000u
#define NX 30u
#define NY 27u
#define GRID (((NY - 1) << 16) | (NX - 1))
#define PB_BYTES 0x10000u
#define SCENE_OFFSET 0x1004u
struct mbx_info mbx;
struct mbx_dbg mbx_dbg;
uint32_t mbx_variant;
struct mbx_fail_info mbx_fail;
/* this scene: +0x12c before mbx_begin's ack, FIFO words stored, begin/EOL us */
static uint32_t scene_pre, scene_words, t_begin, t_eol;
/* Raw clock/power registers captured when mbx_init() fails:
 * [0] CLKCON+0x68 [1] 0x3c50001c [2] CLKCON+0x44 [3..5] PSC +0/+4/+8
 * [6..8] CLKCON +0/+4/+8 [9] CLKCON+0x58 [10] PSC +0x14 (domain status)
 * [11] CLKCON+0x68 and [12] CLKCON+0x58 as found on entry to mbx_init()
 * [13..18] PSC +0x0c/+0x10/+0x14/+0x18/+0x1c/+0x100 on entry, [19..24]
 * the same after the domain wakes, [25] wake result (see pd_try). */
uint32_t mbx_diag[MBX_DIAG_N];
static uint32_t t_scene;
static int scene_failed, job_result = -1, pfx_pending;
/* Doorbells. Round 15: ONE PER SEGMENT again (MBX_BELL_BATCH 0, mbx.h).
 * A 0x1000xxxx draw packet carries no vertex count: after its primitive and
 * control words the TA takes every FIFO0 word as vertex data until the
 * doorbell. The round-6 batching (one bell per >= 512 words after the 3rd
 * segment) packed ~16 ViceNano draw packets per bell, so the TA ate their
 * headers as vertices, ended mid-vertex (32-word packets, 6-word vertices)
 * and swallowed the EOL: RSN 8 on every world scene, never on triangle (2
 * segments) or quad (3). RetailOS's 4-bell scenes (#258/#279/#297/#692)
 * never hold two draw headers in one bell: its 394-word tail is VGP and
 * constant uploads (explicit counts) plus ONE draw. Every ViceNano
 * mbx_submit is exactly one draw packet. MBX_BELL_BATCH 1 = round-6 code. */
#define BELL_SEGS 3u
#define BELL_BATCH 512u
static uint32_t seg_n, pend_words, scene_bells;
struct mbx_ta_probe mbx_probe, mbx_tafail;
/* MBX_PROBE 1: the round-12 per-word +0x12c change ring and the per-submit
 * CPU gap (mbx_probe n/seg/us/st/svc, gap_max_us/gap_seg). Off in
 * production (round 8: a USEC and probe-state access per FIFO word); the
 * per-scene probe fields (clock gates, h838, TA-fail snapshot) stay. */
#ifndef MBX_PROBE
#define MBX_PROBE 0
#endif
#if MBX_PROBE
static uint32_t trk_last, t_sub;
/* log a +0x12c change (occupancy masked) into the probe ring */
static void trk(uint32_t st) {
    st &= ~0x00ff0000u;
    if (st == trk_last) return;
    trk_last = st;
    struct mbx_ta_probe *p = &mbx_probe;
    uint32_t i = p->n++ % MBX_TRK;
    p->seg[i] = seg_n;
    p->us[i] = USEC - t_begin;
    p->st[i] = st;
    p->svc[i] = p->svc_now;
}
#else
#define trk(st) ((void)(st))      /* still reads +0x12c where given */
#endif
/* First failure of the scene: reason (2 status bit 0x20, 3 FIFO full for
 * 20 ms; +4 when it hit the EOL word), raw +0x12c, us since mbx_begin. */
static uint32_t fail_reason, fail_status, fail_us;
static uint32_t background;
/* The GPU-visible buffers (tables, PB, parameter pool and workspace) are in
 * the uncached kernel .dma (memmap.h MM_DMA), inside the first 32 MB of
 * DRAM mapped by these eight page tables. */
static uint32_t pt[8 * 1024] MM_DMA_BUF __attribute__((aligned(4096)));
/* Reserve more than the observed pointer span, independently of grid size.
 * The scene/region area is embedded in this owned PB allocation. The traced
 * +0x1004 offset is known; allocator sizes/free-list seeds are not. */
static uint32_t pb[PB_BYTES / 4] MM_DMA_BUF __attribute__((aligned(4096)));
#define SCENE_VA MBX_VA((uint8_t *)pb + SCENE_OFFSET)
static void barrier(void) { __asm__ volatile("dsb sy" ::: "memory"); }
/* RetailOS-style global parameter allocator storage (63 pages as RetailOS's
 * 828=2f5..82c=334, plus the reserved first page holding the 0x68-byte ISP
 * template) and the 16 KB allocator workspace behind +0x824. Persistent
 * across frames. 16 pages ran out on hardware (round 6: TA st 00008020,
 * ISP st F2060071 = 0x20 on the last of 113 world segments). */
#define PARAM_PAGES 63u
/* MBX_V_POOL: 256 pages; one guard page past the exclusive end */
#define PARAM_MAX 256u
#define PARAM_WORDS ((PARAM_MAX + 2u) * 1024u)
static uint32_t mbx_param[PARAM_WORDS] MM_DMA_BUF __attribute__((aligned(4096)));
static uint32_t mbx_param_aux[0x4000u / 4u] MM_DMA_BUF __attribute__((aligned(4096)));
static void barrier(void);
static uint32_t nonzero(const uint32_t *p, uint32_t words) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < words; ++i) n += p[i] != 0;
    return n;
}
uint32_t mbx_crc32(const uint32_t *w, uint32_t n) {
    uint32_t c = 0xffffffffu;
    for (uint32_t i = 0; i < n; ++i) {
        c ^= w[i];
        for (uint32_t b = 0; b < 32; ++b) c = c >> 1 ^ (0xedb88320u & -(c & 1u));
    }
    return ~c;
}
static uint32_t t_init;
/* MBX_V_DIAG checkpoint: raw +0x12c (or the status a wait ended on) and
 * time since mbx_init() entry */
static void ck_st(uint32_t i, uint32_t st) {
    if (!(mbx_variant & MBX_V_DIAG) || i >= MBX_CK_N) return;
    mbx_dbg.ck_st[i] = st;
    mbx_dbg.ck_us[i] = USEC - t_init;
    mbx_dbg.ck_mask |= 1u << i;
}
static void ck(uint32_t i) { if (mbx_variant & MBX_V_DIAG) ck_st(i, CORE(0x12c)); }
/* pool pages scanned past the template page: at least the 16-page span, so
 * a one-page pool still shows writes beyond its end */
static uint32_t pool_span(void) {
    return (mbx_dbg.pages < PARAM_PAGES ? PARAM_PAGES : mbx_dbg.pages) + 1u;
}
/* [0] after the init wait, [1] after the TA, before region finalization,
 * [2] after the ISP */
static void snap_pool(uint32_t k, const uint32_t *initial) {
    uint32_t span = pool_span(), n = 0, d = 0, m = 0;
    for (uint32_t i = 0; i < 1024u; ++i)
        d += mbx_param[i] != (i < 0x68u / 4u ? initial[i] : 0);
    if (d) m = 1;
    for (uint32_t i = 1024u; i < 1024u * (span + 1u); ++i) {
        uint32_t v = mbx_param[i], pg = i >> 10;
        if (!v) continue;
        if (n < 8) { mbx_dbg.poff[k][n] = i * 4u; mbx_dbg.pval[k][n] = v; }
        ++n;
        m |= 1u << (pg < 31u ? pg : 31u);
    }
    mbx_dbg.pnz[k] = n;
    mbx_dbg.ptpl[k] = d;
    mbx_dbg.pmask[k] = m;
    if (k > 1) return;
    mbx_dbg.pool_nz[k] = n;
    mbx_dbg.aux_nz[k] = nonzero(mbx_param_aux, 0x4000u / 4u);
    mbx_dbg.tpl_diff[k] = d;
    if (k && n) { mbx_dbg.nz_off = mbx_dbg.poff[1][0]; mbx_dbg.nz_word = mbx_dbg.pval[1][0]; }
}
/* aux at A, T and I; change counts against the previous stage */
static uint32_t aux_copy[3][0x4000u / 4u];
static void snap_aux(uint32_t k) {
    uint32_t n = 0, *c = aux_copy[k];
    for (uint32_t i = 0; i < 0x4000u / 4u; ++i) c[i] = mbx_param_aux[i];
    mbx_dbg.ran |= k ? 4u << (k - 1u) : 1u;
    if (!k) {
        for (uint32_t i = 0; i < 8; ++i) { mbx_dbg.aux_a[i] = c[i]; mbx_dbg.aux_a[8 + i] = c[4088 + i]; }
        return;
    }
    const uint32_t *p = aux_copy[k - 1u];
    mbx_dbg.aux_idx[k - 1u] = MBX_NONE;
    for (uint32_t i = 0; i < 0x4000u / 4u; ++i) {
        if (c[i] == p[i]) continue;
        if (!n++) {
            mbx_dbg.aux_idx[k - 1u] = i;
            mbx_dbg.aux_old[k - 1u] = p[i];
            mbx_dbg.aux_new[k - 1u] = c[i];
        }
    }
    mbx_dbg.aux_chg[k - 1u] = n;
}
const uint32_t *mbx_aux_copy(uint32_t k) { return aux_copy[k]; }
/* target words other than the test's 0xff00ff00 fill */
static void snap_target(const uint32_t *t, uint32_t words) {
    uint32_t n = 0;
    mbx_dbg.tgt_idx = MBX_NONE;
    for (uint32_t i = 0; i < words; ++i)
        if (t[i] != 0xff00ff00u && !n++) { mbx_dbg.tgt_idx = i; mbx_dbg.tgt_val = t[i]; }
    mbx_dbg.tgt_chg = n;
}
/* the +0x840 table (CPU zeros) at [0] T, [1] I and the gap before 844 */
static void snap_table(uint32_t k) {
    uint32_t n = 0, g = 0;
    for (uint32_t i = 0; i < 0xfc0u / 4u; ++i) {
        uint32_t v = pb[i];
        if (!v) continue;
        if (n < 8) { mbx_dbg.tbl_off[k][n] = i * 4u; mbx_dbg.tbl_val[k][n] = v; }
        ++n;
    }
    for (uint32_t i = 0xfc0u / 4u; i < SCENE_OFFSET / 4u; ++i) g += pb[i] != 0;
    mbx_dbg.tbl_nz[k] = n;
    mbx_dbg.tbl_gap[k] = g;
    mbx_dbg.tbl_hash[k] = mbx_crc32(pb, 0xfc0u / 4u);
}
static void snap_alloc(uint32_t k) {
    static const uint16_t reg[6] = { 0x824, 0x828, 0x82c, 0x838, 0x83c, 0x6d8 };
    for (uint32_t i = 0; i < 6; ++i) mbx_dbg.alloc[k][i] = CORE(reg[i]);
}
/* Round 13 pool watch (mbx.h mbx_pw): pages seen written since the last
 * allocator init, one bit per pool page (1..pool_span()) */
struct mbx_pool_watch mbx_pw;
static uint32_t pool_seen[(PARAM_MAX + 2u + 31u) / 32u];
static int page_written(const uint32_t *pg) {
    for (uint32_t i = 0; i < 16u; ++i) if (pg[i]) return 1;
    for (uint32_t i = 64u; i < 1024u; i += 64u) if (pg[i]) return 1;
    return 0;
}
/* pages newly written since the last call (reads only) */
static uint32_t pool_mark(void) {
    uint32_t span = pool_span(), n = 0;
    for (uint32_t pg = 1; pg <= span; ++pg) {
        uint32_t b = 1u << (pg & 31u), *w = &pool_seen[pg >> 5];
        if (*w & b || !page_written(mbx_param + pg * 1024u)) continue;
        *w |= b;
        ++n;
    }
    mbx_pw.used += n;
    return n;
}
/* a completed scene: its new pages and the allocator registers after it */
static void pool_scene(void) {
    struct mbx_pool_watch *p = &mbx_pw;
    uint32_t d = pool_mark(), i = p->n++ % 8u;
    p->d[i] = d;
    p->r838[i] = CORE(0x838);
    p->r820[i] = CORE(0x820);
    if (d > p->dmax) p->dmax = d;
}
/* aux free-list evidence (why: 1 re-arm, 2 failure): which "next" links
 * differ from the post-init index + 1, and where the last page points */
static void aux_evidence(uint32_t why) {
    struct mbx_pool_watch *p = &mbx_pw;
    const volatile uint16_t *hw = (const volatile uint16_t *)mbx_param_aux;
    uint32_t n = 0, e = mbx_dbg.last_page;
    for (uint32_t i = 0; i < 0x4000u / 2u - 1u; ++i) {
        if (hw[i] == i + 1u) continue;
        if (n < 4) { p->a_idx[n] = i; p->a_val[n] = hw[i]; }
        ++n;
    }
    p->a_why = why;
    p->a_used = p->used;
    p->a_n = p->n;
    p->a_links = n;
    p->a_end = e < 0x4000u / 2u ? hw[e] : MBX_NONE;
}
/* Scene constructor seed (0x080b7b7e): zeroed prefix/table, then every
 * region record is { 0x22000000, 0 } for the single-subgrid type-2 grid;
 * MBX_V_CRD adds the record's y << 8 | x and bit 31 on the last one. */
static void init_buffers(void) {
    uint32_t crd = mbx_variant & MBX_V_CRD;
    for (uint32_t i = 0; i < PB_BYTES / 4; ++i) pb[i] = 0;
    for (uint32_t i = 0; i < NX * NY; ++i)
        pb[SCENE_OFFSET / 4 + 2 * i] = 0x22000000u |
            (crd ? (i / NX) << 8 | i % NX | (i == NX * NY - 1 ? 0x80000000u : 0) : 0);
    barrier();
}
/* Identification as RetailOS does it (0x0813f574): a one-byte-per-word
 * signature at +0xff0..+0xffc (0xb105f00d) with part 0x910/0x930 at
 * +0xfe0/+0xfe4; when that is absent, the legacy core ID at +0xf00
 * (bits 31:24 == 1, bits 23:16 == 1 or 2). The nano 7G core has no
 * signature (+0xff0 reads 0x00010001) and takes the core ID path. */
static int identify(void) {
    mbx.sig = CORE(0xff0) | CORE(0xff4) << 8 | CORE(0xff8) << 16 |
              CORE(0xffc) << 24;
    mbx.id = CORE(0xf00);
    mbx.rev = CORE(0xf10);
    if (mbx.sig == 0xb105f00du) {
        uint32_t part = (CORE(0xfe4) & 0xfu) << 8 | (CORE(0xfe0) & 0xffu);
        if (part == 0x910u || part == 0x930u) return 1;
    }   /* else fall back to the core ID, as the emulator lacks +0xfe0 */
    uint32_t sub = (mbx.id >> 16) & 0xffu;
    return mbx.id >> 24 == 1u && (sub == 1u || sub == 2u);
}
/* RetailOS reset loop (0x0812b864): 16 passes of +0x080 = 0x111, delay, 0,
 * delay, with +0x134 = 0xffff acknowledged on each pass. */
static void reset(uint32_t us, uint32_t passes) {
    for (uint32_t i = 0; i < passes; ++i) {
        CORE(0x134) = 0xffffu;
        CORE(0x080) = 0x111u;
        barrier();
        delay_us(us);
        CORE(0x080) = 0;
        barrier();
        delay_us(us);
    }
}
void mbx_halt(void) {
    reset(500u, 16u);
    CORE(0x134) = 0xffffu;
}
static void capture_diag(void) {
    static const uint32_t addr[11] = {
        0x3c500068u, 0x3c50001cu, 0x3c500044u, 0x39700000u, 0x39700004u,
        0x39700008u, 0x3c500000u, 0x3c500004u, 0x3c500008u, 0x3c500058u,
        0x39700014u };
    for (uint32_t i = 0; i < 11; ++i) mbx_diag[i] = *(volatile uint32_t *)addr[i];
}
/* Power state controller 0x39700000 (RetailOS on 0x08426f3c -> 0x08001218,
 * off 0x08426d18). +0x0c = power domain n on (1 << n), +0x10 = off (so
 * sleep.md's enter/exit labels are reversed), +0x08 = request shadow,
 * +0x14 = status (bit n off, n+10 settling, n+20 an unchecked third copy),
 * +0x18/+0x1c = off/on config, +0x100 = per-domain enable mask. */
#define PSC(o) (*(volatile uint32_t *)(0x39700000u + (o)))
#define CLK(o) (*(volatile uint32_t *)(0x3c500000u + (o)))
#define PD_US 2000u
/* Domain table 0x0890bb80 (word 1 = on config, words 3..7 = the domain's
 * own CLKCON +0x48/+0x4c/+0x58/+0x68/+0x6c gates). Domains 1 and 2 also
 * retune CLKCON +0x08/+0x18 and 0x3e000008 while waking, so they are left
 * out; 7 is off on the device but has no table entry. None of these masks
 * holds the MBX bits (+0x58 bit 12, +0x68 bit 17): which domain powers the
 * MBX is unknown, hence trying each one that is off. */
static const struct { uint32_t n, on, gate[5]; } pd[] = {
    { 3, 0x00330080u, { 0, 0x00200000u, 0, 0x0000000cu, 0x00040000u } },
    { 5, 0x001d00e2u, { 0x00080000u, 0, 0x00000080u, 0x00010000u, 0 } },
    { 6, 0x00140065u, { 0, 0, 0, 0, 0 } },
};
static const uint16_t gate_reg[5] = { 0x48, 0x4c, 0x58, 0x68, 0x6c };
static const uint16_t psc_reg[6] = { 0x0c, 0x10, 0x14, 0x18, 0x1c, 0x100 };
static void psc_snap(uint32_t *d) {
    for (uint32_t i = 0; i < 6; ++i) d[i] = PSC(psc_reg[i]);
}
/* Bounded copy of the stock poll: until (status & m) == want(). */
static int psc_wait(uint32_t m, int shadow, uint32_t want) {
    uint32_t start = USEC;
    for (uint32_t spins = 0; spins < PD_US * 64u; ++spins) {
        if (shadow) want = PSC(0x08) & m;
        if ((PSC(0x14) & m) == want) return 0;
        if (USEC - start >= PD_US) break;
    }
    return -1;
}
/* The stock wake of domain i's entry, polls bounded. 0 = woken, 1 = was
 * already on, 2/3 = shadow/settle poll timed out (gates still restored). */
static uint32_t pd_wake(uint32_t i) {
    uint32_t n = pd[i].n, bit = 1u << n, saved[5], r = 0;
    if (!(PSC(0x14) & bit)) return 1;
    for (uint32_t g = 0; g < 5; ++g) {
        saved[g] = CLK(gate_reg[g]);
        if (pd[i].gate[g]) CLK(gate_reg[g]) = saved[g] & ~pd[i].gate[g];
    }
    PSC(0x1c) = pd[i].on;
    PSC(0x100) &= ~bit;
    PSC(0x0c) = bit;
    barrier();
    if (psc_wait(bit, 1, 0)) r = 2;
    else if (psc_wait(bit << 10, 0, 0)) r = 3;
    PSC(0x100) |= bit;
    for (uint32_t g = 0; g < 5; ++g) CLK(gate_reg[g]) = saved[g];
    barrier();
    return r;
}
static void ungate(void) {
    /* Ungate the MBX peripheral clock. RetailOS's gate getter (0x0802fa74)
     * reports a device on when NONE of its table bits are set in CLKCON
     * +0x48/+0x4c/+0x58/+0x68/+0x6c, i.e. a set bit gates the clock off.
     * The only table entry (0x08917860) holding +0x68 bit 17 is id 0x2e,
     * which also has +0x58 bit 12: both must be clear. Experimental: the
     * id-to-MBX link is inferred from the bit, not from driver code. */
    CLK(0x58) &= ~(1u << 12);
    /* RetailOS analysis: the MBX sits on power domain 4, whose own
     * peripheral gates are CLKCON +0x68 bits 5-7 (mask 0xe0); a set bit
     * gates the clock off, so clear them with bit 17 in one write. */
    CLK(0x68) &= ~((1u << 17) | 0xe0u);
    barrier();
    delay_us(100);
}
/* Wake each off candidate domain in turn, the first before any MBX gate
 * change, and stop once the ID reads right. Result nibbles (domain 3, 5, 6
 * from the top): pd_wake codes, F = not tried; bits 15:12 = the domain
 * after which the ID passed (0 = none). Domains that did not help stay on,
 * as RetailOS leaves them when in use. */
static int pd_try(void) {
    uint32_t res = 0x0fffu;
    int ok = 0;
    psc_snap(&mbx_diag[13]);
    for (uint32_t i = 0; i < 3 && !ok; ++i) {
        uint32_t r = pd_wake(i), sh = 8u - 4u * i;
        res = (res & ~(0xfu << sh)) | r << sh;
        ungate();
        if (r >= 2) break;
        if (r == 0 && identify()) { ok = 1; res |= pd[i].n << 12; }
    }
    psc_snap(&mbx_diag[19]);
    mbx_diag[25] = res;
    return ok;
}
/* RetailOS 0x080b26e4/0x080b7e34: template in the first 0x68 bytes, then the
 * page-number range and the 0x09000000 init command, wait for event 0x40. */
static const uint32_t *template_words(void);
static int init_parameter_allocator(void) {
    const uint32_t *initial = template_words();
    uint32_t pages = mbx_variant & MBX_V_POOL ? PARAM_MAX :
                     mbx_variant & MBX_V_POOL1 ? 1u : PARAM_PAGES;
    mbx_dbg.pages = pages;
    for (uint32_t i = 0; i < (pool_span() + 1u) * 1024u; ++i) mbx_param[i] = 0;
    for (uint32_t i = 0; i < 0x4000u / 4u; ++i) mbx_param_aux[i] = 0;
    for (uint32_t i = 0; i < 0x68u / 4u; ++i) mbx_param[i] = initial[i];
    for (uint32_t i = 0; i < sizeof pool_seen / 4u; ++i) pool_seen[i] = 0;
    mbx_pw.used = mbx_pw.n = 0;
    ++mbx_pw.arms;
    uint32_t va = MBX_VA(mbx_param);
    uint32_t first_page = (va >> 12) + 1u;
    barrier();
    CORE(0x134) = 0x40u;
    mbx_dbg.st[1] = CORE(0x12c);
    CORE(0x824) = MBX_VA(mbx_param_aux);
    mbx_dbg.st[2] = CORE(0x12c);
    CORE(0x828) = first_page;
    mbx_dbg.st[3] = CORE(0x12c);
    /* RetailOS end = first + pages (2f5 + 63 = 334); the zeroed span
     * (pool_span) covers page first + pages too. MBX_V_POOL1: end = first. */
    mbx_dbg.first_page = first_page;
    mbx_dbg.last_page = first_page + pages - (mbx_variant & MBX_V_POOL1 ? 1u : 0u);
    CORE(0x82c) = mbx_dbg.last_page;
    mbx_dbg.st[4] = CORE(0x12c);
    CORE(0x838) = 1u;
    mbx_dbg.st[5] = CORE(0x12c);
    CORE(0x83c) = va;
    mbx_dbg.st[6] = CORE(0x12c);
    barrier();
    CORE(0x6d8) = 0x09000000u;
    mbx_dbg.st[7] = CORE(0x12c);
    barrier();
    int ok = wait_any(&CORE(0x12c), 0x40u, WAIT_US) == WAIT_OK;
    ck(MBX_CK_A);
    CORE(0x134) = 0x40u;
    barrier();
    if (!ok) ++mbx.timeouts;
    snap_alloc(0);
    if (mbx_variant & MBX_V_DIAG) { snap_pool(0, initial); snap_aux(0); }
    return ok;
}
/* The new-context slot select of 0x0813175c (810 = slot,
 * 81c = 1, completion on event 0x100 for slot 0), raw-status polled with
 * no IRQ handler; slot 0 stays owned (no release). */
/* Round 15: RetailOS's event service as traced (#695-697, #727-729 ...):
 * mask all GPU events (+0x130 = 0), ack, restore the 0x8775 mask. A wake
 * is +0x12c = 1 followed by that service for event 1. */
static void ev_ack(uint32_t bits) {
    CORE(0x130) = 0;
    barrier();
    CORE(0x134) = bits;
    barrier();
    CORE(0x130) = 0x8775u;
    barrier();
}
static void wake(void) {
    CORE(0x12c) = 1;
    barrier();
    ev_ack(1);
}
static void ctx_init(void) {
    static const uint16_t reg[4] = { 0x810, 0x81c, 0x820, 0x864 };
    uint32_t st, spins = 0, t0;
    mbx_dbg.ctx_pre[0] = CORE(0x12c);
    for (uint32_t i = 0; i < 4; ++i) mbx_dbg.ctx_pre[1 + i] = CORE(reg[i]);
    CORE(0x134) = 0x100u;
    barrier();
    CORE(0x810) = 0;
    barrier();
    t0 = USEC;
    CORE(0x81c) = 1;
    barrier();
    for (;;) {
        st = CORE(0x12c);
        if (st & 0x100u) break;
        if (USEC - t0 >= WAIT_US || ++spins / 64u >= WAIT_US) break;
    }
    mbx_dbg.ctx_us = USEC - t0;
    mbx_dbg.ctx_st = st;
    mbx_dbg.ctx_res = st & 0x100u ? 1u : 2u;
    ck_st(MBX_CK_CTX, st);
    ev_ack(0x100u);              /* trace #736-738 */
    mbx_dbg.ctx_post[0] = CORE(0x12c);
    for (uint32_t i = 0; i < 4; ++i) mbx_dbg.ctx_post[1 + i] = CORE(reg[i]);
}
/* first-launch trace (/tmp/mbx_coldboot_trace.txt #16..#225): startup
 * prefix streamed before the first +0x800, no doorbell */
const uint32_t mbx_prefix[MBX_PREFIX_WORDS] = {
    0x10000010u, 0x2003801au, 0x80000004u, 0x01000700u, 0x90000007u, 0xe4e4e408u,
    0x4c440000u, 0xe4e4e408u, 0x4c840010u, 0xe4e4e408u, 0x4d040020u, 0xe4e4e408u,
    0x4e040030u, 0xe4e4c601u, 0x07c1000fu, 0xe4e4e401u, 0x03c20003u, 0xe4e4e408u,
    0x14461003u, 0xe4e4e408u, 0x14861013u, 0x80000008u, 0x00000c08u, 0x9008000cu,
    0xe4e4e408u, 0x410400a0u, 0xe4e4e408u, 0x40440080u, 0xe4e4e408u, 0x40840090u,
    0xe4e4e408u, 0x420400b0u, 0xe4aaaa4au, 0x67ca0000u, 0x80000010u, 0x0660180du,
    0x900d0018u, 0xe4e4aa01u, 0x53c10004u, 0xe400e40au, 0x57c60044u, 0xe455e409u,
    0x57c60045u, 0xe4e4ff04u, 0x57ca0053u, 0xe400e404u, 0x4bc60055u, 0xe455e404u,
    0x57c60055u, 0xe400ff22u, 0x53c10073u, 0xe4e4e404u, 0x4bca0042u, 0xe455ff22u,
    0x53c10073u, 0xe4e4e404u, 0x57ca0045u, 0xe400e402u, 0x004a0032u, 0xe455e402u,
    0x008a0035u, 0x80000020u, 0x06501e1eu, 0x901e001eu, 0xe4e4e408u, 0x150600d0u,
    0x80000040u, 0x0500291fu, 0x901f0029u, 0x5500e403u, 0x57d45cc1u, 0xe4e4e412u,
    0x47c2000cu, 0xe4e4e401u, 0x6bc11006u, 0xe4e4e401u, 0x6fc11007u, 0xe4e4e401u,
    0x4bc11004u, 0xe4e4e405u, 0x77c20000u, 0xe4e4e407u, 0x444600c5u, 0xe4e4e407u,
    0x448600d5u, 0xe4e4e407u, 0x450600e5u, 0xe4e4e404u, 0x43ca000du, 0xe4e4e401u,
    0x77c11008u, 0x80000080u, 0x00803533u, 0x90330035u, 0xe4e4e407u, 0x57ca0011u,
    0xe4e4e406u, 0x57c20005u, 0xe4e4e404u, 0x47ca0051u, 0x80000100u, 0x05103c36u,
    0x9036003cu, 0xe4e4e402u, 0x73c6404cu, 0xe4e4e404u, 0x57c680ebu, 0xe4e4e407u,
    0x5bc680c1u, 0xe4e4e403u, 0x4be682dau, 0xe4e4e412u, 0x47c2000cu, 0xe400e40au,
    0x5bc64036u, 0xe4e4e403u, 0x4bea0256u, 0x80000200u, 0x05105141u, 0x90410051u,
    0xe4e4e402u, 0x73c6404cu, 0xe4e4e442u, 0x57c9800bu, 0xe4e4e407u, 0x5fca0055u,
    0xe4e4e40au, 0x5fc65067u, 0xe4e4e406u, 0x5bc20007u, 0xe4e4e410u, 0x5fca0067u,
    0xe4e4e404u, 0x5bca0065u, 0xe4e4e407u, 0x5fc69017u, 0xe4e4e407u, 0x63ca0061u,
    0xe4e4e405u, 0x5fc20007u, 0xe400e40au, 0x5bc64038u, 0xe4e4e404u, 0x57ca007au,
    0xe4e4e403u, 0x4be682d5u, 0xe4e4e404u, 0x57ca007bu, 0xe4e4e404u, 0x57c680e5u,
    0xe4e4e412u, 0x47c2000cu, 0xe4e4e403u, 0x4bea0256u, 0x80000400u, 0x01106363u,
    0x90630063u, 0xe4e4c601u, 0x07c10004u, 0x80000800u, 0x05506865u, 0x90650068u,
    0xe4e4e401u, 0x03c00000u, 0xe455c604u, 0x07c40042u, 0xe4e4e401u, 0x0fc00003u,
    0xe4e4e401u, 0x13c00004u, 0x80001000u, 0x00806969u, 0x90690069u, 0xe4e4e401u,
    0x0fc00003u, 0x80002000u, 0x00806e6eu, 0x906e006eu, 0xe4e4e401u, 0x13c00004u,
    0x80004000u, 0x05507473u, 0x90730074u, 0xe4e4aa04u, 0x57c9009bu, 0xe4e4e433u,
    0x0a020005u, 0xa0100011u, 0x00000000u, 0x00000000u, 0x3f800000u, 0x3f800000u,
    0x00000000u, 0x00000000u, 0xbf800000u, 0x3f800000u, 0xa0530054u, 0x00000000u,
    0x00000000u, 0x3f800000u, 0x00000000u, 0x40e00000u, 0x3f808084u, 0x40e00000u,
    0x40e00000u, 0xa0660066u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
};
static const uint32_t *template_words(void) {
    static const uint32_t initial[0x68 / 4] = {
        0xe0000000u, 0xa8800000u, 0x0e000000u, 0xd6887610u,
        0x22220e80u, 0, 0, 0, 0, 0, 0,
        0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
        0x3f800000u, 0x3f800000u, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    return initial;
}
static int core_init(void) {
    t_init = USEC;
    mbx_diag[11] = CLK(0x68);
    mbx_diag[12] = CLK(0x58);
    int ok = pd_try();
    /* Check before and after reset; retry with longer settle times if the
     * ID still reads wrong (500 us is the stock delay argument 0x1f4). */
    ok = ok || identify();
    for (uint32_t i = 0, us = 500u; i < 3; ++i, us *= 4u) {
        reset(us, i ? 1u : 16u);
        if (identify()) { ok = 1; break; }
    }
    mbx.status = mbx_dbg.st[0] = CORE(0x12c);
    ck_st(MBX_CK_RESET, mbx.status);
    mbx_dbg.pa[0] = (uint32_t)pt;
    mbx_dbg.pa[1] = (uint32_t)pb;
    mbx_dbg.pa[2] = (uint32_t)mbx_param;
    mbx_dbg.pa[3] = (uint32_t)mbx_param_aux;
    for (uint32_t i = 0; i < 4; ++i) mbx_dbg.va[i] = MBX_VA(mbx_dbg.pa[i]);
    if (!ok) { capture_diag(); return 0; }
    CORE(0x85c) = 0x80u;
    CORE(0x134) = 0xffffu;
    /* RetailOS enable mask; no CPU IRQ handler is installed, so +0x12c
     * (raw status) is still polled. */
    CORE(0x130) = 0x8775u;
    init_buffers();
    /* PTEs are plain physical page addresses, no low-bit flags. */
    for (uint32_t i = 0; i < 8 * 1024; ++i)
        pt[i] = 0x08000000u + (i << 12);
    barrier();
    for (uint32_t i = 0; i < 8; ++i)
        CORE(0x1000 + 4 * i) = (uint32_t)(pt + i * 1024) & 0x7fffffffu;
    barrier();
    for (uint32_t i = 0; i < 8; ++i) mbx_dbg.root[i] = CORE(0x1000 + 4 * i);
    /* MMU control as RetailOS (0x08485118): clear bit 0 and poll, set bit 0
     * and poll. Bit 16 is the status; the stock wait is for it to clear.
     * The emulator mirrors the enable into bit 16 instead, so a set bit 16
     * after enabling is accepted as the fallback. */
    CORE(0x1020) &= ~1u;
    barrier();
    if (wait_until(&CORE(0x1020), 0x10000u, 0, WAIT_US) != WAIT_OK) {
        ++mbx.timeouts;
        mbx.status = CORE(0x1020);
        capture_diag();
        return 0;
    }
    CORE(0x1020) |= 1u;
    barrier();
    if (wait_until(&CORE(0x1020), 0x10000u, 0, WAIT_US) != WAIT_OK &&
        wait_any(&CORE(0x1020), 0x10000u, WAIT_US) != WAIT_OK) {
        ++mbx.timeouts;
        mbx.status = CORE(0x1020);
        capture_diag();
        return 0;
    }
    mbx_dbg.mmu = CORE(0x1020);
    if (!init_parameter_allocator()) {
        mbx.status = CORE(0x12c);
        capture_diag();
        return 0;
    }
    ctx_init();
    pfx_pending = 1;
    return 1;
}
int mbx_init(void) {
    ++mbx_fail.inits;
    return core_init();
}
/* Round 13: no RetailOS write returns pages between scenes (traces: only
 * the 0x0c/0x40 acks and the 810/81c select, which NanoOS does too), yet the
 * device died once the pool had been swept (R80 "used 64", scene 11), and a
 * re-init gave about 10 more scenes. So before the pool can run dry the
 * whole init path runs again (GPU idle: the last job was waited for),
 * exactly what m8_gpu_recover does after a failure. Not in DevMode. */
/* Round 14: the primary trigger is a plain scene count. The device died at
 * scene 10 and 11 (about 6.4 pages a scene, 64 pages), so a re-arm after
 * every MBX_REARM_EVERY completed scenes keeps the pool under ~40 pages
 * whatever the page scan sees; the scan estimate stays as a backstop for a
 * scene far larger than the world (a sampled page scan can under-count).
 * mbx_pw.rearm_n counts the scene-count re-arms, rearms all of them. */
/* Round 16 (VC mode 4, 224 segments, ~13+ pages a scene): six scenes no
 * longer fit, so the trigger adapts. Called before seg_n is reset, so seg_n
 * and d[n - 1] describe the scene just completed; the next one is assumed
 * at least as large (or dmax, if larger): re-arm once used + 1.5 * that + 2
 * would pass MBX_REARM_PAGES, and after MBX_REARM_BIG_EVERY scenes when the
 * last one had more than MBX_REARM_BIG_SEGS segments. Triangle/quad/preview
 * scenes keep the six-scene count. */
static int pool_low(void) {
    struct mbx_pool_watch *p = &mbx_pw;
    if ((mbx_variant & MBX_V_DIAG) || pfx_pending || !p->n) return 0;
    uint32_t d = p->d[(p->n - 1u) % 8u];
    if (d < p->dmax) d = p->dmax;
    if (p->n >= MBX_REARM_EVERY ||
        (seg_n > MBX_REARM_BIG_SEGS && p->n >= MBX_REARM_BIG_EVERY)) { ++p->rearm_n; return 1; }
    return p->used + d + d / 2u + 2u > MBX_REARM_PAGES;
}
/* RetailOS ISP list, all pairs before the +0x680 kick */
static void isp_list(const uint32_t *target, uint32_t w, uint32_t h, uint32_t stride) {
    CORE(0x6ec) = 0;
    CORE(0x608) = SCENE_VA;
    CORE(0x610) = 0x00030000u;
    CORE(0x614) = 0x1e3ce508u;
    CORE(0x618) = 0x1e3ce508u;
    CORE(0x61c) = 0x00020007u;
    CORE(0x63c) = background;
    CORE(0x644) = 0;
    CORE(0x648) = 1;
    CORE(0x64c) = 0;
    CORE(0x650) = 0xeu;
    CORE(0x654) = (w - 1) << 16;
    CORE(0x658) = (h - 1) << 16;
    CORE(0x65c) = MBX_VA(target);
    CORE(0x660) = stride;        /* pixels = bytes / 4 for ARGB8888 */
    CORE(0x674) = 0;
    CORE(0x6d0) = 1;
    CORE(0x6dc) = 0x3f800001u;
    CORE(0x860) = GRID;
    CORE(0x60c) = mbx_variant & MBX_V_60C ? MBX_VA(pb) + 0x3d00u : MBX_VA(mbx_param);
}
static const uint32_t *isp_target;
static uint32_t isp_w, isp_h, isp_stride;
void mbx_set_target(const uint32_t *target, uint32_t w, uint32_t h, uint32_t stride) {
    isp_target = target;
    isp_w = w;
    isp_h = h;
    isp_stride = stride;
}
static void submit_rec(const uint32_t *fifo, uint32_t words, uint32_t c, int bell);
static void fail(uint32_t reason);
__attribute__((noinline)) void mbx_begin(void) {
    int rearm_bad = 0;
    if (pool_low()) {
        ++mbx_pw.rearms;
        aux_evidence(1);
        rearm_bad = !core_init();
    }
    t_begin = t_scene = USEC;
    t_eol = 0;
    scene_failed = 0;
    fail_reason = 0;
    job_result = -1;
    scene_words = 0;
    ++mbx_fail.scene;
    mbx.ta_status = mbx.job_status = 0;
    scene_pre = CORE(0x12c);
    {   /* round 12 probe: new scene (ring, gap, clock gates at begin) */
        struct mbx_ta_probe *p = &mbx_probe;
        p->n = p->gap_max_us = p->gap_seg = 0;
        p->clk4c_b = CLK(0x4c);
        p->clk6c_b = CLK(0x6c);
#if MBX_PROBE
        trk_last = 0xffffffffu;
        t_sub = 0;
        trk(scene_pre);
#endif
    }
    if (rearm_bad) {             /* mbx.status: +0x12c or +0x1020 (mbx_init) */
        ++mbx.timeouts;
        fail(12);
        return;
    }
    CORE(0x134) = 0xffffu;
    init_buffers();
    if (mbx_variant & MBX_V_ISP && isp_target) {
        isp_list(isp_target, isp_w, isp_h, isp_stride);
        barrier();
    }
    /* first-launch trace #16..#229: prefix, wake, event 1 acknowledged;
     * later scenes (#734..#742) select context slot 0 again instead. The
     * allocator is built once (trace #1-10); rebuilding it per scene made
     * the world preview fail at frame 12 instead of ~223 (round 6). */
    if (pfx_pending) {
        pfx_pending = 0;
        submit_rec(mbx_prefix, MBX_PREFIX_WORDS, MBX_CK_PFX, 0);
    } else {
        wake();                  /* trace #726-733: two wakes before the */
        wake();                  /* context select of every later scene */
        ctx_init();
    }
    seg_n = pend_words = scene_bells = 0;
    /* No 0x100 after the slot select: suspected cause of the round-7 TA
     * timeout with +0x12c = 0 (not even the 0x8000 a good select leaves).
     * Fail now, without kicking the TA, so the report names it (reason 5).
     * DevMode (MBX_V_DIAG) keeps running on as before. */
    if (mbx_dbg.ctx_res != 1u && !(mbx_variant & MBX_V_DIAG)) {
        ++mbx.timeouts;
        mbx.status = mbx_dbg.ctx_st;
        fail(5);
        return;
    }
    CORE(0x12c) = 1;             /* wake, trace #739-742 */
    barrier();
    ck(MBX_CK_WAKE);
    ev_ack(1);
    trk(CORE(0x12c));            /* round 15: status after ctx + wake */
    uint32_t desired = 0x000400e1u |
        (((NX - 1) << 19) & 0x07f00000u) | ((NY - 1) << 10);
    desired &= ~0x00040000u;
    CORE(0x85c) = (desired & ~0x388u) | (CORE(0x85c) & 0x388u) | 0x80u;
    mbx_dbg.r85c = CORE(0x85c);
    CORE(0x840) = MBX_VA(pb);
    CORE(0x844) = SCENE_VA;
    CORE(0x84c) = 240u << 16;
    CORE(0x850) = 432u << 16;
    CORE(0x854) = 0x3727c5acu;
    if (mbx_variant & MBX_V_DIAG) mbx_dbg.rb_w[0] = CORE(0x854);
    CORE(0x858) = 0x3727c5acu;
    if (mbx_variant & MBX_V_DIAG) mbx_dbg.rb_w[1] = CORE(0x860);
    barrier();
    if (mbx_variant & MBX_V_DIAG) {
        static const uint16_t rb[15] = { 0x824, 0x828, 0x82c, 0x838, 0x83c, 0x6d8, 0x85c,
            0x840, 0x844, 0x84c, 0x850, 0x854, 0x858, 0x860, 0x1020 };
        static const uint16_t re[10] = { 0x608, 0x60c, 0x610, 0x63c, 0x650, 0x654, 0x658,
            0x65c, 0x660, 0x6dc };
        ck(MBX_CK_B);
        for (uint32_t i = 0; i < 15; ++i) mbx_dbg.rb_b[i] = CORE(rb[i]);
        for (uint32_t i = 0; i < 10; ++i) mbx_dbg.rb_e[i] = CORE(re[i]);
        mbx_dbg.ran |= 2u;
    }
    CORE(0x800) = 1;
    barrier();
    ck(MBX_CK_800);
    trk(CORE(0x12c));            /* round 15: status after the 800 kick */
}
/* failure snapshot (mbx_fail), taken once per failed scene */
static void snap_fail(uint32_t reason, uint32_t st) {
    struct mbx_fail_info *f = &mbx_fail;
    ++f->fails;
    f->reason = reason;
    f->fscene = f->scene;
    f->st = st;
    f->st_pre = scene_pre;
    f->ctx_res = mbx_dbg.ctx_res;
    f->ctx_st = mbx_dbg.ctx_st;
    f->ctx_us = mbx_dbg.ctx_us;
    f->words = scene_words;
    f->bells = scene_bells;
    f->sub_us = (t_eol ? t_eol : USEC) - t_begin;
    f->r1020 = CORE(0x1020);
    f->r828 = CORE(0x828);
    f->r82c = CORE(0x82c);
    f->r840 = CORE(0x840);
    f->r85c = CORE(0x85c);
    f->clk58 = CLK(0x58);
    f->clk68 = CLK(0x68);
    f->clk1c = CLK(0x1c);
    f->psc14 = PSC(0x14);
    f->pool_used = mbx_pool_pages_used();
    f->aux_links = mbx_aux_links();
    pool_mark();
    aux_evidence(2);
    f->isp_late_us = f->isp_late_st = 0;
    struct mbx_ta_probe *p = &mbx_probe;   /* round 12: reads only */
    p->clk4c_f = CLK(0x4c);
    p->clk6c_f = CLK(0x6c);
    p->id = CORE(0xf00);
    p->r130 = CORE(0x130);
    p->r824 = CORE(0x824);
    p->r838 = CORE(0x838);
    p->r83c = CORE(0x83c);
    p->r844 = CORE(0x844);
    for (uint32_t i = 0; i < 3; ++i) p->st3[i] = CORE(0x12c);
    p->late_us = p->late_st = 0;
    mbx_tafail = *p;
}
/* RSN 8 (round 12): keep reading +0x12c (no writes) MBX_TA_LATE_US more:
 * did the TA finish late (slow) or never (dead)? */
static void ta_late_watch(void) {
    uint32_t st = 0, t0 = USEC;
    mbx_tafail.late_us = MBX_NEVER;
    for (uint32_t spins = 0; spins < MBX_TA_LATE_US * 64u; ++spins) {
        st = CORE(0x12c);
        if (st & RENDER_DONE) { mbx_tafail.late_us = USEC - t_eol; break; }
        if (USEC - t0 >= MBX_TA_LATE_US) break;
    }
    mbx_tafail.late_st = st;
}
static void fail(uint32_t reason) {
    if (!fail_reason) {
        fail_reason = reason;
        fail_status = mbx.status;
        fail_us = USEC - t_scene;
        snap_fail(reason, mbx.status);
    }
    scene_failed = 1;
}
/* USEC is read only once the FIFO is found full (the timeout start) */
static int fifo_word(uint32_t word) {
    uint32_t start = 0;
    for (uint32_t spins = 0; spins < WAIT_US * 64u; ++spins) {
        mbx.status = CORE(0x12c);
        trk(mbx.status);
        if (mbx.status & OVERFLOW) { fail(2); return -1; }
        uint32_t occ = (mbx.status >> 16) & 0xffu;
        if (mbx_variant & MBX_V_DIAG && occ > mbx_dbg.fifo_max) mbx_dbg.fifo_max = occ;
        if (occ < 0x7bu) {
            TA_FIFO = word;
            return 0;
        }
        if (!spins) start = USEC;
        else if (USEC - start >= WAIT_US) break;
    }
    ++mbx.timeouts;
    fail(3);
    return -1;
}
/* MBX_V_DIAG: every FIFO0 word as stored */
static uint32_t fifo_log[MBX_LOG_WORDS];
/* returns the words stored */
static uint32_t submit(const uint32_t *fifo, uint32_t words, int bell) {
    uint32_t log = mbx_variant & MBX_V_DIAG, i;
    if (scene_failed) return 0;
#if MBX_PROBE
    if (t_sub && USEC - t_sub > mbx_probe.gap_max_us) {
        mbx_probe.gap_max_us = USEC - t_sub;
        mbx_probe.gap_seg = seg_n;
    }
#endif
    for (i = 0; i < words; ++i) {
        if (fifo_word(fifo[i])) { scene_words += i; return i; }
        if (log && mbx_dbg.log_n < MBX_LOG_WORDS) fifo_log[mbx_dbg.log_n++] = fifo[i];
    }
    scene_words += i;
    barrier();
    if (bell) { DOORBELL = 1; ++scene_bells; if (log) ++mbx_dbg.bells; }
    barrier();
#if MBX_PROBE
    t_sub = USEC;
#endif
    return i;
}
static void submit_rec(const uint32_t *fifo, uint32_t words, uint32_t c, int bell) {
    uint32_t s = mbx_dbg.nseg, rec = mbx_variant & MBX_V_DIAG && s < MBX_SEGS;
    if (rec) {
        mbx_dbg.nseg = s + 1u;
        mbx_dbg.seg_ck[s] = c;
        mbx_dbg.seg_words[s] = words;
        mbx_dbg.seg_log[s] = mbx_dbg.log_n;
    }
    uint32_t n = submit(fifo, words, bell);
    if (rec) mbx_dbg.seg_wr[s] = n;
    if (rec) ck(c);
}
void mbx_submit_seg(const uint32_t *fifo, uint32_t words, uint32_t c) { ++seg_n; submit_rec(fifo, words, c, 1); }
static void flush_bell(void) {
    if (!pend_words || scene_failed) return;
    pend_words = 0;
    barrier();
    DOORBELL = 1;
    ++scene_bells;
    if (mbx_variant & MBX_V_DIAG) ++mbx_dbg.bells;
    barrier();
}
void mbx_submit(const uint32_t *fifo, uint32_t words) {
    int bell = !MBX_BELL_BATCH || seg_n < BELL_SEGS;
    ++seg_n;
    if (bell) { submit_rec(fifo, words, MBX_CK_N, 1); return; }
    submit_rec(fifo, words, MBX_CK_N, 0);
    pend_words += words;
    if (pend_words >= BELL_BATCH) flush_bell();
}
const uint32_t *mbx_fifo_log(void) { return fifo_log; }
/* Overflow takes precedence over completion. Bound every poll even if the
 * microsecond timer stops. Acknowledge only after saving the final status. */
/* extra: bits allowed up to MBX_W8_US to follow a completion before the ack
 * (RetailOS acks 0x0c, then 0x40, after the ISP); a miss counts in no8.
 * Round 13: was 2 ms; the next scene's context select must not start while
 * the job may still be returning its pages. */
static int wait_status(uint32_t bits, uint32_t us, uint32_t *n, uint32_t *took,
                       uint32_t extra) {
    uint32_t start = USEC;
    uint32_t spins = 0;
    int r = -1;
    for (;;) {
        mbx.status = CORE(0x12c);
        trk(mbx.status);
        if (mbx.status & OVERFLOW) break;
        if ((mbx.status & bits) == bits) { r = 0; ++*n; break; }
        if (USEC - start >= us || ++spins / 64u >= us) {
            ++mbx.timeouts;
            break;
        }
    }
    *took = USEC - t_scene;
    if (!r && extra && (mbx.status & extra) != extra) {
        uint32_t t0 = USEC, w = MBX_NEVER;
        for (spins = 0; spins < MBX_W8_US * 64u && USEC - t0 < MBX_W8_US; ++spins)
            if ((CORE(0x12c) & extra) == extra) { w = USEC - t0; break; }
        if (w == MBX_NEVER) { ++mbx_fail.no8; ++mbx_pw.w8_miss; }
        else if (w > mbx_pw.w8_max) mbx_pw.w8_max = w;
    }
    /* job done: RetailOS's ISR acks 0x0c, then 0x40 (trace #723/#724) */
    if (extra) { CORE(0x134) = 0x0cu; CORE(0x134) = 0x40u; }
    CORE(0x134) = 0xffffu;
    barrier();
    return r;
}
/* A scene that failed before its TA wait reports on the ViceNano diag
 * lines: ta_status = raw +0x12c at the failure, done_us = us since
 * mbx_begin, job_status = 0xf0000000 | reason << 24 | doorbells << 16 |
 * segment (1-based, as counted when it failed). */
static int scene_fail(void) {
    mbx.ta_status = fail_status;
    mbx.done_us = fail_us;
    mbx.job_status = 0xf0000000u | (fail_reason & 0xfu) << 24 |
                     (scene_bells & 0xffu) << 16 | (seg_n & 0xffffu);
    return -1;
}
static int end_scene(void) {
    if (scene_failed) return scene_fail();
    const uint32_t eol = 0xf0000000u;
    flush_bell();                /* trace #692: last batch rung before EOL */
    ck(MBX_CK_PREEOL);
    uint32_t n = submit(&eol, 1, 0);
    if (mbx_variant & MBX_V_DIAG) { mbx_dbg.eol_wr = n; mbx_dbg.eol_bell = 0; }
    ck(MBX_CK_EOL);
    if (scene_failed) { fail_reason += 4; return scene_fail(); }
    /* trace #694-697/#1194: wake right after EOL, event 1 serviced */
    wake();
    t_eol = USEC;
    int r = wait_status(RENDER_DONE, WAIT_TA_US, &mbx.scenes, &mbx.done_us, 0);
    uint32_t ta = USEC - t_eol;
    mbx.ta_status = mbx.status;
    ck_st(MBX_CK_T, mbx.status);
    if (r) {
        snap_fail(mbx.status & OVERFLOW ? 10u : 8u, mbx.status);
        if (mbx_fail.reason == 8u) ta_late_watch();
    } else {
        if (ta > mbx_fail.ta_max_us) mbx_fail.ta_max_us = ta;
        mbx_probe.h838[mbx_probe.h_n++ % 8u] = CORE(0x838);   /* read only */
    }
    return r;
}
int mbx_empty_scene(void) {
    mbx_begin();
    return end_scene();
}
/* Region records 0 1 29 30 31 808 809: [0] after the TA (before the CPU
 * finalization below), [1] after the ISP. flags_diff counts word 0 that
 * differs from its CPU value (0x22000000, finalized: | 0x40000000, last
 * | 0xc0000000) outside the coordinate bits 0x1f1f; crd_diff word 0 whose
 * bits 0x1f1f are not (row << 8 | column); rec_w1 nonzero word 1. */
static void snap_regions(uint32_t k) {
    static const uint16_t idx[7] = { 0, 1, 29, 30, 31, 808, 809 };
    const uint32_t *r = pb + SCENE_OFFSET / 4;
    uint32_t d = 0, w1 = 0, b29 = 0, b31 = 0, crd = 0, i = 0;
    mbx_dbg.b31_idx[k] = mbx_dbg.w1_idx[k] = MBX_NONE;
    for (uint32_t y = 0; y < NY; ++y)
        for (uint32_t x = 0; x < NX; ++x, ++i) {
            uint32_t w = r[2 * i], v = r[2 * i + 1];
            uint32_t want = 0x22000000u | (k ? i == 809 ? 0xc0000000u : 0x40000000u :
                            i == 809 && mbx_variant & MBX_V_CRD ? 0x80000000u : 0);
            d += (w & ~0x1f1fu) != want;
            b29 += (w >> 29) & 1u;
            if (w >> 31 && !b31++) mbx_dbg.b31_idx[k] = i;
            crd += (w & 0x1f1fu) != (y << 8 | x);
            if (v && !w1++) { mbx_dbg.w1_idx[k] = i; mbx_dbg.w1_val[k] = v; }
        }
    mbx_dbg.flags_diff[k] = d;
    mbx_dbg.rec_w1[k] = w1;
    mbx_dbg.b29[k] = b29;
    mbx_dbg.b31[k] = b31;
    mbx_dbg.crd_diff[k] = crd;
    for (uint32_t j = 0; j < 7; ++j) {
        mbx_dbg.rec[k][j][0] = r[2 * idx[j]];
        mbx_dbg.rec[k][j][1] = r[2 * idx[j] + 1];
    }
}
/* RSN 9 (round 11): the ISP job did not end in 20 ms. Keep reading +0x12c
 * (no writes: the MMIO sequence is unchanged) to see whether it ends late. */
static void isp_late_watch(void) {
    uint32_t st = 0, t0 = USEC;
    mbx_fail.isp_late_us = MBX_NEVER;
    for (uint32_t spins = 0; spins < MBX_ISP_LATE_US * 64u; ++spins) {
        st = CORE(0x12c);
        if ((st & JOB_DONE) == JOB_DONE) { mbx_fail.isp_late_us = USEC - t_scene; break; }
        if (USEC - t0 >= MBX_ISP_LATE_US) break;
    }
    mbx_fail.isp_late_st = st;
}
const char *mbx_fail_text(uint32_t reason) {
    switch (reason) {
    case 2: return "GPU overflow (0x20) while sending";
    case 3: return "TA input FIFO stuck full 20 ms";
    case 5: return "context select never acked (0x100)";
    case 8: return "TA never finished (100 ms)";
    case 9: return "TA ok, ISP render never finished";
    case 10: return "overflow (0x20) while waiting TA";
    case 11: return "overflow (0x20) while waiting ISP";
    case 12: return "pool re-arm (GPU re-init) failed";
    default: return "unknown reason";
    }
}
int mbx_render(const uint32_t *target, uint32_t w, uint32_t h, uint32_t stride) {
    job_result = -1;
    int ta = end_scene();
    if (mbx_variant & MBX_V_DIAG) {
        snap_regions(0);
        snap_pool(1, template_words());
        snap_aux(1);
        snap_target(target, h * stride);
        snap_table(0);
        mbx_dbg.rb_t[0] = CORE(0x840);
        mbx_dbg.rb_t[1] = CORE(0x844);
    }
    if (ta) return -1;
    /* RetailOS finalizes the region records after TA completion and before
     * the ISP list is applied, not before the TA starts. */
    for (uint32_t i = 0; i < 810 && SCENE_OFFSET + 8 * i + 8 <= PB_BYTES; ++i)
        pb[(SCENE_OFFSET + 8 * i) / 4] |= i == 809 ? 0xc0000000u : 0x40000000u;
    barrier();
    isp_list(target, w, h, stride);
    mbx_dbg.r60c = CORE(0x60c);
    barrier();
    t_scene = USEC;
    CORE(0x680) = 1;             /* final pair: ordinary ISP launch */
    barrier();
    job_result = wait_status(JOB_DONE, WAIT_US, &mbx.jobs, &mbx.job_us, 0x08u);
    mbx.job_status = mbx.status;
    if (job_result) {
        snap_fail(mbx.status & OVERFLOW ? 11u : 9u, mbx.status);
        if (mbx_fail.reason == 9u) isp_late_watch();
    } else pool_scene();
    if (mbx_variant & MBX_V_DIAG) {
        ck_st(MBX_CK_I, mbx.status);
        snap_regions(1);
        snap_aux(2);
        snap_table(1);
        snap_pool(2, template_words());
        mbx_dbg.rb_i[0] = CORE(0x608);
        mbx_dbg.rb_i[1] = CORE(0x60c);
    }
    snap_alloc(1);
    return job_result;
}
int mbx_wait_done(uint32_t timeout_us) {
    /* mbx_render has already waited and acknowledged completion. Preserve
     * the existing render/wait API without waiting again or double counting. */
    (void)timeout_us;
    return job_result;
}
void mbx_set_background(uint32_t word) { background = word; }
uint32_t mbx_pb_word(uint32_t byte_off) { return pb[byte_off / 4]; }
uint32_t mbx_pb_used(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < PB_BYTES / 4; ++i) {
        uint32_t init = i * 4 >= SCENE_OFFSET && ((i * 4 - SCENE_OFFSET) & 7) == 0 &&
                        (i * 4 - SCENE_OFFSET) / 8 < 810 &&
                        i * 4 + 8 <= PB_BYTES ? 0x62000000u : 0;
        if (init && (i * 4 - SCENE_OFFSET) / 8 == 809) init = 0xe2000000u;
        n += pb[i] != init;
    }
    return n;
}
/* pool pages after the template page holding any nonzero word */
uint32_t mbx_pool_pages_used(void) {
    uint32_t n = 0, span = pool_span();
    for (uint32_t pg = 1; pg <= span; ++pg)
        n += nonzero(mbx_param + pg * 1024u, 1024u) != 0;
    return n;
}
uint32_t mbx_aux_links(void) {
    const volatile uint16_t *hw = (const volatile uint16_t *)mbx_param_aux;
    uint32_t n = 0;
    for (uint32_t i = 0; i < 0x4000u / 2u - 1u; ++i) n += hw[i] != i + 1u;
    return n;
}
int mbx_gpu_page(uint32_t a) {
    return (a - (uint32_t)pt < sizeof pt) || (a - (uint32_t)mbx_param < sizeof mbx_param) ||
           (a - (uint32_t)mbx_param_aux < sizeof mbx_param_aux);
}
uint32_t mbx_f32(int v) {
    uint32_t s = 0, m = (uint32_t)v;
    if (!v) return 0;
    if (v < 0) { s = 1u << 31; m = -m; }
    int e = 31 - __builtin_clz(m);  /* |v| < 2^24: exact */
    return s | (uint32_t)(127 + e) << 23 | ((m << (23 - e)) & 0x7fffffu);
}
