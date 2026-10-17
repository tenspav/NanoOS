#ifndef NANOOS_MBX_H
#define NANOOS_MBX_H
#include <stdint.h>
/* id: core +0xf00 (core ID); rev: +0xf10; sig: +0xff0..+0xffc bytes
 * (0xb105f00d on cores that have it); done_us: scene submit to render
 * done; scenes: completed; status: +0x12c at the end of the last one;
 * job_us: render job kick to done; ta_status/job_status: +0x12c when the
 * TA wait / render job wait of the last frame ended (0 = not reached) */
struct mbx_info { uint32_t id, rev, sig, done_us, scenes, timeouts, status,
                  jobs, job_us, ta_status, job_status; };
extern struct mbx_info mbx;
/* Counted since boot: inits = mbx_init() calls (1 = first session, more =
 * re-inits after failures or a new app session; pool re-arms count in
 * mbx_pw.rearms instead); scene = scenes begun; ta_max_us = slowest
 * TA (EOL to event 0x10); no8 = jobs whose 0x08 event did not follow 0x44
 * within MBX_W8_US. fail* = the LAST failed scene: reason (2 +0x12c 0x20 while
 * submitting, 3 FIFO full 20 ms, +4 on the EOL word, 5 context select (event
 * 0x100) timeout, 8 TA wait timeout, 9 ISP wait timeout, 10/11 0x20 during
 * the TA/ISP wait, 12 pool re-arm (mbx_init) failed), its scene number,
 * raw +0x12c at the failure and when the scene began (before its ack),
 * the context select result/status/us of that scene, FIFO words and
 * doorbells, submit us (begin to EOL), and readbacks taken right after:
 * +0x1020, +0x828, +0x82c, +0x840, +0x85c, CLKCON +0x58, +0x68, +0x1c,
 * PSC +0x14, pool pages ever written, aux links changed.
 * isp_late_us/isp_late_st (reason 9 only, round 11): after the 20 ms ISP
 * timeout +0x12c is watched (reads only) for MBX_ISP_LATE_US more; late_us
 * = us after the kick when both 0x44 bits showed (MBX_NEVER = never), st =
 * +0x12c when the watch ended. Tells a slow ISP from a dead one. */
struct mbx_fail_info {
    uint32_t inits, scene, ta_max_us, no8, fails,
             reason, fscene, st, st_pre, ctx_res, ctx_st, ctx_us, words, bells,
             sub_us, r1020, r828, r82c, r840, r85c, clk58, clk68, clk1c,
             psc14, pool_used, aux_links, isp_late_us, isp_late_st;
};
extern struct mbx_fail_info mbx_fail;
/* Round 12 TA-stall probe (RSN 8 world-only), reads only, no new MMIO
 * writes. Live per scene (reset by mbx_begin), copied to mbx_tafail by the
 * failure snapshot:
 *  - ring of the last MBX_TRK changes of +0x12c with the FIFO occupancy
 *    bits 16..23 masked (the status fifo_word/wait_status already read):
 *    segment, us since mbx_begin, status, input_service calls so far
 *    (svc); n = changes this scene. Shows when 0x8000 dropped and whether
 *    an input_service (PMU I2C, I2C1 clock-gate toggles) came just before.
 *  - gap_max_us/gap_seg: longest CPU gap between two submits in the scene.
 *  - clk4c/6c_b = CLKCON +0x4c/+0x6c at mbx_begin (I2C1 gate 0x27 lives
 *    there), *_f the same at the failure; id = +0xf00 (core alive?),
 *    r130/824/838/83c/844, st3 = +0x12c read three times.
 *  - late_us/late_st (reason 8 only): +0x12c watched MBX_TA_LATE_US more,
 *    us after the EOL when 0x10 showed (MBX_NEVER = never): slow vs dead.
 *  - h838: +0x838 after each of the last 8 completed TA waits (pool
 *    pointer drift = page leak), h_n = completed scenes counted. */
