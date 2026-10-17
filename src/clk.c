#pragma GCC optimize("Os")   /* runs once per boot: size first */
#include "clk.h"
#include "timer.h"
#include "fmt.h"
#include "report.h"
/* CLKCON 0x3c500000 (RetailOS clock reader 0x0842a1c0, PLL reader
 * 0x083c9d8c, PLL setter IRAM 0x220007d8, all snap6):
 *   +0x00     system parent: source [13:12] (0 osc, 1..3 PLL-A/B/C), divisor
 *             [3:0]+1. Device: 0x40009001 = PLL-A/2 = 342 MHz.
 *   +0x04     children of +0x00, divisor 2 x (field+1) when enabled, else /1:
 *             CPU [29:25] en 30; buses [5:1] en 6, [21:17] en 22, [13:9]
 *             en 14. Device: 0x00404240 = CPU /1, buses /2 /2 /4.
 *   +0x20..28 PLL-A/B/C PMS: P [28:23], M [12:3], S [2:0],
 *             f = 24 MHz x M / (P x 2^S). Device PLL-A 0x03000558 = 684.
 *   +0x30..38 lock counters; +0x40 lock bits 0..2; +0x44 enable bits 0..2,
 *             output select bits 16..18.
 *   +0x08..+0x1c two domains each, source [29:28] and [13:12]; +0x64
 *             bytes 0x20 = PLL-A. +0x1c: GPU [31:16], memory [15:0].
 * The CPU = system parent: raising it means PLL-A/1 at a new PLL-A rate.
 * The rates are the firmware PLL table's own (IRAM 0x220037e4): SAFE 432
 * (P6 M108 S0), MAX 532 (P6 M133 S0). No voltage is touched.
 * Order (RetailOS mode set 0x22000e0e / +0x00 case 0x22000930): bus
 * dividers raised first so no bus exceeds its stock rate at the new parent
 * (432: 108/108/72, 532: 133/133/66.5 MHz against 171/171/85.5); parent to
 * PLL-B/2 (RetailOS's own DFS idle setting, 0x4000a001); PLL-A
 * reprogrammed exactly like 0x220007d8 (output off, PLL off, PMS, lock
 * count 0x1c20, both on, poll lock), here with a deadline; parent back to
 * PLL-A, source before divisor on the way up. Code runs from DRAM, which
 * is on PLL-B (+0x1c[13:12] = 2), and the 1 MHz timer counts the
 * oscillator, so neither moves.
 * Only the exact device state above is retuned (NanoEmu's PLL words read
 * as regfile fill: refused there), and only when no other domain is on
 * PLL-A. The GPU is the exception: the device hands over with it on
 * PLL-A/6 (R86, +0x1c = 0x10052003), so it is parked on PLL-B/3 first
 * (-11 if that fails) and gets its profile rate (or its old field after a
 * rollback) at the end. Stays at stock: clk_result -1 state differs,
 * -0x1NN domain at +0xNN on PLL-A (no write in either case; -284 is now
 * the memory half of +0x1c). Every write must read back
 * before the next step. A failed step rolls back to stock (restore():
 * PLL_STOCK relocked from PLL-B/2, then +0x00 and DIV_STOCK, each read
 * back); clk_result is then the failed step: -4 dividers did not land
 * (PLL-A untouched), -5 parent did not leave PLL-A (untouched), -2 PLL-A
 * write or lock failed, -6 parent did not return to PLL-A, -9 GPU field
 * did not land. After -2 -4 -5 -6 -9 -11 the GPU field is put back as
 * found. If the rollback itself fails,
 * clk_result is its code and R86 "cause" keeps the failed step: -7 parent
 * could not leave the retuned PLL-A, -3 stock PLL-A did not relock
 * (parked on PLL-B/2: 133 MHz CPU, buses /4 /4 /6..8), -8 parent or
 * dividers not restored, -10 GPU field not back (or, after -9 with the
 * CPU raised, the GPU could not be parked: CPU left raised). R86 flags these and
 * its "after" rows are the registers as found. */
#define CLK(o) (*(volatile uint32_t *)(0x3c500000u + (o)))
#define RPT_CLK_ID 86u   /* 80 M8, 81 NAND, 82 audio, 83 USB, 84 touch, 85 BT */
#define SYS_STOCK 0x1001u          /* +0x00 [13:12],[3:0]: PLL-A/2 */
#define DIV_STOCK 0x00404240u
#define PLL_STOCK 0x03000558u
#define GPU_RATE 0x3fff0000u
#define GPU_PARK 0x20020000u       /* +0x1c[29:16]: PLL-B/3 */
static const struct { uint32_t pms, div; } cpu_up[] = {
    {0x03000360u, 0x00424442u},    /* SAFE: PLL-A 432, buses /4 /4 /6 */
    {0x03000428u, 0x00424642u},    /* MAX: PLL-A 532, buses /4 /4 /8 */
};
int clk_result;
static int clk_cause;   /* the step that failed first (R86 "cause") */
uint32_t udiv(uint32_t n, uint32_t d) {
    uint32_t q = 0, r = 0;
    if (!d) return 0;
    for (int i = 31; i >= 0; --i) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= 1u << i; }
    }
    return q;
}
uint32_t clk_src_khz(unsigned src, int *nominal) {
    static const uint32_t stock[] = {684000, 266000, 486000};
    if (!src) return 24000;
    uint32_t v = CLK(0x1c + 4 * src);
    uint32_t p = (v >> 23) & 63, m = (v >> 3) & 1023, s = v & 7;
    /* Plausible PMS: reserved [22:13] clear, 24 MHz / P >= 1 MHz. */
    uint32_t khz = p && p <= 24 && !(v & 0x007fe000u) ? udiv(24000u * m, p) >> s : 0;
    if (khz >= 24000 && khz <= 1200000) return khz;
    *nominal = 1;
    return stock[src - 1];
}
uint32_t clk_cpu_div(void) {
    /* reserved bits 0 7 8 15 16 23 24 31 set = NanoEmu's regfile fill
     * (0xffc1c3c1 after RetailOS's writes): no CPU divider */
    uint32_t c = CLK(0x04);
    return ((CLK(0x00) & 15) + 1) *
           ((c & 0x40000000u) && !(c & 0x81818181u) ? 2 * ((c >> 25 & 31) + 1) : 1);
}
uint32_t clk_cpu_khz(int *nominal) {
    return udiv(clk_src_khz(CLK(0x00) >> 12 & 3, nominal), clk_cpu_div());
}
uint32_t clk_gpu_khz(int *nominal) {
    uint32_t g = CLK(0x1c);
    return udiv(clk_src_khz(g >> 28 & 3, nominal),
                ((g >> 16 & 15) + 1) * ((g >> 20 & 15) + 1));
}
/* write, then wait for the read-back like the firmware does; 1 = landed */
static int wr(unsigned o, uint32_t v) {
    CLK(o) = v;
    __asm__ volatile("dsb sy" ::: "memory");
    return wait_until(&CLK(o), ~0u, v, 100) == WAIT_OK;
}
/* 1 = every write landed and the PLL locked; stops at the first failure */
static int pll_a(uint32_t pms) {
    if (!wr(0x44, CLK(0x44) & ~0x10000u) || !wr(0x44, CLK(0x44) & ~1u) ||
        !wr(0x20, pms) || !wr(0x30, 0x1c20) || !wr(0x44, CLK(0x44) | 0x10001u))
        return 0;
    delay_us(400);       /* lock count 0x1c20 = 300 us at 24 MHz */
    return wait_any(&CLK(0x40), 1, 2000) == WAIT_OK;
}
/* Back to stock from wherever a sequence stopped, every write read back.
 * pll: PLL-A may differ from PLL_STOCK, so it is relocked first, with the
 * parent on PLL-B/2 (moved there from PLL-A/1 or /2: divisor, then source,
 * as the firmware clocks down). 0 = stock restored; -7 parent could not
 * leave PLL-A, -3 stock PLL-A did not relock (parent stays on PLL-B/2),
 * -8 parent or dividers not restored. */