#define MBX_TRK 12u
#define MBX_TA_LATE_US 300000u
struct mbx_ta_probe {
    uint32_t n, seg[MBX_TRK], us[MBX_TRK], st[MBX_TRK], svc[MBX_TRK];
    uint32_t svc_now, gap_max_us, gap_seg, clk4c_b, clk6c_b, clk4c_f, clk6c_f,
             id, r130, r824, r838, r83c, r844, st3[3], late_us, late_st,
             h838[8], h_n;
};
extern struct mbx_ta_probe mbx_probe, mbx_tafail;
#define MBX_ISP_LATE_US 200000u
/* Round 13 parameter pool watch. R80's "used" (mbx_pool_pages_used) counts
 * pool pages holding ANY nonzero word and is only reset by the allocator
 * init: a high-water mark of pages ever written, not a live count. Here:
 * used = pages first written since the last allocator init, sampled after
 * every scene (16 head words + every 64th word of each page not yet seen,
 * reads only); d/r838/r820 = per completed scene (last 8, n = scenes since
 * the init): pages newly written, +0x838, +0x820; dmax = largest d ever.
 * rearms: mbx_begin re-runs the whole mbx_init path (reset, MMU, allocator,
 * context, prefix: the proven m8_gpu_recover path) BEFORE a scene when
 * used + dmax + dmax / 2 + 2 > pool pages, so a page leak can no longer
 * empty the pool. a_*: aux free-list evidence at the last re-arm or failure
 * (a_why 1 re-arm, 2 failure): used, n, halfwords != index + 1 (a_links)
 * and the first 4 (a_idx/a_val), a_end = aux[82c page] (82c + 1 = the list
 * never wrapped; the 828 page = pages came back in FIFO order). w8_max =
 * slowest 0x08 after JOB_DONE 0x44 (us), w8_miss = never within MBX_W8_US. */
#define MBX_W8_US 10000u
/* round 14: re-arm after this many completed scenes (mbx.c pool_low) */
#ifndef MBX_REARM_EVERY
#define MBX_REARM_EVERY 6u
#endif
/* round 16 (mbx.c pool_low): projected-page limit, and the scene count for
 * scenes of more than MBX_REARM_BIG_SEGS segments (VC mode 4) */
#ifndef MBX_REARM_PAGES
#define MBX_REARM_PAGES 60u
#endif
#define MBX_REARM_BIG_SEGS 150u
#define MBX_REARM_BIG_EVERY 3u
/* R80 build tag: a report without it came from an older kernel */
#define MBX_BUILD_TAG "r15"
/* round 15: 0 = one doorbell per mbx_submit segment (default), 1 = the
 * round-6..14 batching (3 single bells, then one per >= 512 words) */
#ifndef MBX_BELL_BATCH
#define MBX_BELL_BATCH 0
#endif
struct mbx_pool_watch {
    uint32_t arms, rearms, used, n, d[8], r838[8], r820[8], dmax, a_why,
             a_used, a_n, a_links, a_idx[4], a_val[4], a_end, w8_max, w8_miss,
             rearm_n;   /* round 14: re-arms by scene count */
};
extern struct mbx_pool_watch mbx_pw;
#define MBX_NEVER 0xffffffffu
/* mbx_fail.reason in plain words, for the failure screen and R80 */
const char *mbx_fail_text(uint32_t reason);
/* aux free-list halfwords that differ from the post-init pattern n + 1 */
uint32_t mbx_aux_links(void);
#define MBX_DIAG_N 26
extern uint32_t mbx_diag[MBX_DIAG_N]; /* clock/power regs captured on mbx_init failure */
/* GPU virtual address of DRAM: mbx_init() maps GPU VA 0..32 MB onto
 * physical 0x08000000.. (the MBX MMU has 8 tables of 4 MB) */
#define MBX_VA(p) ((uint32_t)(p) - 0x08000000u)
int mbx_init(void);       /* 1 if core ID and MMU completion are valid
                           * (on failure, status holds +0x12c or +0x1020);
                           * also resets/configures the core, PB and GPU MMU */
int mbx_empty_scene(void);/* submit an empty scene, wait; 0 ok, -1 timeout/overflow */
/* A frame: mbx_begin(), mbx_submit() per segment (TA stream, see mbx.c),
 * mbx_render() into an ARGB8888 target, mbx_wait_done().
 * TA scenes use the native 240x432 panel tile/clip setup. Render w/h are
 * the actual target dimensions; stride is in pixels (normally 240/432/240
 * for a native target, or 432/240/432 for a landscape target).
 * Parameter/region memory is owned by the driver in .bss.
 * mbx_render waits for completion; mbx_wait_done returns its saved result. */