static int gpu_set(uint32_t f) {
    return wr(0x1c, (CLK(0x1c) & ~GPU_RATE) | f);
}
static int restore(uint32_t sys, int pll) {
    if (pll) {
        if ((CLK(0x00) & 0x3000u) != 0x2000u &&
            !(wr(0x00, sys) && wr(0x00, sys ^ 0x3000u)))
            return -7;
        if (!pll_a(PLL_STOCK)) return -3;
    }
    return wr(0x00, sys) && wr(0x04, DIV_STOCK) ? 0 : -8;
}
static int raise_cpu(unsigned profile) {
    uint32_t sys = CLK(0x00);
    if ((sys & 0x300f) != SYS_STOCK || CLK(0x04) != DIV_STOCK ||
        CLK(0x20) != PLL_STOCK || (CLK(0x44) & 0x30003u) != 0x30003u ||
        (CLK(0x40) & 3) != 3)
        return -1;
    /* the GPU (+0x1c upper) is exempt: the device hands over on PLL-A/6
     * (0x1005xxxx, 114 MHz) and the GPU field is rewritten anyway */
    for (unsigned o = 0x08; o <= 0x1c; o += 4)
        if (((CLK(o) >> 28 & 3) == 1 && o != 0x1c) || (CLK(o) >> 12 & 3) == 1)
            return -(int)(0x100 + o);
    if ((CLK(0x64) & 0xff) == 0x20 || (CLK(0x64) >> 8 & 0xff) == 0x20) return -0x164;
    /* GPU off PLL-A before anything else: parked on PLL-B/3 (88.7 MHz, under
     * its 114 at handoff; the MBX is idle on the selector). -11: it did not
     * land, nothing else written (clk_apply puts the field back). */
    if ((CLK(0x1c) >> 28 & 3) == 1 && !gpu_set(GPU_PARK)) return -11;
    /* Every step must read back before the next: PLL-A is only touched
     * once the raised dividers are in and the parent has left it. */
    int r = -4, pll = 0;                     /* -4: dividers did not land */
    if (wr(0x04, cpu_up[profile - 1].div)) {
        r = -5;                              /* parent did not leave PLL-A */
        if (wr(0x00, sys ^ 0x3000u)) {       /* PLL-A/2 -> PLL-B/2 */
            pll = 1;
            r = -2;                          /* PLL-A write or lock failed */
            if (pll_a(cpu_up[profile - 1].pms)) {
                if (wr(0x00, sys) && wr(0x00, sys & ~15u))   /* PLL-A/2, /1 */
                    return 1;
                r = -6;                      /* parent did not return */
            }
        }
    }
    clk_cause = r;
    int e = restore(sys, pll);
    return e ? e : r;
}
/* R86 (report.h mailbox, exported by NanoOSLoader as R86.txt): profile,
 * result; CPU/GPU kHz; then before and after, 7 words a line (fmt's line
 * is 63 chars): +00 +04 +1c +20 +24 +28 +40 / +44 +08 +0c +10 +14 +18 +64 */
static const uint8_t regs[] = {0x00, 0x04, 0x1c, 0x20, 0x24, 0x28, 0x40, 0x44,
                               0x08, 0x0c, 0x10, 0x14, 0x18, 0x64};
/* result codes as bit sets (bit n = result -n): back at stock after a
 * failed step (-2 -4 -5 -6 -9 -11); recovery incomplete (-3 -7 -8 -10) */