void mbx_begin(void);
void mbx_submit(const uint32_t *fifo, uint32_t words);
/* mbx_submit() recording checkpoint ck (MBX_CK_S0..TRI) under MBX_V_DIAG */
void mbx_submit_seg(const uint32_t *fifo, uint32_t words, uint32_t ck);
int mbx_render(const uint32_t *target, uint32_t w, uint32_t h, uint32_t stride);
int mbx_wait_done(uint32_t timeout_us); /* 0 ok, -1 timeout/overflow */
uint32_t mbx_f32(int v);  /* IEEE single of a small int (no libgcc) */
/* ISP background (+0x63c, written as given; byte order unproven) for
 * tiles no object covers. Default 0. Bring-up: a distinct colour shows
 * whether the ISP wrote the target while all geometry was rejected. */
void mbx_set_background(uint32_t word);
/* MBX_V_ISP: the target mbx_begin() preloads into the ISP registers */
void mbx_set_target(const uint32_t *target, uint32_t w, uint32_t h, uint32_t stride);
/* FIFO0 startup prefix (first-launch trace #16..#225) streamed, no
 * doorbell, by the first mbx_begin() after mbx_init() */
#define MBX_PREFIX_WORDS 210u
extern const uint32_t mbx_prefix[MBX_PREFIX_WORDS];
/* Bring-up variants, read by mbx_init() and every frame; 0 = the
 * ordinary path. Hardware tests only (mbx_test.c). */
/* Round 5 winners, now always on (kept as 0 for old case tables): 85c
 * bit 18 clear, no EOL doorbell, context slot 0 init, startup prefix +
 * +0x12c = 1 wake, +0x860 only in the ISP list. */
#define MBX_V_85C 0u
#define MBX_V_NOBELL 0u
#define MBX_V_CTX 0u
#define MBX_V_PFX 0u
#define MBX_V_860 0u
#define MBX_V_60C 2u     /* +0x60c = scene buffer end, VA(pb) + 0x3d00 */
#define MBX_V_POOL 4u    /* 256-page pool, +0x82c one past the last page */
#define MBX_V_POOL1 0x10u /* one usable pool page: +0x82c = +0x828 */
#define MBX_V_ISP 0x40u  /* ISP list (all but 680) also before +0x800,
                          * for the mbx_set_target() target */
#define MBX_V_CRD 0x100u /* region seeds 0x22000000 | y << 8 | x, last
                          * | 0x80000000 (RetailOS 0x080b7ce2) */
#define MBX_V_DIAG 0x80u /* fill mbx_dbg pool/region snapshots (slow) */
extern uint32_t mbx_variant;
/* Captured by mbx_init()/mbx_render(); all words, in display order.
 * st: +0x12c after reset, after the event 0x40 ack, after each allocator
 * store 824 828 82c 838 83c 6d8. alloc: those six read back after the
 * init wait / after the ISP. pa/va: pt pb param aux. pool_nz/aux_nz/
 * tpl_diff: [0] after init, [1] after the TA (pool pages 1..pages+1,
 * page 0 against its template); nz_off/nz_word: first nonzero pool word
 * after the TA. Region records (snap_regions() in mbx.c) and aux changes
 * are [0] after the TA, before the CPU finalization, [1] after the ISP.
 * aux_a: aux[0..7] and aux[4088..4095] after the init wait; aux_chg/idx/
 * old/new: words changed against the previous stage, the first of them.
 * tgt_*: target words != 0xff00ff00 after the TA, before the ISP launch.
 * Indices are MBX_NONE when nothing matched.
 * Round 3 (MBX_V_DIAG only), stages A = allocator completion, B = just
 * before +0x800, T = after the TA wait, I = after the ISP; ran: bit per
 * stage snapshotted (1 A, 2 B, 4 T, 8 I), others are NOT_RUN.
 * ck_st/ck_us/ck_mask: raw +0x12c and microseconds since mbx_init() entry
 * at each MBX_CK_* checkpoint, bit set when reached (T/I: the status the
 * wait ended on). fifo_max: highest (+0x12c >> 16) & 0xff seen while
 * polling before FIFO0 stores. seg_*: per mbx_submit_seg() call, its
 * checkpoint, requested/stored words and offset into mbx_fifo_log();
 * log_n: FIFO0 words logged (EOL included); bells: FIFO2 stores;
 * eol_wr/eol_bell: EOL stored, doorbell after it. first/last_page: the
 * +0x828/+0x82c values written. rb_b: 824 828 82c 838 83c 6d8 85c 840
 * 844 84c 850 854 858 860 1020 at B; rb_t: 840 844 at T; rb_i: 608 60c
 * at I. tbl_*: [0] T [1] I, the 0xfc0-byte +0x840 table: nonzero words,
 * first 8 (byte offset, value), CRC32; tbl_gap: nonzero words in
 * [pb+0xfc0, pb+0x1004). p*: pool [0] A [1] T [2] I over pages 1..span
 * (span = max(pages, 16) + 1): nonzero words, template page diffs, page
 * mask (bit 0 template page, bit n pool page n, n >= 31 in bit 31),
 * first 8 nonzero (byte offset from the pool base, value).
 * Round 4: ctx_res 0 not run, 1 event 0x100, 2 timeout (CTX TO);
 * ctx_pre/ctx_post: 12c 810 81c 820 864 before the 134 ack / after the
 * final ack; ctx_st: +0x12c the wait ended on; ctx_us: 81c=1 to the end
 * of the wait. rb_w: 854 860 read right after their mbx_begin() writes;
 * rb_e: 608 60c 610 63c 650 654 658 65c 660 6dc at B.
 * Round 5: PFX after the prefix stores, WAKE right after +0x12c = 1; the
 * prefix is recorded as a segment (no doorbell). */
#define MBX_NONE 0xffffffffu
enum { MBX_CK_RESET, MBX_CK_A, MBX_CK_CTX, MBX_CK_PFX, MBX_CK_WAKE, MBX_CK_B, MBX_CK_800, MBX_CK_S0, MBX_CK_S1,
       MBX_CK_S2, MBX_CK_S3, MBX_CK_TRI, MBX_CK_PREEOL, MBX_CK_EOL, MBX_CK_T,
       MBX_CK_I, MBX_CK_N };
#define MBX_SEGS 8u
#define MBX_LOG_WORDS 1024u
struct mbx_dbg {
    uint32_t st[8], alloc[2][6], r85c, r60c, mmu, root[8], pa[4], va[4], pages,
             pool_nz[2], aux_nz[2], tpl_diff[2], nz_off, nz_word,
             flags_diff[2], rec_w1[2], rec[2][7][2],
             b29[2], b31[2], b31_idx[2], crd_diff[2], w1_idx[2], w1_val[2],
             aux_a[16], aux_chg[2], aux_idx[2], aux_old[2], aux_new[2],
             tgt_chg, tgt_idx, tgt_val,
             ran, ck_st[MBX_CK_N], ck_us[MBX_CK_N], ck_mask, fifo_max,
             nseg, seg_ck[MBX_SEGS], seg_words[MBX_SEGS], seg_wr[MBX_SEGS],
             seg_log[MBX_SEGS], log_n, bells, eol_wr, eol_bell,
             first_page, last_page, rb_b[15], rb_t[2], rb_i[2],
             tbl_nz[2], tbl_off[2][8], tbl_val[2][8], tbl_hash[2], tbl_gap[2],
             pnz[3], ptpl[3], pmask[3], poff[3][8], pval[3][8],
             ctx_res, ctx_st, ctx_us, ctx_pre[5], ctx_post[5], rb_w[2], rb_e[10];
};
extern struct mbx_dbg mbx_dbg;
/* Diagnostic: PB words that differ from their mbx_begin() pattern, i.e.
 * written by the TA (or ISP) since the scene began. */
uint32_t mbx_pb_word(uint32_t byte_off);  /* raw PB word, for diagnostics */
uint32_t mbx_pb_used(void);
/* parameter pool pages (after the template page) holding any nonzero word */
uint32_t mbx_pool_pages_used(void);
int mbx_gpu_page(uint32_t addr); /* inside pt, param pool or aux */
/* MBX_V_DIAG: FIFO0 words as stored, aux copies [0] A [1] T [2] I */
const uint32_t *mbx_fifo_log(void);
const uint32_t *mbx_aux_copy(uint32_t k);
uint32_t mbx_crc32(const uint32_t *w, uint32_t n); /* IEEE, LE words */
/* DevMode GPU page (mbx_test.c) */
void mbx_test_enter(void);
void mbx_test_draw(void);
void mbx_test_key(unsigned keys);  /* Vol+/Vol- (BUTTON_RIGHT/LEFT): variant */
#endif