#define BACK_AT_STOCK (1u << 2 | 1u << 4 | 1u << 5 | 1u << 6 | 1u << 9 | 1u << 11)
#define NOT_RESTORED (1u << 3 | 1u << 7 | 1u << 8 | 1u << 10)
static void words(const uint32_t *w) {
    for (unsigned i = 0; i < 14; ++i) {
        fmt_str(" "); fmt_hex(w[i], 8);
        if (i % 7 == 6) fmt_line(0, 0, 0);
    }
}
int clk_apply(unsigned profile, uint32_t gpu) {
    uint32_t w[2][14];
    for (unsigned i = 0; i < 14; ++i) w[0][i] = CLK(regs[i]);
    /* STOCK: no CLKCON write at all (the report below is RAM only) */
    clk_cause = 0;            /* raise_cpu sets it when a step failed */
    int r = profile ? raise_cpu(profile) : 0;
    if (!clk_cause) clk_cause = r;
    /* a GPU on PLL-A (SAFE: 684/4 = 171) follows the retune: /3 = 144 */
    if (r > 0 && gpu >> 28 == 1) gpu -= 0x10000u;
    /* GPU field: SAFE/MAX only, after a retune or a refusal that changed
     * nothing (-1, -0x1NN). If it does not land (-9) after a retune: GPU
     * parked off PLL-A, then the CPU back to stock (a GPU that cannot
     * leave PLL-A keeps the CPU raised: -10). */
    if (profile && (r > 0 || r == -1 || r < -0x100) && !gpu_set(gpu)) {
        clk_cause = -9;
        if (r < 0) r = -9;
        else if (!gpu_set(GPU_PARK)) r = -10;
        else { int c = restore(w[0][0], 1); r = c ? c : -9; }
    }
    /* back at stock after a failed step: the GPU field as found (it may
     * be parked); -10 if it does not land */
    if (r < 0 && -r < 32 && (BACK_AT_STOCK >> -r & 1) && !gpu_set(w[0][2] & GPU_RATE))
        r = -10;
    clk_result = r;
    for (unsigned i = 0; i < 14; ++i) w[1][i] = CLK(regs[i]);
    volatile struct rpt_hdr *h = RPT_HDR;
    unsigned s = 4;
    if (!rpt_valid()) {
        for (uint32_t i = 0; i < sizeof *h / 4; ++i) ((volatile uint32_t *)h)[i] = 0;
        h->slot_size = RPT_SLOT_SIZE;
    }
    while (--s && h->id[s] && h->id[s] != RPT_CLK_ID) ;
    if (h->id[s] && h->id[s] != RPT_CLK_ID) s = 3;
    char *p0 = fmt_out = RPT_SLOT(s);
    fmt_end = p0 + 1024;
    int n = 0;
    fmt_str("== CLK R86 == profile "); fmt_dec(profile);
    fmt_str(" result "); fmt_sdec(r);
    fmt_str(" cause "); fmt_sdec(clk_cause);
    fmt_line(0, 0, 0);
    /* -3 -7 -8 -10: recovery incomplete, the "after" rows are as found */
    if (r < 0 && -r < 32 && (NOT_RESTORED >> -r & 1)) {
        fmt_str("STOCK NOT RESTORED (after rows: as found)");
        fmt_line(0, 0, 0);
    }
    fmt_str("cpu "); fmt_dec(clk_cpu_khz(&n));
    fmt_str(" gpu "); fmt_dec(clk_gpu_khz(&n));
    fmt_str(" kHz nominal "); fmt_dec(n);
    fmt_line(0, 0, 0);
    words(w[0]);
    words(w[1]);
    fmt_str("== END R86 =="); fmt_line(0, 0, 0);
    uint32_t len = (uint32_t)(fmt_out - p0);
    fmt_out = 0;
    h->id[s] = RPT_CLK_ID;
    h->len[s] = len;
    h->sum[s] = rpt_sum(p0, len);
    h->magic[0] = RPT_MAGIC0;
    h->magic[1] = RPT_MAGIC1;
    h->hdr_sum = rpt_hdr_sum();
    return r;
}
