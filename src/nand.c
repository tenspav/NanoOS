#pragma GCC optimize("Os")   /* register pokes, no hot path: size first */
#include "nand.h"
#include "timer.h"
#include "fmt.h"
#include "report.h"
#include "clk.h"
#include "mmu.h"
#include "memmap.h"
/* FMC 0x38a00000, PPN ("managed") NAND, CPU register path. Every sequence
 * is RetailOS's own (snap6 disassembly), not the NanoEmu shortcut:
 *   command  +0x08 = cmd (issued on the write), wait +0x48 bit 1 (0x0840b1fc)
 *   address  +0x0c/+0x10 bytes LSB first, +0x2c = n - 1, +0x04 = 1, wait
 *            bit 2 (0x0810157c)
 *   data in  +0x30 = len - 1, +0x14 = 1, +0x24 = 0, +0x04 = 0x22, wait bit 3;
 *            PIO: +0x30 = 3, +0x14 = 0x100, +0x00 &= ~(bit 10 | bit 24),
 *            +0x28 = 0, +0x04 = 0x1e0, pop +0x80 until +0x28's low byte
 *            counts the word, +0x48 = bit 20, +0x04 = 0x20; <= 1 KB per
 *            call (0x084095f0, 0x080d33fc)
 *   status   +0x48 = bit 23, +0x14 = 1, +0x04 = 0xea, wait bit 23 (then
 *            +0x04 = 0x20), +0x4c = status byte, +0x48 = bit 24 (0x0840c3e8;
 *            +0x14 = 1 from the PPN wait 0x084d1e5c, round 10)
 *   waits    W1C the bit once set; 20 ms (0x081810b8)
 * Chip select: +0x00 = timing | (1 << ce) << 1 | 1, 1 = none. READ ID
 * (0x084d13d8) uses the runtime timing [0x0896e8e0 + 0x24] = 0x20010000;
 * reset (0x08010b4c) uses 0xff000 (slowest). Round 10: page reads always
 * use the runtime timing RUN_TM (snap6's value at this clock; a proper
 * choice is RetailOS's table 0x080fb710), the slow one is for reset and
 * probe only. The old code read +0x80
 * without a data phase: the FIFO was empty, hence "id 00000000".
 * Clock: RetailOS never gates the FMC (no gate call in its NAND code), so
 * the probe ungates nothing; only when +0x00 does not latch even after the
 * controller reset, the bootrom's +0x1c clock setup and the domain-6 check
 * may recover() clear candidate CLKCON gate bits as a last resort (round
 * 13/14). The report logs the CLKCON gate words.
 * Page read (round 15) = RetailOS's standalone CPU read 0x084ed600: +0x38
 * = 7, select, +0x48 = 0x0ff00ffe, +0x81c = 1, 0x0a + 4 address bytes
 * (+0x0c = physical page, +0x10 not written), 0x37, status (0x77 0x7d
 * poll; bit 4 is recorded, not an error), 0x7a, then 16 B meta + 4 x 1 KB
 * data by PIO; after the first probe, SET FEATURES power state 2
 * (set_power()). (Rounds 1-14 used the FMSS program's 0x07 + 8-byte form,
 * which the device answered with status 0x50.)
 *
 * No FTL: in NanoEmu raw reads come back 0xff ("blank"; the disk image is
 * served only to RetailOS's FMSS/FTL path). RetailOS's sftl (metadata
 * type 1 = LBA) is the next, read-only step.
 * Round 18b (nandscan4) adds data_in_retailos()/nand_read_phase(): RetailOS's
 * page-reader data phase writes +0x48 = 0x08000000 after 0x7a and +0x18 = 1
 * before arming, which the historical data_in() never did (see the flags and
 * variants at data_in_variant()/nand_read_phase()). Variant 0 is the old
 * phase byte-for-byte; only the arming differs between variants. */
#define FMC(o) (*(volatile uint32_t *)(0x38a00000u + (o)))
#define CLK(o) (*(volatile uint32_t *)(0x3c500000u + (o)))
#define WAIT_US 20000u
#define POLL 0x800000u
#define RPT_NAND_ID 81u
#define RUN_TM 0x20010000u
struct nand_info nand;
/* PASSIVE MODE (game path, nandscan). When set, recover() returns right
 * after its first latch check (ctl_init is the controller's own reset, the
 * known-safe step): the CLKCON +0x1c write, the PSC domain-6 wake and the
 * CLKCON gate clearing never run, so no clock/power-domain state is changed.
 * The passive path still touches exactly TWO pieces of hardware state:
 *  (1) nand_pins(): GPIO function-select writes for the NAND bus pads (the
 *      bootrom's own values, idempotent);
 *  (2) set_power(): one PPN SET FEATURES power-state command (not an array
 *      access; RetailOS issues the same one at its NAND init).
 * Default 0 = DevMode drivers page behaviour, byte-identical. */
volatile int nand_passive;
/* first failure of a probe or page read: the wait's +0x48 bit (2 cmd, 4
 * addr, 8 data arm, 0x100 PIO stall, POLL status) or 0x01 (page read,
 * round 15: status byte bit 0, fail; bit 4 no longer fails a read), then
 * FMC +00 +14 +18 +24 +28 +30 +38 +3c +48 +4c as read right after it
 * expired (0x01: right after the status poll, before 0x7a) */
static const uint8_t pf_at[10] = {0x00, 0x14, 0x18, 0x24, 0x28, 0x30, 0x38, 0x3c, 0x48, 0x4c};
static void failed(uint32_t m) {
    ++nand.errors;
    if (nand.ph) return;
    nand.ph = m;
    for (unsigned i = 0; i < 10; ++i) nand.pf[i] = FMC(pf_at[i]);
}
/* round 11: an expired status poll is still ended (+04 = 0x20) and its
 * bit acked, as RetailOS's 084d1e5c does on its failure exit too (the
 * deselect alone does not end it); other phases return at once */
static int wt(uint32_t m) {
    int r = 0;
    if (wait_any(&FMC(0x48), m, WAIT_US) != WAIT_OK) {
        failed(m);
        if (m != POLL) return -1;
        r = -1;
    }
    if (m == POLL) FMC(0x04) = 0x20;
    FMC(0x48) = m;
    return r;
}
static int cmd(unsigned c) { FMC(0x08) = c; return wt(2); }
/* round 15: up to 4 address bytes from +0x0c, +0x10 not written
 * (RetailOS 0x0840944c, its page read and SET FEATURES) */
static int addr_lo(unsigned n, uint32_t a0) {
    FMC(0x0c) = a0;
    FMC(0x2c) = n - 1;
    FMC(0x04) = 1;
    return wt(4);
}
static int addr(unsigned n, uint32_t a0, uint32_t a1) {
    FMC(0x10) = a1;
    return addr_lo(n, a0);
}
static int poll(void) {
    FMC(0x48) = POLL;
    FMC(0x14) = 1;      /* a PIO data phase left 0x100 here */
    FMC(0x04) = 0xea;
    int r = wt(POLL);
    nand.st = FMC(0x4c) & 0xff;
    FMC(0x48) = 0x1000000u;
    return r;
}
/* Round 18b / nandscan4: the data phase arming has two extra writes that
 * RetailOS's own page reader (0x084ed600) performs and this driver never
 * did. They are selected by flags here so variant 0 is byte-for-byte the
 * old data_in() and the PIO drain/end-of-phase code below is IDENTICAL for
 * every variant - only the arming differs:
 *   DP_PRE48  FMC(0x48) = 0x08000000 immediately after 0x7a, before arming
 *             (RetailOS's write; bit 27, W1C on +0x48)
 *   DP_W18    FMC(0x18) = 1 written just before +0x30/+0x14/+0x24/+0x04
 *             (RetailOS's non-ECC transfer setup)
 * data_in_retailos() = DP_PRE48|DP_W18 is RetailOS's non-ECC data phase. */
#define DP_PRE48 1u
#define DP_W18   2u
/* Round 19 / nandscan5: the PIO drain loop, factored out so EVERY data phase
 * (today's, RetailOS's +0x18 arming and the ECC-style 0x34 arming) drains
 * through byte-identical code. Only the arming writes differ between them,
 * which is what makes a shape comparison meaningful. Every wait bounded, no
 * flash write: buf's words are popped from +0x80 after the phase is armed. */
/* Round 21 / nandscan7: RetailOS's drain helper 0x080d33fc computes
 * +0x14 = 1 << (arg + 8), i.e. the PIO beat/parity selector is a CALLER
 * argument there, not the fixed 0x100 this driver has always written. The
 * value is factored out so the +0x14 hypothesis can be probed while the drain
 * loop, the end-of-phase ack and every bounded wait stay byte-identical. */
/* Round 22 / nandscan8: the drain BODY (mask, PIO counter reset, pop loop and
 * end-of-phase ack) is factored out of pio_drain14 so nandscan8's Part B E6
 * variant can run "one armed phase then the PIO loop" WITHOUT the +0x30 = 3
 * re-arm. For every pre-existing caller pio_drain14() below writes +0x30 = 3
 * and +0x14 = v14 and then calls this, so the emitted register sequence is
 * byte-identical to the old single function. */
/* Round 23 / nandscan9: words the LAST pio_body() call actually delivered, i.e.
 * words committed to the caller's buffer. nand_read_single() exports this as
 * nand_single_got so a full-page drain can report how far it got.
 *
 * Historical note: nandscan10 changed the shared drain to a pre-pop gate
 * based on an external document. Direct inspection of the saved RetailOS
 * image now confirms the ORIGINAL post-pop gate at 0x080d3432..0x080d3456.
 * The shared path is restored below; pre-pop modes remain isolated diagnostic
 * variants. Matching a counter never establishes valid NAND content.
 * FIFOCTR's low byte aliases every 64 words; hardware trace run35 also shows
 * a full counter wrap at 1024 bytes. Neither implies a physical data beat.
 * pio_words counts retained words, pio_popped all FIFO pops, and
 * pio_confirmed successful counter comparisons. */
#define PIO_GATE_FIXED  0u   /* historical name: UNPROVEN pre-pop experiment */
#define PIO_GATE_NONE   1u   /* pop unconditionally, no counter comparison */
#define PIO_GATE_FULL32 2u   /* sample before pop, full 32-bit == 4*n */
#define PIO_GATE_OLD    3u   /* historical: pop, then low byte == 4*n+4 */
static uint32_t pio_words, pio_popped, pio_confirmed;

/* ---- FIFOCTR trace (MEASUREMENT ONLY; changes no behaviour) --------------
 * run34 left one question open: what does FMC(+0x28) actually do at word 256?
 * Nothing in the driver ever recorded its value, so "1 KiB period" was an
 * inference and is withdrawn. This trace records exactly what the OLD gate
 * samples, so the question can be answered from data.
 *
 * Enabled only by the fixture, and only for the FIRST read of a fresh boot.
 * When nand_trace_on == 0 the loop below is byte-for-byte the original. */
#define NAND_TRACE_FULL   320u   /* first 320 iterations stored in full */
#define NAND_TRACE_MAX    384u   /* hard cap on stored entries */
#define NAND_TRACE_STRIDE 4096u  /* after the full window, one sample per stride */
struct nand_trace_ent { uint32_t i, ctr, word, ok; };
static struct nand_trace_ent nand_trace[NAND_TRACE_MAX];
static volatile uint32_t nand_trace_n;
static volatile int nand_trace_on;
/* aggregates: iters matches mismatches first_bad last_bad min_c max_c noadv entries */
static volatile uint32_t nand_trace_agg[9];

static void nand_trace_reset(void) {
    nand_trace_n = 0;
    for (unsigned k = 0; k < 9; ++k) nand_trace_agg[k] = 0;
    nand_trace_agg[3] = 0xffffffffu;   /* first_bad = none */
    nand_trace_agg[4] = 0xffffffffu;   /* last_bad  = none */
    nand_trace_agg[5] = 0xffffffffu;   /* min_c     = none */
    nand_trace_agg[6] = 0;             /* max_c     = none */
}
static void nand_trace_rec(uint32_t i, uint32_t ctr, uint32_t word, int ok) {
    uint32_t n = nand_trace_n;
    uint32_t prev_min = nand_trace_agg[5], prev_max_seen = nand_trace_agg[6];
    ++nand_trace_agg[0];                     /* iters */
    if (ok) ++nand_trace_agg[1]; else {
        ++nand_trace_agg[2];
        if (nand_trace_agg[3] == 0xffffffffu) nand_trace_agg[3] = i;
        nand_trace_agg[4] = i;
    }
    if (nand_trace_agg[0] == 1u) { nand_trace_agg[5] = ctr; nand_trace_agg[6] = ctr; nand_trace_agg[7] = 0; }
    else {
        if (ctr < prev_min) nand_trace_agg[5] = ctr;
        if (ctr > prev_max_seen) nand_trace_agg[6] = ctr;
        if (ctr == nand_trace[n ? n - 1u : 0u].ctr) ++nand_trace_agg[7];  /* did not advance */
    }
    if (n < NAND_TRACE_MAX && (i < NAND_TRACE_FULL || (i % NAND_TRACE_STRIDE) == 0u)) {
        nand_trace[n].i = i; nand_trace[n].ctr = ctr; nand_trace[n].word = word;
        nand_trace[n].ok = ok ? 1u : 0u;
        nand_trace_n = n + 1u;
    }
    nand_trace_agg[8] = nand_trace_n;
}
int nand_trace_enable(int on) { nand_passive = 1; nand_trace_on = on ? 1 : 0; nand_trace_reset(); return 0; }
uint32_t nand_trace_fetch(uint32_t *dst, uint32_t max_entries) {
    uint32_t n = nand_trace_n, k;
    if (!dst) return 0;
    if (n > max_entries) n = max_entries;
    for (k = 0; k < n; ++k) {
        dst[k * 4u + 0u] = nand_trace[k].i;
        dst[k * 4u + 1u] = nand_trace[k].ctr;
        dst[k * 4u + 2u] = nand_trace[k].word;
        dst[k * 4u + 3u] = nand_trace[k].ok;
    }
    return n;
}
void nand_trace_stats(uint32_t out[9]) {
    unsigned k;
    if (!out) return;
    for (k = 0; k < 9; ++k) out[k] = nand_trace_agg[k];
    out[8] = nand_trace_n;
}
static int pio_drain_mode(uint32_t *buf, unsigned len, unsigned mode) {
    int r = 0;
    unsigned i = 0, stall = 0;
    unsigned n = len / 4u;
    pio_words = 0;
    pio_popped = 0;
    pio_confirmed = 0;
    FMC(0x00) &= 0xfefffbffu;          /* bits 10, 24 (0x080d341a pool) */
    FMC(0x28) = 0;
    FMC(0x04) = 0x1e0;
    while (i < n) {
        if (mode == PIO_GATE_NONE) {
            /* no gate at all: every pop lands, the loop bound is the only cap */
            buf[i++] = FMC(0x80);
            ++pio_popped;
            continue;
        }
        if (mode == PIO_GATE_OLD) {
            /* THE HISTORICAL GATE - explicit control, kept byte-for-byte.
             * The only change is that the single FIFOCTR read is bound to `c`
             * so the trace can record exactly the value the gate compares. */
            uint32_t v = FMC(0x80);
            ++pio_popped;
            buf[i] = v;
            uint32_t c = FMC(0x28);
            int ok = ((c & 0xffu) == ((i * 4u + 4u) & 0xffu));
            if (nand_trace_on) nand_trace_rec(i, c, v, ok);
            if (ok) {
                ++i; ++pio_confirmed; stall = 0;
            } else if (++stall > 4096u) { failed(0x100); r = -1; break; }
            continue;
        }
        /* FIXED / FULL32: sample FIFOCTR BEFORE the pop; retain word n only when
         * the sampled count is 4*n. A mismatch is popped and discarded (a pop
         * always consumes the FIFO), never written to the destination. */
        {
            uint32_t c = FMC(0x28);
            uint32_t v = FMC(0x80);
            int hit = mode == PIO_GATE_FULL32 ? (c == i * 4u)
                                              : ((c & 0xffu) == ((i * 4u) & 0xffu));
            ++pio_popped;
            if (hit) {
                buf[i] = v;
                ++i;
                ++pio_confirmed;
                stall = 0;
            } else if (++stall > 4096u) { failed(0x100); r = -1; break; }
        }
    }
    pio_words = i;
    FMC(0x48) = 0x100000u;
    FMC(0x04) = 0x20;
    return r;
}
static int pio_body(uint32_t *buf, unsigned len) {
    /* Verified in the saved RetailOS image at 0x080d3432..0x080d3456:
     * pop/store first, then compare the counter's low byte with 4*i+4.
     * This also restores READ ID/parameter initialization, not just the
     * experimental page drain. The pre-pop mode remains an explicit test. */
    return pio_drain_mode(buf, len, PIO_GATE_OLD);
}
static int pio_drain14(uint32_t *buf, unsigned len, uint32_t v14) {
    FMC(0x30) = 3;
    FMC(0x14) = v14;
    return pio_body(buf, len);
}
/* today's drain value 0x100 = 1 << 8, byte-for-byte what every earlier round
 * wrote (0x080d33fc with arg = 0) */
static int pio_drain(uint32_t *buf, unsigned len) {
    return pio_drain14(buf, len, 0x100);
}
static int data_in_variant(uint32_t *buf, unsigned len, unsigned flags) {
    if (flags & DP_PRE48) FMC(0x48) = 0x08000000u;
    if (flags & DP_W18) FMC(0x18) = 1;
    FMC(0x30) = len - 1;
    FMC(0x14) = 1;
    FMC(0x24) = 0;
    FMC(0x04) = 0x22;
    int r = wt(8);
    if (r) return r;                    /* arming failed: no FIFO pops */
    return pio_drain(buf, len);
}
/* today's phase (the one every earlier round used) */
static int data_in(uint32_t *buf, unsigned len) {
    return data_in_variant(buf, len, 0);
}
/* RetailOS's non-ECC data phase: +0x48 = 0x08000000, then +0x18 = 1, then
 * the historical arming. Read-only; the flash array is never written. */
static int data_in_retailos(uint32_t *buf, unsigned len) {
    return data_in_variant(buf, len, DP_PRE48 | DP_W18);
}
/* Normal PPN storage path, translated from the saved stock read sequencer
 * (0x0896eb00). Unlike 0x084ed600, this uses EIGHT address cycles, SPR record
 * capture and DMA. It never calls the boot reader or pops the PIO FIFO.
 * Use a private uncached, identity-mapped DMA buffer, then copy to the caller.
 * CPU replays the read-only register sequence; no CS program is installed. */
static uint32_t ppn_dma[4096] MM_DMA_BUF __attribute__((aligned(64)));
static void desel(void);
static int ppn_status(void) {
    FMC(0x48) = POLL;
    FMC(0x04) = 0xca;
    int ok = wait_any(&FMC(0x48), POLL, WAIT_US) == WAIT_OK;
    nand.st = FMC(0x4c) & 0xffu;
    FMC(0x04) = 0x30020;
    FMC(0x48) = 0x1800000;
    if (!ok) { failed(POLL); return -1; }
    if (nand.st & 0x11u) { failed(1); return -2; }
    return 0;
}
static int ppn_dma_wait(void) {
    int r = wt(0x100000);
    if (!r) r = wt(8);
    return r;
}
static uint32_t ppn_records[16];
static unsigned ppn_transport_complete;
static int ppn_page(unsigned ce, uint32_t row, unsigned count) {
    ppn_transport_complete = 0;
    nand_passive = 1;
    nand.ph = 0;
    nand.ppn_bytes = nand.ppn_start = nand.ppn_end = 0;
    nand.ppn_stage = 1;
    for (unsigned i = 0; i < 10; ++i) nand.pf[i] = 0;
    /* CS +0x028..0x0f0: D04 is stock runtime timing (0x20010000),
     * OR 0x01000801, then replace CE bits 1..8. */
    FMC(0x00) = RUN_TM | 0x01000801u | (2u << ce);
    nand.ppn_select = FMC(0x00);
    int array_error = 0;
    int r = cmd(0x0a);  /* single/final setup entry, CS +0x120 */
    /* Snapshot's real request stream at 0x08cd9f70 has 0x4040, not 0x40:
     * BOTH low bytes encode the four-sector count. The omitted second
     * count let the first record work but misframed subsequent sectors. */
    if (!r) r = addr(8, count * 0x1010u, row);
    if (!r) r = cmd(0x37);
    if (!r) r = cmd(0x77);
    if (!r) r = cmd(0x7d);
    if (!r) {
        nand.ppn_stage = 2;
        r = ppn_status();
        /* A failed array read still owns a queued result. Drain/release it
         * before issuing another request. Never commit its bytes to callers.
         * run39's failures after the first blank read suggest queue poisoning;
         * recovery by completing the read needs physical confirmation. */
        if (r == -2) { array_error = r; r = 0; }
    }
    nand.ppn_start = nand.st;
    if (!r) r = cmd(0x7a);
    /* Read all four sectors from column zero. run37 showed that the old
     * nonzero-column calculation returned payload bytes as metadata. */
    for (unsigned sector = 0; !r && sector < count; ++sector) {
        nand.ppn_stage = 3 + sector * 8;
        FMC(0x48) = 0x100008;
        FMC(0x30) = 15;
        FMC(0x58) = 0x20;
        FMC(0x04) = 0x32;
        r = wt(8);
        if (r) break;
        nand.ppn_stage = 4 + sector * 8;
        FMC(0x30) = 1023;
        if (!sector) {
            FMC(0x58) = 1;
            FMC(0x04) = 0x62;
        } else {
            /* CS +0x8d8: subsequent sectors enter on the other buffer. */
            FMC(0x14) = 0x201;
            FMC(0x18) = 2;
            FMC(0x04) = 0x1e2;
        }
        FMC(0x40) = 0x1102;
        if (wait_until(&FMC(0x40), 2, 0, WAIT_US) != WAIT_OK) {
            failed(0x40); r = -1; break;
        }
        for (unsigned i = 0; i < 4; ++i)
            ppn_records[sector * 4 + i] = FMC(0x60 + i * 4);
        if (sector) { r = wt(0x100000); if (r) break; }
        nand.ppn_stage = 5 + sector * 8;
        FMC(0xc54) = 0xc;
        __asm__ volatile("dsb sy" ::: "memory");
        FMC(0x34) = (uint32_t)(uintptr_t)(ppn_dma + sector * 1024);
        FMC(0x38) = 7;
        r = wt(8);
        for (unsigned i = 0; !r && i < 3; ++i) {
            nand.ppn_stage = 6 + sector * 8 + i;
            FMC(0x14) = i == 1 ? 0x201 : 0x102;
            FMC(0x18) = i == 1 ? 2 : 1;
            FMC(0x04) = 0x1e2;
            r = ppn_dma_wait();
        }
    }
    if (!r) {
        /* CS +0x9c8..0x9f0: flush last DMA buffer. */
        nand.ppn_stage = 40;
        FMC(0x14) = 0x200;
        FMC(0x18) = 2;
        FMC(0x30) = 1023;
        FMC(0x04) = 0x1a0;
        r = wt(0x100000);
    }
    if (!r) r = cmd(0x77);
    if (!r) r = cmd(0x79);
    if (!r) {
        nand.ppn_stage = 41;
        uint32_t start = USEC;
        unsigned polls = 0;
        do {
            r = ppn_status();
            if (r || (nand.st & 0x62u) == 0x62u) break;
            if (USEC - start >= WAIT_US || ++polls >= 64u) {
                failed(POLL); r = -1; break;
            }
        } while (1);
    }
    nand.ppn_end = nand.st;
    /* Inspection may expose an errored page only when the complete data
     * transfer and final ready handshake finished. Normal reads still
     * preserve caller buffers on every failure. */
    if ((r == 0 || r == -2) && nand.ppn_stage == 41 &&
        (nand.st & 0x62u) == 0x62u) ppn_transport_complete = 1;
    FMC(0x04) = 0x20;
    desel();
    if (!r && array_error) r = array_error;
    if (!r) {
        __asm__ volatile("dsb sy" ::: "memory");
        nand.ppn_bytes = count * 4096u;
        nand.ppn_stage = 42;
    }
    return r;
}
static int ppn_args(unsigned ce, uint32_t row, void *buf, uint32_t *meta) {
    if (ce > 1u || !buf || !meta || ((uintptr_t)buf & 3u) ||
        ((uintptr_t)meta & 3u) || !nand.caus || !nand.blocks || !nand.pages ||
        (row >> 19) >= nand.caus || ((row >> 7) & 0xfffu) >= nand.blocks ||
        (row & 127u) >= nand.pages) return -3;
    return 0;
}
int nand_read_ppn_page(unsigned ce, uint32_t row, void *buf, uint32_t meta[16]) {
    int r = ppn_args(ce, row, buf, meta);
    if (r) return r;
    r = ppn_page(ce, row, 4);
    if (!r) {
        for (unsigned i = 0; i < 4096; ++i) ((uint32_t *)buf)[i] = ppn_dma[i];
        for (unsigned i = 0; i < 16; ++i) meta[i] = ppn_records[i];
    }
    return r;
}
int nand_read_ppn_context(unsigned block, unsigned page, unsigned view, unsigned count,
                          void *buf, uint32_t meta[16]) {
    if (nand.blocks != 2088 || nand.pages != 128 || nand.caus != 2 ||
        (block != 2086 && block != 2087) || page > 5 || view > 1 || !count || count > 4 ||
        !buf || !meta || ((uintptr_t)buf & 3u) || ((uintptr_t)meta & 3u)) return -3;
    int r = ppn_page(0, (view << 20) | (block << 7) | page, count);
    if (!r) {
        for (unsigned i = 0; i < 4096; ++i) ((uint32_t *)buf)[i] = i < count * 1024u ? ppn_dma[i] : 0xffffffffu;
        for (unsigned i = 0; i < 16; ++i) meta[i] = i < count * 4u ? ppn_records[i] : 0xffffffffu;
    }
    return r;
}
static void burst_copy_4096(uint32_t *dst, const uint32_t *src) {
    for (unsigned i = 0; i < 4096; i += 8) {
        uint32_t a = src[i], b = src[i+1], c = src[i+2], d = src[i+3];
        uint32_t e = src[i+4], f = src[i+5], g = src[i+6], h = src[i+7];
        dst[i] = a; dst[i+1] = b; dst[i+2] = c; dst[i+3] = d;
        dst[i+4] = e; dst[i+5] = f; dst[i+6] = g; dst[i+7] = h;
    }
}
int nand_inspect_ppn_page(unsigned ce, uint32_t row, void *buf, uint32_t meta[16]) {
    int r = ppn_args(ce, row, buf, meta);
    if (r) return r;
    r = ppn_page(ce, row, 4);
    if (ppn_transport_complete) {
        burst_copy_4096((uint32_t *)buf, ppn_dma);
        for (unsigned i = 0; i < 16; ++i) meta[i] = ppn_records[i];
        return r;
    }
    /* -2 specifically promises transport-complete diagnostic bytes here. */
    return r == -2 ? -1 : r;
}
int nand_read_ppn(unsigned ce, uint32_t row, unsigned sector,
                   void *buf, uint32_t meta[4]) {
    int r = ppn_args(ce, row, buf, meta);
    if (r || sector > 3u) return -3;
    r = ppn_page(ce, row, 4);
    if (!r) {
        for (unsigned i = 0; i < 1024; ++i)
            ((uint32_t *)buf)[i] = ppn_dma[sector * 1024 + i];
        for (unsigned i = 0; i < 4; ++i) meta[i] = ppn_records[sector * 4 + i];
        nand.ppn_bytes = 4096;
    }
    return r;
}
/* the named entry points above are what the phase selector calls */
static int phase_data_in(uint32_t *buf, unsigned len, unsigned flags) {
    if (flags == (DP_PRE48 | DP_W18)) return data_in_retailos(buf, len);
    return data_in_variant(buf, len, flags);
}
/* Round 19 / nandscan5, shape 3: the ECC-style payload arming RetailOS uses
 * when its ecc argument is nonzero - FMC+0x30 = 0x34, +0x14 = 1, +0x24 =
 * 0x400, +0x04 = 0x22, wait bit 8 - then the SAME PIO drain loop as
 * data_in(). Only the arming differs, so comparing it against data_in()
 * isolates the arming from the drain. Read-only: the flash array is never
 * written; every wait goes through wt() and is bounded. */
static int data_in_ecc34(uint32_t *buf, unsigned len) {
    FMC(0x30) = 0x34;
    FMC(0x14) = 1;
    FMC(0x24) = 0x400;
    FMC(0x04) = 0x22;
    int r = wt(8);
    if (r) return r;                    /* arming failed: no FIFO pops */
    return pio_drain(buf, len);
}
static void sel_tm(unsigned ce, uint32_t tm) { FMC(0x00) = tm | (1u << ce) << 1 | 1u; }
static void sel(unsigned ce) { sel_tm(ce, nand.tm); }
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
 * reading. Gates, CLKCON +0x1c and the PSC are written only by round
 * 13/14's recover() (+0x00 not latching): the +0x1c clock setup, the
 * domain-6 wake if it is off, candidate gate bits as a last resort. */
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
 * 3 +0x04 bit 30. Round 16: _resetFMSS waits for +0x04 bit 30 (FMC idle,
 * 0x080ff842 lsls #1 / bpl; its timeout text "waiting for FMC idle"), not
 * bit 31: the device's R81 had +0x04 = 0x40000000 while the old bit-31
 * wait expired (in 3). Then +0x04 = 0x80000000 (0x080ff84c, bit 31 acked)
 * and +0x81c = 1 (ECC/FIFO engine reset, 0x080ff850). */
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
    if (wait_any(&FMC(0x04), 0x40000000u, WAIT_US) != WAIT_OK) return 3;
    FMC(0x04) = 0x80000000u;
    FMC(0x81c) = 1;
    return 0;
}
/* Round 15: PPN power state 2 (PPN_FEATURE__POWER_STATE), RetailOS
 * 0x0812abc0 -> 0x0812aa5c. Its NAND init (0x080fb6e0) runs it once,
 * unconditionally, right after reset / READ ID / timings and before any
 * page read, and its resume path (0x0812a4ae) again after a chip reset;
 * the probe here resets CE0 (0xff), so the chip may be back in its
 * default power state. Sequence (checked against the snap6 disassembly):
 * select at the slow timing (literal 0x000ff001), 0xef, 2 address bytes
 * 0x80 0x01 (+0x0c = 0x180), data out 02 00 00 00 (0x080d3558: +0x00 |=
 * bit 25 and &= ~(bit 10 | bit 24), +0x14 = 1, +0x24 = 0, +0x04 = 0x2e0,
 * one word to the +0x80 FIFO, W1C +0x48 bit 20, +0x04 = 0x20, +0x00 bit
 * 25 cleared; then 0x0810d3e6: +0x30 = 3, +0x14 = 0x100, +0x04 =
 * 0x100e4, wait bit 3), 0xe7, 0x77, 0x7d, status poll 0x0840c3e8 (W1C
 * bit 23, +0x04 = 0xea, wait bit 23 up to 100 ms, then +0x04 = 0x20 and
 * W1C bit 23 whatever the wait did (round 17), +0x4c = status, W1C bit
 * 24), 0x77, deselect. +0x14 stays 0x100 from the data out through the
 * poll, as in RetailOS (neither 0x0840c3e8 nor 0x081810b8 writes it);
 * the page read's poll() sets it to 1 (0x084d1e6e). A feature write, not
 * a program or erase: the flash array is not touched. Every wait bounded; the first failure
 * stops it. nand.pwr: 0 not run, 1 done, else the step that timed out
 * (2 0xef, 3 address, 4 data out, 5 0xe7/0x77/0x7d, 6 status poll, 7
 * the final 0x77, round 15b);
 * nand.pwr_st the status byte. Restore after EVERY probe reset; a once-per-
 * boot cache is invalid as soon as either CE is reset. NAND_SET_POWER0
 * leaves it out. */
#ifndef NAND_SET_POWER
#define NAND_SET_POWER 1
#endif
#if NAND_SET_POWER
static uint32_t set_power(unsigned ce) {
    uint32_t step = 2;
    sel_tm(ce, 0xff000u);
    int r = cmd(0xef);
    if (!r) { step = 3; r = addr_lo(2, 0x180); }
    if (!r) {
        step = 4;
        FMC(0x00) |= 1u << 25;
        FMC(0x00) &= 0xfefffbffu;
        FMC(0x14) = 1;
        FMC(0x24) = 0;
        FMC(0x04) = 0x2e0;
        FMC(0x80) = 2;                  /* bytes 02 00 00 00 */
        FMC(0x48) = 0x100000u;
        FMC(0x04) = 0x20;
        FMC(0x00) &= ~(1u << 25);
        FMC(0x30) = 3;
        FMC(0x14) = 0x100;
        FMC(0x04) = 0x100e4;
        r = wt(8);
    }
    if (!r) { step = 5; r = cmd(0xe7); }
    if (!r) r = cmd(0x77);
    if (!r) r = cmd(0x7d);
    if (!r) {
        step = 6;
        FMC(0x48) = POLL;
        FMC(0x04) = 0xea;
        if (wait_any(&FMC(0x48), POLL, 100000u) != WAIT_OK) {
            failed(POLL);
            r = -1;
        }
        /* round 17: end the poll and ack bit 23 on success too (RetailOS's
         * wait 0x081810b8 does both for bit 23 at 0x081810f0..f4, as wt()
         * here). Before, a good poll left +0x04 = 0xea: the final 0x77
         * (pw 7) and the page read's 0x0a (ph 2) never got a command phase */
        FMC(0x04) = 0x20;
        FMC(0x48) = POLL;
        nand.pwr_st = FMC(0x4c) & 0xff;
        FMC(0x48) = 0x1000000u;
    }
    if (!r) { step = 7; r = cmd(0x77); }
    desel();
    return r ? step : 1;
}
#endif
/* attempts: 1 controller reset (ctl_init, even if a step timed out: the
 * probe still tells more), 0xff reset and READ ID at the slow timing
 * 0xff000 as RetailOS's detection; 2 READ ID at the runtime timing
 * 0x20010000 without a reset (the round-5 path that answered); 3 FMC
 * quiesce, slow 0xff reset, 100 ms. Each phase stops the attempt at its
 * first failure (no FIFO pops after a failed arm). */
/* Round 13. Device (after round 12, which touched no NAND code): "NAND
 * ERR id 00000000 cs 00000000 48 00000000 er 5", the round-6 symptom:
 * every command wait expired and +0x00 read 0 though the chainloader
 * leaves 1 there, so the FMC looks unclocked or held in reset at the
 * handoff. Diagnostic: write CS_PROBE (runtime timing, no CE) to +0x00 and
 * read it back; a clocked controller latches it. If not, recover() tries
 * one step at a time, each only if the probe after the previous one still
 * failed, and probes again after each (round 14 order, /tmp/inv_psc.txt):
 *   1 ctl_init: RetailOS's own controller reset (known safe)
 *   2 CLKCON +0x1c = (+0x1c & 0xffff) | 0x10010000: the bootrom's clock
 *     setup before its NAND boot (0x2000337c: source PLL-A, divider 1;
 *     the low half, the memory domain, is kept). NB this is the field
 *     clk.c and RetailOS's MBX driver treat as the GPU clock [31:16]
 *   3 PSC 0x39700000 domain 6 (memory subsystem) wake, RetailOS
 *     0x08005856, only if +0x14 shows the domain off (bit 6) or settling
 *     (bit 16); no other domain is touched
 *   4 last resort: the candidate CLKCON gates, cleared (= on) only, never
 *     set. The FMC gate is not known: the bootrom ungates nothing for its
 *     NAND reads and RetailOS never gates the FMC, so the candidates are
 *     the gates on at bootrom entry and in the live device words that
 *     RetailOS never switches: +0x4c bits 22, 20, 18, 14, 12 (ids 0x17,
 *     0x19, 0x1b, 0x1f, 0x21; bit 14 added in round 14), +0x68 bits 13, 11
 *     (0x38, 0x3a).
 * No SoC reset bit for the FMC is known. Every wait is bounded. No flash
 * array access: controller, CLKCON and PSC registers only. R81 "pwr"
 * "clk" "pin" (the state before recover()) and "rc" log it. */
#define CS_PROBE 0x20010001u
#define G4C 0x00545000u
#define G68 0x00002800u
#define PSC(o) (*(volatile uint32_t *)(0x39700000u + (o)))
#define PD6 0x40u
#define PD6_SETTLE 0x10000u
/* rc_log: 0 +0x00 as found, 1 its first readback, 2 ctl_init's step, 3
 * readback after it, 4/5 CLKCON +0x4c/+0x68 before the gates, 6/7 after,
 * 8 readback after the gates, 9 nand.rcv; round 14: 10/11 CLKCON +0x1c
 * before/after step 2, 12 readback after it, 13 PSC +0x14 before step 3,
 * 14 pd6_wake() result, 15 PSC +0x14 after, 16 readback after it. 0 for
 * steps not run, all 0 when +0x00 latched at once. */
static uint32_t rc_log[17];
/* failure-path snapshot before recover(): PSC +08 +0c +10 +14 +18 +1c
 * +100, CLKCON +00 +04 +1c +40 +44 +48 +4c +58 +68 +6c, GPIO (FMC pads,
 * bootrom 0x20001bc4) +c0 +e0 +f4 +100 +114 */
static const uint16_t pwr_at[7] = {0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c, 0x100};
static const uint8_t clk_at[10] = {0x00, 0x04, 0x1c, 0x40, 0x44, 0x48, 0x4c, 0x58, 0x68, 0x6c};
static const uint16_t pin_at[5] = {0xc0, 0xe0, 0xf4, 0x100, 0x114};
static uint32_t snap[22];
static uint32_t latch(void) { FMC(0x00) = CS_PROBE; return FMC(0x00); }
/* 0 = domain 6 on and settled (nothing written), 1 = woken, 2 = status
 * never matched the shadow, 3 = settle bit never cleared (the enable mask
 * bit is restored in every case) */
static uint32_t pd6_wake(void) {
    if (!(PSC(0x14) & (PD6 | PD6_SETTLE))) return 0;
    uint32_t r = 1, t0 = USEC;
    PSC(0x1c) = 0x00140065u;
    PSC(0x100) &= ~PD6;
    PSC(0x0c) = PD6;
    __asm__ volatile("dsb sy" ::: "memory");
    for (uint32_t n = 0; (PSC(0x14) & PD6) != (PSC(0x08) & PD6); ++n)
        if (USEC - t0 >= WAIT_US || n >= WAIT_US * 64u) { r = 2; break; }
    if (r == 1 && wait_until(&PSC(0x14), PD6_SETTLE, 0, WAIT_US) != WAIT_OK) r = 3;
    PSC(0x100) |= PD6;
    return r;
}
/* Round 16: the parallel NAND bus pads, the bootrom's own mux before its
 * NAND boot (0x20001bc4, called at 0x20003390): GPIO +0xc0 = (+0xc0 &
 * 0xf) | 0x22222220, +0xe0 = 0x22222222, +0xf4 |= 0xfe, +0x100 = (+0x100
 * & 0xff0000f0) | 0x00333302, +0x114 |= 1. The device's R81 pin row (the
 * handoff state) had +0x100 = 0x00000002 (bank 8 pins 2..5, CLE ALE WE#
 * RE#, not on the FMC), +0xe0 pin 0 and +0xc0 pin 7 clear: commands ran
 * with er 0 but READ ID read 00000000. Function selects of the NAND bus
 * pins only, exactly the bootrom's values, every probe (idempotent); no
 * flash access. pin2 = the five words read back after it (R81). */
#define GPIO(o) (*(volatile uint32_t *)(0x3cf00000u + (o)))
static uint32_t pin2[5];
static void nand_pins(void) {
    GPIO(0xc0) = (GPIO(0xc0) & 0x0fu) | 0x22222220u;
    GPIO(0xe0) = 0x22222222u;
    GPIO(0xf4) |= 0xfeu;
    GPIO(0x100) = (GPIO(0x100) & 0xff0000f0u) | 0x00333302u;
    GPIO(0x114) |= 1u;
    __asm__ volatile("dsb sy" ::: "memory");
    for (unsigned i = 0; i < 5; ++i) pin2[i] = GPIO(pin_at[i]);
}
static void recover(void) {
    for (unsigned i = 0; i < 7; ++i) snap[i] = PSC(pwr_at[i]);
    for (unsigned i = 0; i < 10; ++i) snap[7 + i] = CLK(clk_at[i]);
    for (unsigned i = 0; i < 5; ++i) snap[17 + i] = *(volatile uint32_t *)(0x3cf00000u + pin_at[i]);
    rc_log[0] = nand.cs0; rc_log[1] = nand.cs;
    rc_log[2] = ctl_init();
    nand.rcv = 1;
    if ((rc_log[3] = latch()) == CS_PROBE) return;
    if (nand_passive) return;   /* no CLKCON/PSC writes on the game path */
    rc_log[10] = CLK(0x1c);
    CLK(0x1c) = (CLK(0x1c) & 0xffffu) | 0x10010000u;
    __asm__ volatile("dsb sy" ::: "memory");
    delay_us(100);
    rc_log[11] = CLK(0x1c);
    nand.rcv = 2;
    if ((rc_log[12] = latch()) == CS_PROBE) return;
    rc_log[13] = PSC(0x14);
    rc_log[14] = pd6_wake();
    /* round 16: settle after a wake before the re-probe (and the probe's
     * ctl_init): RetailOS's domain-6 wake 0x08005856 spins ~1 ms (1050
     * USEC ticks, 0x080058a6..ba), the bootrom waits 20 ms (0x20003370)
     * before it touches CLKCON, the pads or the FMC */
    if (rc_log[14] == 1) delay_us(2000);
    rc_log[15] = PSC(0x14);
    nand.rcv = 3;
    if ((rc_log[16] = latch()) == CS_PROBE) return;
    rc_log[4] = CLK(0x4c); rc_log[5] = CLK(0x68);
    CLK(0x4c) &= ~G4C;
    CLK(0x68) &= ~G68;
    __asm__ volatile("dsb sy" ::: "memory");
    delay_us(100);
    rc_log[6] = CLK(0x4c); rc_log[7] = CLK(0x68);
    nand.rcv = (rc_log[8] = latch()) == CS_PROBE ? 4 : 5;
}
int nand_probe(void) {
    uint32_t p[14];
    ++nand.probes;
    nand.errors = 0;
    nand.ph = 0;
    nand.pwr = nand.pwr_st = nand.ppn_power1 = 0;
    nand.cs0 = FMC(0x00);
    nand.st48 = FMC(0x48);
    nand.rcv = 0;
    for (unsigned i = 0; i < 17; ++i) rc_log[i] = 0;
    for (unsigned i = 0; i < 22; ++i) snap[i] = 0;
    if ((nand.cs = latch()) != CS_PROBE) recover();
    nand_pins();        /* round 16: after recover(), before ctl_init and READ ID */
    /* An init timeout is only logged (nand.init) and the probe continues,
     * for diagnostics: on the device the runtime-timing attempt answered
     * without this reset before. Once NAND is confirmed OK on hardware with
     * ctl_init in place, make this fail-early. */
    nand.init = ctl_init();
    FMC(0x38) = 7;      /* round 16: bootrom 0x20009280, RetailOS 0x084ed620; after the reset */
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
#if NAND_SET_POWER
    if (ok) {
        /* Complete both resets before either feature setup. run38 showed
         * CE0 failing after the old CE0-config/CE1-reset/config ordering;
         * the ordering change still needs physical confirmation. */
        sel_tm(1, 0xff000u);
        int r1 = cmd(0xff);
        delay_us(50000);
        desel();
        if (r1) { nand.ppn_power1 = 2u << 8; return 0; }
        nand.pwr = set_power(0);
        uint32_t p1 = set_power(1);
        nand.ppn_power1 = (p1 << 8) | nand.pwr_st;
        if (nand.pwr != 1u || p1 != 1u) return 0;
    }
#endif
    return ok;
}
/* Round 15: RetailOS's standalone CPU page read 0x084ed600 (device:
 * "rd P0 ph 000010 st 50": the old 0x07 + 8-byte form got status 0x50 and
 * the read stopped there, before 0x7a). +0x38 = 7, chip select at the
 * runtime timing, +0x48 = 0x0ff00ffe (W1C), +0x81c = 1; 0x0a, 4 address
 * bytes = the physical page in +0x0c (+0x10 not written), 0x37; ready wait
 * 0x77 0x7d + status poll (0x084d1e5c); 0x7a; data. RetailOS reads the
 * data whatever the status says (its ready wait never looks at +0x4c):
 * status bit 4 (GEB / uECC, also set on erased pages) is only recorded in
 * nand.st, the caller judges it. Status bit 0 (fail) is a failure: phase
 * 0x01 is recorded (pf = FMC right after the poll), the data is still
 * transferred, then -2. A timed-out phase stops the read (-1) and ph/pf
 * describe it. The phase record is reset first. sector is not encoded
 * (the 4-byte address has no sector field): the transfer is the page's
 * 16 B meta + first 4 KB. */
/* The one read implementation, shared verbatim by nand_read_sector and
 * nand_read_raw so the two can never diverge in command sequence. It never
 * clears buf or meta: data_in() only assigns words it pops, and it returns
 * before popping anything when the data phase cannot be armed. A PIO stall
 * mid-transfer can leave a partial transfer in the buffer (data_in assigns
 * buf[i] = FMC(0x80) before checking the counter), and a read that really
 * delivers zeros overwrites the caller's buffer with zeros. That is exactly
 * what the nandscan2 sentinel experiment relies on: sentinel intact = no
 * transfer, buffer changed = something was transferred. */
/* One page-read sequence; `flags` selects ONLY the data phase (0 = today's
 * data_in(), DP_* = the RetailOS arming). Command and address phases are
 * shared, so two variants cannot diverge anywhere else. */
static int read_page_phase(unsigned ce, uint32_t raw_addr, void *buf,
                           uint32_t meta[4], unsigned flags) {
    nand.ph = 0;
    for (unsigned i = 0; i < 10; ++i) nand.pf[i] = 0;
    FMC(0x38) = 7;
    sel_tm(ce, RUN_TM);
    FMC(0x48) = 0x0ff00ffeu;
    FMC(0x81c) = 1;
    int r = cmd(0x0a);
    if (!r) r = addr_lo(4, raw_addr);
    if (!r) r = cmd(0x37);
    if (!r) r = cmd(0x77);
    if (!r) r = cmd(0x7d);
    if (!r) r = poll();
    int st_fail = !r && (nand.st & 0x01);
    if (st_fail) failed(0x01);
    if (!r) r = cmd(0x7a);
    if (!r) r = phase_data_in(meta, 16, flags);
    for (unsigned k = 0; !r && k < 4; ++k)
        r = phase_data_in((uint32_t *)buf + 256 * k, 1024, flags);
    desel();
    return r ? r : st_fail ? -2 : 0;
}
static int read_page_raw(unsigned ce, uint32_t raw_addr, void *buf,
                         uint32_t meta[4]) {
    return read_page_phase(ce, raw_addr, buf, meta, 0);
}
int nand_read_sector(unsigned ce, uint32_t page, unsigned sector,
                     void *buf, uint32_t meta[4]) {
    (void)sector;
    return read_page_raw(ce, page, buf, meta);
}
/* Read-only experiment hook (nandscan2): identical to nand_read_sector but
 * raw_addr goes to addr_lo() unchanged and unbounded. No write/erase is
 * reachable from here; this exists only so a caller can test whether a
 * nonzero 4-byte page address selects a page or reads as zeros/nothing. */
int nand_read_raw(unsigned ce, uint32_t raw_addr, void *buf, uint32_t meta[4]) {
    return read_page_raw(ce, raw_addr, buf, meta);
}
/* Round 18 / nandscan3: read entry point with a recovery pre-action. The
 * point is to find out WHY delivery stops after ~2 reads on the device
 * (run26): every mode below performs the same single command sequence
 * (read_page_raw) and differs only in what runs immediately before it, so a
 * mode that restores delivery names the state that was stuck.
 *   mode 0  baseline: no pre-action at all.
 *   mode 1  ctl_init(): RetailOS's own FMC controller reset. The probe runs
 *           it and so does recover(); known safe, no flash access.
 *   mode 2  desel() + 1 ms: release the chip select and let it settle.
 *   mode 3  FMC(0x48) = 0xffffffff (W1C every completion bit) and
 *           FMC(0x81c) = 1 (ECC/FIFO engine reset): clear a latched/aborted
 *           completion state without resetting the whole controller.
 *   mode 4  nand_probe(): the full heavy probe (ctl_init + 3 READ ID
 *           attempts + a parameter-page read), diagnostic only.
 *   mode 5  set_power(0): PPN SET FEATURES power state 2. THIS IS A FEATURE
 *           WRITE, NOT A PROGRAM OR ERASE - the flash array is not touched,
 *           no data changes. RetailOS issues the same command at its NAND
 *           init, and the probe already ran it once this boot (nand.pwr), so
 *           it is a known-safe re-issue.
 *   mode 6  desel() + 1 ms, then ctl_init(): a combination of modes 1/2.
 * raw_addr still goes to addr_lo() unchanged and unbounded (the nandscan2
 * experiment's property is preserved). No write/erase function is called and
 * nand_passive is forced, so recover() can never touch CLKCON/PSC. */
int nand_read_mode(unsigned ce, uint32_t raw_addr, void *buf, uint32_t meta[4],
                   unsigned mode) {
    if (ce > 1u || !buf || !meta) return -3;
    if (mode >= NAND_RM_MODES) return NAND_RM_BAD;
    nand_passive = 1;
    switch (mode) {
    case NAND_RM_NONE: break;
    case NAND_RM_CTL_INIT: (void)ctl_init(); break;
    case NAND_RM_DESEL_DELAY: desel(); delay_us(1000); break;
    case NAND_RM_W1C: FMC(0x48) = 0xffffffffu; FMC(0x81c) = 1; break;
    case NAND_RM_PROBE: (void)nand_probe(); break;
    case NAND_RM_POWER:
#if NAND_SET_POWER
        (void)set_power(0);
        break;
#else
        return NAND_RM_UNAVAIL;
#endif
    case NAND_RM_DESEL_CTL: desel(); delay_us(1000); (void)ctl_init(); break;
    default: return NAND_RM_BAD;     /* unreachable: mode < NAND_RM_MODES */
    }
    return read_page_raw(ce, raw_addr, buf, meta);
}
/* Round 18b / nandscan4: the same page read as nand_read_raw/nand_read, but
 * the data phase is one of four arming variants. ONLY the data phase differs
 * - read_page_phase() shares the command/address/status/0x7a sequence, and
 * the variant is applied to the 16 B metadata read and all four 1 KiB data
 * reads (the same five data_in calls the normal read makes):
 *   0  NAND_RP_TODAY          today's data_in() (= every earlier round)
 *   1  NAND_RP_RETAILOS       +0x48 = 0x08000000, then +0x18 = 1, then today's
 *                             arming: RetailOS 0x084ed600's non-ECC data phase
 *   2  NAND_RP_RETAILOS_NO48  +0x18 = 1 only: variant 1 minus the +0x48 write
 *   3  NAND_RP_TODAY_18       today's phase plus +0x18 = 1 only
 * Variants 2 and 3 therefore perform the same data-phase writes (they are the
 * subtractive and additive isolations of the +0x18 write); running both is a
 * control for run-to-run noise, and a difference between them would itself be
 * evidence of history/state dependence. Unknown variants get a distinct
 * negative code and touch nothing. Read-only: no write/erase is reachable,
 * raw_addr is not range-checked or decoded (nandscan2's property), and
 * passive mode is forced. A null buffer/meta or ce >= 2 is -3. */
int nand_read_phase(unsigned ce, uint32_t raw_addr, void *buf, uint32_t meta[4],
                    unsigned variant) {
    unsigned flags;
    if (ce > 1u || !buf || !meta) return NAND_RP_BADARGS;
    switch (variant) {
    case NAND_RP_TODAY: flags = 0; break;
    case NAND_RP_RETAILOS: flags = DP_PRE48 | DP_W18; break;
    case NAND_RP_RETAILOS_NO48:
    case NAND_RP_TODAY_18: flags = DP_W18; break;
    default: return NAND_RP_BADVARIANT;
    }
    nand_passive = 1;
    return read_page_phase(ce, raw_addr, buf, meta, flags);
}
/* Round 19 / nandscan5: the TRANSFER-SHAPE matrix. run26/27/28 established
 * that after the first page read the 4 KB payload stops being delivered
 * (0xcfcfcfcf filler, then zeros) while commands, addresses, the ready wait
 * and SMALL data-in reads (READ ID / parameter page) still work, and that
 * neither a controller/FIFO reset nor a re-probe restores it. The remaining
 * suspicion is therefore the 4 KB payload phase itself: its arming, its
 * chaining into four 1 KiB data_in() calls, or its end-of-phase state.
 *
 * Every shape below runs the SAME command/address/status/0x7a sequence as
 * today's page read (read_page_phase) and differs ONLY in how the payload is
 * armed, transferred and ended:
 *   0 NAND_SHAPE_TODAY     exactly today's read: meta via data_in(), then
 *                          4 x 1024-byte data_in() (the no-change control)
 *   1 NAND_SHAPE_ONE_4096  meta, then ONE 4096-byte data_in()
 *   2 NAND_SHAPE_REARM_7A  meta, then 0x7a + 1024 bytes x4 (0x7a re-issued
 *                          before each block, the data-command re-arm)
 *   3 NAND_SHAPE_ECC_34    meta, then 4 x 1024 bytes armed the ECC-style way
 *                          (data_in_ecc34: +0x30=0x34, +0x14=1, +0x24=0x400,
 *                          +0x04=0x22, wait bit 8) and drained by pio_drain()
 *   4 NAND_SHAPE_NO_81C    today's read WITHOUT FMC+0x81c = 1 at the start
 *   5 NAND_SHAPE_META_ONLY meta only, no 1 KiB payload (does a small
 *                          post-0x7a read still work after the break?)
 *   6 NAND_SHAPE_ONE_1024  meta, then ONE 1024-byte block (256 words)
 *   7 NAND_SHAPE_2X2048    meta, then two 2048-byte data_in() calls
 * Shape 0 and shapes 1/6/7 all use today's arming, so a difference between
 * them is the CHAINING of data_in calls; shape 3 changes the arming; shape 4
 * changes the pre-read FIFO/ECC reset; shape 5 isolates the payload from the
 * metadata read. ALL shapes force passive mode (no CLKCON/PSC writes), call
 * no write/erase function, and every wait is bounded (wt()/pio_drain()).
 * raw_addr is passed to the 4-byte address field unchanged (the nandscan2
 * property). An unknown shape returns a distinct negative code and touches
 * NOTHING. */
static int read_page_shape(unsigned ce, uint32_t raw_addr, void *buf,
                           uint32_t meta[4], unsigned shape) {
    uint32_t *w = (uint32_t *)buf;
    nand.ph = 0;
    for (unsigned i = 0; i < 10; ++i) nand.pf[i] = 0;
    FMC(0x38) = 7;
    sel_tm(ce, RUN_TM);
    FMC(0x48) = 0x0ff00ffeu;
    if (shape != NAND_SHAPE_NO_81C) FMC(0x81c) = 1;   /* shape 4 omits this */
    int r = cmd(0x0a);
    if (!r) r = addr_lo(4, raw_addr);
    if (!r) r = cmd(0x37);
    if (!r) r = cmd(0x77);
    if (!r) r = cmd(0x7d);
    if (!r) r = poll();
    int st_fail = !r && (nand.st & 0x01);
    if (st_fail) failed(0x01);
    if (!r) r = cmd(0x7a);
    if (!r) r = data_in(meta, 16);      /* every shape, today's meta read */
    switch (shape) {
    case NAND_SHAPE_META_ONLY:
        break;                          /* shape 5: stop after the metadata */
    case NAND_SHAPE_ONE_4096:
        if (!r) r = data_in(w, 4096);
        break;
    case NAND_SHAPE_2X2048:
        for (unsigned k = 0; !r && k < 2; ++k) r = data_in(w + 512 * k, 2048);
        break;
    case NAND_SHAPE_ONE_1024:
        if (!r) r = data_in(w, 1024);
        break;
    case NAND_SHAPE_REARM_7A:
        for (unsigned k = 0; !r && k < 4; ++k) {
            r = cmd(0x7a);              /* re-issue the data command */
            if (!r) r = data_in(w + 256 * k, 1024);
        }
        break;
    case NAND_SHAPE_ECC_34:
        for (unsigned k = 0; !r && k < 4; ++k) r = data_in_ecc34(w + 256 * k, 1024);
        break;
    case NAND_SHAPE_TODAY:
    case NAND_SHAPE_NO_81C:
    default:                            /* shape 0: exactly today's read */
        for (unsigned k = 0; !r && k < 4; ++k) r = data_in(w + 256 * k, 1024);
        break;
    }
    desel();
    return r ? r : st_fail ? -2 : 0;
}
/* Round 19 / nandscan5 entry point. Read-only experiment hook: the caller
 * must have probed; ce is bounded; buf/meta must be non-null. Passive mode is
 * forced BEFORE anything runs, so recover() can never touch CLKCON/PSC. An
 * unknown shape returns NAND_SHAPE_BAD (-7) without touching hardware; bad
 * arguments return NAND_SHAPE_BADARGS (-3). Otherwise 0 / -1 / -2 as the
 * normal page read. No write/erase path is reachable from here. */
int nand_read_shape(unsigned ce, uint32_t raw_addr, void *buf, uint32_t meta[4],
                    unsigned shape) {
    if (ce > 1u || !buf || !meta) return NAND_SHAPE_BADARGS;
    if (shape >= NAND_SHAPE_COUNT) return NAND_SHAPE_BAD;
    nand_passive = 1;
    return read_page_shape(ce, raw_addr, buf, meta, shape);
}
/* Round 20 / nandscan6: the read-ORDERING experiment. Every order runs the
 * SAME command/address/ready sequence and the SAME data_in() arming/drain
 * (only the order and extent of the data reads after 0x7a change), so a
 * difference between orders cannot be an arming or a drain difference:
 *   0 TODAY              meta(16) then 4 x 1024             (today)
 *   1 PAYLOAD_ONLY       no meta; 4 x 1024
 *   2 PAYLOAD_ONLY_4096  no meta; one 4096
 *   3 META_AFTER         4 x 1024, then meta(16)
 *   4 META_ONLY          meta(16) only
 *   5 PAYLOAD_1K_ONLY    no meta; one 1024
 *   6 SPLIT              4 x 1024, then 0x77+0x7d, then meta(16)
 *   7 NO_81C_PAYLOAD_ONLY no meta; 4 x 1024, without FMC+0x81c = 1
 * The historical read always fetched the 16-byte metadata FIRST; a phase or
 * offset interaction between that small transfer and the bulk transfer is the
 * one mechanism previous rounds never varied. A caller passes meta == NULL for
 * 1/2/5/7; those orders never form a metadata pointer.
 * All waits go through wt()/pio_drain() and are bounded; no write/erase is
 * reachable; passive mode is forced by the public entry point. */
static int read_page_order(unsigned ce, uint32_t raw_addr, void *buf,
                           uint32_t meta[4], unsigned order) {
    uint32_t *w = (uint32_t *)buf;
    nand.ph = 0;
    for (unsigned i = 0; i < 10; ++i) nand.pf[i] = 0;
    FMC(0x38) = 7;
    sel_tm(ce, RUN_TM);
    FMC(0x48) = 0x0ff00ffeu;
    /* order 7 is order 1 minus this pre-read FIFO/ECC reset */
    if (order != NAND_ORD_NO_81C_PAYLOAD_ONLY) FMC(0x81c) = 1;
    int r = cmd(0x0a);
    if (!r) r = addr_lo(4, raw_addr);
    if (!r) r = cmd(0x37);
    if (!r) r = cmd(0x77);
    if (!r) r = cmd(0x7d);
    if (!r) r = poll();
    int st_fail = !r && (nand.st & 0x01);
    if (st_fail) failed(0x01);
    if (!r) r = cmd(0x7a);
    switch (order) {
    case NAND_ORD_TODAY:
        if (!r) r = data_in(meta, 16);
        for (unsigned k = 0; !r && k < 4; ++k)
            r = data_in(w + 256 * k, 1024);
        break;
    case NAND_ORD_PAYLOAD_ONLY:
    case NAND_ORD_NO_81C_PAYLOAD_ONLY:
        for (unsigned k = 0; !r && k < 4; ++k)
            r = data_in(w + 256 * k, 1024);
        break;
    case NAND_ORD_PAYLOAD_ONLY_4096:
        if (!r) r = data_in(w, 4096);
        break;
    case NAND_ORD_META_AFTER:
        for (unsigned k = 0; !r && k < 4; ++k)
            r = data_in(w + 256 * k, 1024);
        if (!r) r = data_in(meta, 16);
        break;
    case NAND_ORD_META_ONLY:
        if (!r) r = data_in(meta, 16);
        break;
    case NAND_ORD_PAYLOAD_1K_ONLY:
        if (!r) r = data_in(w, 1024);
        break;
    case NAND_ORD_SPLIT:
        /* metadata LAST, but only after a ready-wait handshake inserted
         * between the payload and the metadata read (orders 3 and 6 differ
         * ONLY in this 0x77+0x7d insertion) */
        for (unsigned k = 0; !r && k < 4; ++k)
            r = data_in(w + 256 * k, 1024);
        if (!r) r = cmd(0x77);
        if (!r) r = cmd(0x7d);
        if (!r) r = data_in(meta, 16);
        break;
    default:
        break;              /* unreachable: the entry point rejects it */
    }
    desel();
    return r ? r : st_fail ? -2 : 0;
}
/* Round 20 / nandscan6 entry point. Read-only: ce is bounded, buf must be
 * non-null, meta may be NULL only for the orders that never read metadata
 * (1/2/5/7). An unknown order returns NAND_ORD_BAD (-9) and touches NOTHING;
 * bad arguments return NAND_ORD_BADARGS (-3). Passive mode is forced BEFORE
 * anything runs. Otherwise 0 / -1 / -2 as the normal page read. No write/erase
 * function is reachable from here. */
int nand_read_order(unsigned ce, uint32_t raw_addr, void *buf, uint32_t meta[4],
                    unsigned order) {
    if (ce > 1u || !buf) return NAND_ORD_BADARGS;
    if (order >= NAND_ORD_COUNT) return NAND_ORD_BAD;
    if (!meta && order != NAND_ORD_PAYLOAD_ONLY &&
        order != NAND_ORD_PAYLOAD_ONLY_4096 &&
        order != NAND_ORD_PAYLOAD_1K_ONLY &&
        order != NAND_ORD_NO_81C_PAYLOAD_ONLY) return NAND_ORD_BADARGS;
    nand_passive = 1;
    return read_page_order(ce, raw_addr, buf, meta, order);
}
/* Round 20 / nandscan6, task 2: the "DMA-ish" entry point. It exists so the
 * hypothesis can be stated and answered in code instead of being silently
 * dropped, but the disassembly does NOT support a drain implementation:
 *
 * 0x084cbd14 (the routine the "RetailOS DMA drain" hypothesis named) is the
 * hardware BCH ECC DECODE step of the FMC ECC sub-block at 0x38a00800. Its
 * literal-derived register sequence, in order, is:
 *   [0x38a0081c] = 1                 (+0x81c reset/arm)
 *   [0x38a00818] = 0x04000400        (+0x818 history/init)
 *   [0x38a0080c] = 1                 (+0x80c result init)
 *   [0x38a00810] = 0x3f1f73af        (+0x810 status init)
 *   [0x38a00820] = 0x80000000        (+0x820 config)
 *   [0x38a00808] = 0x02000000        (+0x808 control)
 *   [0x38a00804] = a0 | ((a1 & 0x7ff) << 16) | 0x1e1   (+0x804 ECC command)
 * then it reads the timestamp [0x3c700084], polls +0x810 bit 0 until it is
 * set (timeout 0x4e20 = 20000 ticks), on completion reads +0x80c and +0x818,
 * writes +0x810 = 1 to ack, and returns 0 / 1 / 0x80000002. There is NO data
 * movement, NO +0x30/+0x14/+0x24 arming, and NO +0x34/+0x38 destination write
 * anywhere in it. The two callees 0x0833526c / 0x08335284 are ARM veneers
 * (ldr pc,[pc,#-4]) into IRAM 0x22000fb8 / 0x22000fbe, i.e. a timestamp
 * getter and an "elapsed >= timeout" helper - fully resolved, not opaque.
 * Its only call in RetailOS's page reader 0x084ed600 is on the ECC path; the
 * NON-ECC path arms +0x18=1 +0x30=len-1 +0x14=1 +0x24=0 +0x04=0x22 and then
 * drains 1024 B at a time through 0x080d33fc (0x084ed73c) - exactly the PIO
 * path this driver already uses. The +0x34 DMA writes in RetailOS/BootROM
 * belong to a different raw-page parity routine, not to 0x084cbd14.
 *
 * So there is no evidenced DMA payload drain to implement. Rather than guess
 * a register sequence and pretend, both variants return NAND_RD_UNSUPPORTED
 * (-8) and touch NO hardware. This is the documented answer, not a failure to
 * port: see the README and the nandscan6 report's "dma unsupported" line. */
int nand_read_dmaish(unsigned ce, uint32_t raw_addr, void *buf, uint32_t meta[4],
                     unsigned variant) {
    (void)raw_addr;
    if (ce > 1u || !buf) return NAND_RD_BADARGS;
    if (variant >= NAND_RD_VARIANTS) return NAND_RD_BADVARIANT;
    if (variant == NAND_RD_META_DMA && !meta) return NAND_RD_BADARGS;
    nand_passive = 1;
    return NAND_RD_UNSUPPORTED;     /* no hardware access: premise refuted */
}
/* Round 21 / nandscan7: the BEAT-SELECT experiment.
 *
 * Why: on hardware the FIRST page read of a boot delivers real data and every
 * later one returns zeros or a status byte, rc 0 throughout (run25..30). Six
 * experiments have ruled out the data-phase register set, the transfer shape,
 * the read ordering (including payload-without-metadata), a controller reset, a
 * chip 0xff reset, a FIFO/ECC reset (+0x81c), deselect, PPN SET FEATURES and a
 * full re-probe. One thing this driver has NEVER written is FMC+0x18.
 *
 * EXTERNAL, LLM-GENERATED, POSSIBLY WRONG (n31firmwarespecs,
 * Architecture/NAND_STORAGE.md) claims +0x18 is a BEAT SELECT: 1 = spare/
 * metadata beats, 2 = data beats, and that a page moves as sixteen 1 KiB
 * BCH-protected units with a 52-byte parity/status beat and a 1,024-byte data
 * beat. That document's geometry agrees with our own probe (2 CEs x 2 CAU x
 * 2,088 blocks, 128 pages, 16,384+64 = 16,448), but NOTHING else in it has been
 * verified against our hardware, and it is not trusted here. If +0x18 selects
 * the beat type, then never writing it leaves whatever the previous user (the
 * boot ROM / RetailOS) selected, which would explain exactly the observed
 * "first read fine, then nothing" pattern.
 *
 * Every variant below runs the SAME command/address/ready sequence as today's
 * page read (0x0a, 4 address bytes, 0x37, 0x77, 0x7d, poll, 0x7a) and the SAME
 * data_in/pio_drain machinery. It differs ONLY in the FMC+0x18 value written
 * immediately before each data phase (and, for the optional +0x14 flag, in the
 * PIO drain's +0x14 value):
 *   0 BEAT_TODAY              never write +0x18 (the control: today's read)
 *   1 BEAT_SPLIT              +0x18=1 before the 16-byte metadata read,
 *                             +0x18=2 before each 1,024-byte payload read
 *   2 BEAT_DATA_ONLY          +0x18=2 before all reads including metadata;
 *                             NAND_BEAT_NOMETA skips metadata entirely (4 x
 *                             1,024 payload only)
 *   3 BEAT_SPARE_BOTH         +0x18=1 before all reads (the inverse control)
 *   4 BEAT_SPLIT_META_LAST    payload first with +0x18=2, metadata last with
 *                             +0x18=1
 *   5 BEAT_SPLIT_PLUS_77      variant 1 plus a trailing 0x77 after the payload,
 *                             before deselect (run30's leading hypothesis)
 *   6 BEAT_DATA_ONLY_PLUS_77  variant 2 with no metadata plus trailing 0x77
 *   7 BEAT_SPLIT_14_200       variant 1 but the payload drains with
 *                             FMC+0x14 = 0x200 instead of 0x100
 * Sub-flags may be OR-ed into `variant` (the low 4 bits are the base variant):
 *   NAND_BEAT_NOMETA (0x100)  skip metadata; valid for base 2 and 6 only
 *   NAND_BEAT_14_200 (0x200)  payload drain +0x14 = 0x200 (bit 9)
 *   NAND_BEAT_14_010 (0x400)  payload drain +0x14 = 0x010 (bit 4)
 * The +0x14 value is UNKNOWN and this is a probe, not a fix: RetailOS's helper
 * computes 1 << (arg + 8), and only arg = 0 (0x100) has ever been observed;
 * 0x200 would be arg = 1 and 0x10 cannot come from that formula at all (it is
 * not a bit at or above 8), so 0x10 is the "bit 4" hypothesis recorded from the
 * arming write, not evidence. The external document says nothing about +0x14.
 *
 * Every variant forces passive mode, calls no write/erase function, and every
 * wait is bounded (wt()/pio_drain14). Unknown variant/flags return the distinct
 * NAND_BEAT_BAD (-11) before touching hardware; bad ce/pointers NAND_BEAT_BADARGS
 * (-3). Like nand_read_raw, raw_addr is passed to the 4-byte address field
 * unchanged (the nandscan2 property). */
#define BEAT_14_100 0x100u
#define BEAT_14_200 0x200u
#define BEAT_14_010 0x10u
/* one data phase with an optional +0x18 write before arming and an explicit
 * PIO drain +0x14 value; the arming/drain otherwise is data_in()'s */
static int data_in_beat(uint32_t *buf, unsigned len, int set18, uint32_t w18,
                        uint32_t v14) {
    if (set18) FMC(0x18) = w18;
    FMC(0x30) = len - 1;
    FMC(0x14) = 1;
    FMC(0x24) = 0;
    FMC(0x04) = 0x22;
    int r = wt(8);
    if (r) return r;                    /* arming failed: no FIFO pops */
    return pio_drain14(buf, len, v14);
}
static int read_page_beat(unsigned ce, uint32_t raw_addr, void *buf,
                          uint32_t meta[4], unsigned variant, unsigned opts,
                          uint32_t v14) {
    uint32_t *w = (uint32_t *)buf;
    int skip_meta = (opts & NAND_BEAT_NOMETA) != 0;
    nand.ph = 0;
    for (unsigned i = 0; i < 10; ++i) nand.pf[i] = 0;
    FMC(0x38) = 7;
    sel_tm(ce, RUN_TM);
    FMC(0x48) = 0x0ff00ffeu;
    FMC(0x81c) = 1;
    int r = cmd(0x0a);
    if (!r) r = addr_lo(4, raw_addr);
    if (!r) r = cmd(0x37);
    if (!r) r = cmd(0x77);
    if (!r) r = cmd(0x7d);
    if (!r) r = poll();
    int st_fail = !r && (nand.st & 0x01);
    if (st_fail) failed(0x01);
    if (!r) r = cmd(0x7a);
    switch (variant) {
    case NAND_BEAT_TODAY:
        /* never write +0x18, but the optional +0x14 payload selector still
         * applies: data_in_beat(..., set18 = 0) is byte-for-byte data_in()
         * when no +0x14 flag is given */
        if (!r) r = data_in_beat(meta, 16, 0, 0, BEAT_14_100);
        for (unsigned k = 0; !r && k < 4; ++k)
            r = data_in_beat(w + 256 * k, 1024, 0, 0, v14);
        break;
    case NAND_BEAT_SPLIT:
    case NAND_BEAT_SPLIT_PLUS_77:
    case NAND_BEAT_SPLIT_14_200:
        if (!r) r = data_in_beat(meta, 16, 1, 1, BEAT_14_100);
        for (unsigned k = 0; !r && k < 4; ++k)
            r = data_in_beat(w + 256 * k, 1024, 1, 2, v14);
        if (variant == NAND_BEAT_SPLIT_PLUS_77 && !r) r = cmd(0x77);
        break;
    case NAND_BEAT_DATA_ONLY:
        if (!skip_meta && !r) r = data_in_beat(meta, 16, 1, 2, BEAT_14_100);
        for (unsigned k = 0; !r && k < 4; ++k)
            r = data_in_beat(w + 256 * k, 1024, 1, 2, v14);
        break;
    case NAND_BEAT_SPARE_BOTH:
        if (!r) r = data_in_beat(meta, 16, 1, 1, BEAT_14_100);
        for (unsigned k = 0; !r && k < 4; ++k)
            r = data_in_beat(w + 256 * k, 1024, 1, 1, v14);
        break;
    case NAND_BEAT_SPLIT_META_LAST:
        for (unsigned k = 0; !r && k < 4; ++k)
            r = data_in_beat(w + 256 * k, 1024, 1, 2, v14);
        if (!r) r = data_in_beat(meta, 16, 1, 1, BEAT_14_100);
        break;
    case NAND_BEAT_DATA_ONLY_PLUS_77:
        for (unsigned k = 0; !r && k < 4; ++k)
            r = data_in_beat(w + 256 * k, 1024, 1, 2, v14);
        if (!r) r = cmd(0x77);
        break;
    default:
        break;                          /* unreachable: entry point rejects it */
    }
    desel();
    return r ? r : st_fail ? -2 : 0;
}
/* Round 21 / nandscan7 entry point. Read-only: ce is bounded, buf must be
 * non-null, and meta may be NULL only when no metadata read happens (base 2
 * with NAND_BEAT_NOMETA, or base 6 which never reads it). An unknown
 * variant/flag combination returns NAND_BEAT_BAD (-11) and touches NOTHING;
 * bad arguments return NAND_BEAT_BADARGS (-3). Passive mode is forced BEFORE
 * anything runs, and no write/erase function is reachable. */
int nand_read_beat(unsigned ce, uint32_t raw_addr, void *buf, uint32_t meta[4],
                   unsigned variant) {
    unsigned base = variant & NAND_BEAT_VARIANT_MASK;
    unsigned opts = variant & ~NAND_BEAT_VARIANT_MASK;
    unsigned sel = (opts & NAND_BEAT_14_MASK) >> NAND_BEAT_14_SHIFT;
    int need_meta;
    uint32_t v14;
    if (ce > 1u || !buf) return NAND_BEAT_BADARGS;
    if (opts & ~(NAND_BEAT_NOMETA | NAND_BEAT_14_MASK)) return NAND_BEAT_BAD;
    if (base >= NAND_BEAT_COUNT) return NAND_BEAT_BAD;
    if (sel > 2u) return NAND_BEAT_BAD;     /* 3 = unused +0x14 selector value */
    if ((opts & NAND_BEAT_NOMETA) && base != NAND_BEAT_DATA_ONLY &&
        base != NAND_BEAT_DATA_ONLY_PLUS_77) return NAND_BEAT_BAD;
    need_meta = !(base == NAND_BEAT_DATA_ONLY_PLUS_77 ||
                  (base == NAND_BEAT_DATA_ONLY && (opts & NAND_BEAT_NOMETA)));
    if (need_meta && !meta) return NAND_BEAT_BADARGS;
    v14 = sel == 1u ? BEAT_14_200 : sel == 2u ? BEAT_14_010 : BEAT_14_100;
    if (base == NAND_BEAT_SPLIT_14_200 && sel == 0u) v14 = BEAT_14_200;
    nand_passive = 1;
    return read_page_beat(ce, raw_addr, buf, meta, base, opts, v14);
}
/* ======================================================================
 * Round 22 / nandscan8, PART A: ONE long bulk drain of the page stream.
 *
 * Why: exactly ONE page read per boot returns real data (run25..31), and
 * every earlier round spent it on a 4 KiB payload delivered as five separate
 * data_in() phases (16 B metadata + 4 x 1,024 B). That answers "what are the
 * first 4 KiB", which we already knew. It cannot answer what the transfer's
 * TRUE layout is: where the real data, the parity and the metadata sit in the
 * page's byte stream, or whether the 4 KiB our normal read returns is the
 * beginning of the page. The external (LLM-generated, possibly wrong) document
 * claims 16,384 B main + 64 B spare delivered as sixteen ~1 KiB BCH-protected
 * units each with a parity/status beat, plus a separate 16 B per 4 KiB sector
 * metadata allocation - which does not fit together (16 x 52 = 832 B parity vs
 * 64 B spare), so the real layout is unknown.
 *
 * What: today's EXACT command/address/ready sequence (FMC+0x38 = 7, chip
 * select at RUN_TM, +0x48 = 0x0ff00ffe, +0x81c = 1, 0x0a, four address bytes,
 * 0x37, 0x77, 0x7d, status poll, 0x7a) and then ONE data phase of `words`
 * 32-bit words: one arming (+0x30 = words*4 - 1, +0x14 = 1, +0x24 = 0,
 * +0x04 = 0x22, wait bit 8) and ONE pio_drain of the whole length, i.e. no
 * per-1 KiB re-arming at all. With flags = 0 the drain starts at stream byte
 * 0 of the page, so the returned stream contains whatever the normal read's
 * 16-byte metadata phase would have consumed first; NAND_BULK_META_FIRST
 * additionally performs today's 16-byte metadata data_in() first, which is the
 * comparison shape (does a 16-byte read before the bulk change the stream?).
 *
 * Read-only: no write/erase/program function is reachable, every wait is
 * bounded (wt()/pio_body's stall counter), `ce` is bounded, `words` is bounded
 * by NAND_BULK_MAX_WORDS and passive mode is forced before anything runs.
 * `raw_addr` is passed to the 4-byte address field unchanged (the nandscan2
 * property). The buffer is never cleared: a word that is never popped keeps
 * whatever the caller pre-filled it with, which is what lets the fixture count
 * words that were NOT delivered. */
int nand_read_bulk(unsigned ce, uint32_t raw_addr, void *buf, uint32_t words,
                   uint32_t *meta4, unsigned flags) {
    if (ce > 1u || !buf) return NAND_BULK_BADARGS;
    if (flags & ~(unsigned)NAND_BULK_META_FIRST) return NAND_BULK_BADFLAGS;
    if (!words || words > NAND_BULK_MAX_WORDS) return NAND_BULK_BADWORDS;
    if ((flags & NAND_BULK_META_FIRST) && !meta4) return NAND_BULK_BADARGS;
    nand.ph = 0;
    for (unsigned i = 0; i < 10; ++i) nand.pf[i] = 0;
    nand_passive = 1;
    FMC(0x38) = 7;
    sel_tm(ce, RUN_TM);
    FMC(0x48) = 0x0ff00ffeu;
    FMC(0x81c) = 1;
    int r = cmd(0x0a);
    if (!r) r = addr_lo(4, raw_addr);
    if (!r) r = cmd(0x37);
    if (!r) r = cmd(0x77);
    if (!r) r = cmd(0x7d);
    if (!r) r = poll();
    int st_fail = !r && (nand.st & 0x01);
    if (st_fail) failed(0x01);
    if (!r) r = cmd(0x7a);
    if (!r && (flags & NAND_BULK_META_FIRST)) r = data_in(meta4, 16);
    if (!r) r = data_in((uint32_t *)buf, words * 4u);   /* ONE arming, ONE drain */
    desel();
    return r ? r : st_fail ? -2 : 0;
}
/* ======================================================================
 * Round 23 / nandscan9: the SINGLE-ARMED-PHASE page read.
 *
 * run32 Part A (hardware, hw-reports/run32.txt): ONE armed 17,408-byte transfer
 * of CE0 page 0 returned rc 0, sent 0 (every word delivered) and the stream
 *   word 0..3 69f5134b 816d19e4 c2034e36 daeedecd   <- 16 B metadata FIRST
 *   word 4..7 3adc3861 e02d0015 1c2dc5d3 29544baf   <- then the data
 * i.e. the page layout is linear metadata-then-data and a single-armed-phase
 * drain of many KB works. Our normal page read (read_page_phase) instead uses
 * FIVE separately armed phases: the 16-byte metadata data_in() and four 1 KiB
 * payload data_in() calls, and after the first read of a boot every later read
 * returns 0xcf filler then zeros (run25..31).
 *
 * LEADING HYPOTHESIS: repeated arming is what breaks - each re-arm may leave
 * the datapath in a state the next arm cannot recover from - so a page read
 * that arms ONCE may work repeatedly. nand_read_single exists to test exactly
 * that with the smallest possible change: the command/address/ready sequence is
 * BYTE-IDENTICAL to read_page_phase's (0x0a, 4 address bytes, 0x37, 0x77,
 * 0x7d, poll, 0x7a), then exactly ONE arming (+0x30 = words*4 - 1, +0x14 = 1,
 * +0x24 = 0, +0x04 = 0x22, wait bit 8) and ONE pio_body drain of `words` 32-bit
 * words - the same two calls nand_read_bulk makes (data_in()). The PIO body,
 * its +0x00 mask, its counter reset, its bounded 4,096-stall counter and its
 * end-of-phase ack are the SHARED pio_body()/pio_drain(); only the FRAMING
 * differs per variant:
 *   0 SINGLE_PLAIN     drain then deselect - the bulk read as it is today
 *   1 SINGLE_77        drain, then cmd(0x77), then deselect (run30's lead:
 *                      terminate the transfer the way RetailOS does)
 *   2 SINGLE_77_7D     drain, then 0x77 and 0x7d, then deselect
 *   3 SINGLE_NO_DESEL  drain and STOP: the chip stays selected so a caller can
 *                      continue the same transaction
 *   4 SINGLE_FULL_PAGE ignores the caller's `words` and reads the FULL page:
 *                      nand.page_size / 4 words (16,448 B = 4,112 words on the
 *                      hardware part, 16,384 B = 4,096 in NanoEmu). The number
 *                      of words actually delivered is reported through
 *                      nand_single_got.
 * No variant program/erases, sends an erase/program opcode or writes CLKCON or
 * the PSC: passive mode is forced before any hardware access, every wait goes
 * through wt() (bounded) and the drain's stall counter is bounded. `raw_addr`
 * is passed to the 4-byte address field unchanged (the nandscan2 property).
 * Bad ce/pointer -> NAND_SINGLE_BADARGS (-3); unknown variant ->
 * NAND_SINGLE_BADVARIANT (-15) and BADWORDS (-16) are returned BEFORE touching
 * hardware. Otherwise 0 / -1 / -2 exactly as the normal page read.
 * The caller's buffer is never cleared: a word the drain did not deliver keeps
 * whatever the caller prefilled, which is what lets the fixture separate
 * delivered 0xFF from never-popped words. */
volatile uint32_t nand_single_got;      /* words the last drain delivered */
static int single_data_in(uint32_t *buf, unsigned len) {
    FMC(0x30) = len - 1;
    FMC(0x14) = 1;
    FMC(0x24) = 0;
    FMC(0x04) = 0x22;
    int r = wt(8);
    if (r) { pio_words = 0; return r; } /* arming failed: no FIFO pops */
    return pio_drain(buf, len);         /* shared body; ONE phase, no re-arm */
}
int nand_read_single(unsigned ce, uint32_t raw_addr, void *out, uint32_t words,
                     unsigned variant) {
    int r, st_fail, tr = 0;
    uint32_t *w = (uint32_t *)out;
    if (variant >= NAND_SINGLE_COUNT) return NAND_SINGLE_BADVARIANT;
    if (ce > 1u || !out) return NAND_SINGLE_BADARGS;
    if (variant == NAND_SINGLE_FULL_PAGE) {
        uint32_t ps = nand.page_size;   /* probed geometry: the real page size */
        if (!ps || (ps & 3u) || ps / 4u > NAND_SINGLE_MAX_WORDS)
            return NAND_SINGLE_BADWORDS;
        words = ps / 4u;
    } else if (!words || words > NAND_SINGLE_MAX_WORDS) {
        return NAND_SINGLE_BADWORDS;
    }
    nand_single_got = 0;
    pio_words = 0;                      /* no drain yet: nothing delivered */
    nand.ph = 0;
    for (unsigned i = 0; i < 10; ++i) nand.pf[i] = 0;
    nand_passive = 1;
    FMC(0x38) = 7;
    sel_tm(ce, RUN_TM);
    FMC(0x48) = 0x0ff00ffeu;
    FMC(0x81c) = 1;
    r = cmd(0x0a);
    if (!r) r = addr_lo(4, raw_addr);
    if (!r) r = cmd(0x37);
    if (!r) r = cmd(0x77);
    if (!r) r = cmd(0x7d);
    if (!r) r = poll();
    st_fail = !r && (nand.st & 0x01);
    if (st_fail) failed(0x01);
    if (!r) r = cmd(0x7a);
    if (!r) r = single_data_in(w, words * 4u);   /* ONE arming, ONE drain */
    nand_single_got = pio_words;
    if (variant == NAND_SINGLE_77 || variant == NAND_SINGLE_77_7D) {
        tr = cmd(0x77);                 /* terminate the transfer (RetailOS) */
        if (variant == NAND_SINGLE_77_7D && !tr) tr = cmd(0x7d);
        if (!r) r = tr;                 /* the first error is the one reported */
    }
    if (variant != NAND_SINGLE_NO_DESEL) desel();
    return r ? r : st_fail ? -2 : 0;
}
/* ======================================================================
 * Round 25 / nandscan10: the SAME one-arming + one-drain page read as
 * nand_read_single/nand_read_bulk, exposing (a) which drain gate is used and
 * (b) the transfer length, plus a separate spare-record capture path.
 *
 * WHY (two hardware observations and one external hardware document):
 *  - run32 Part A: the FIRST read after the probe, one armed 4,352-word
 *    (17,408-byte) transfer of CE0 page 0, returned rc 0, sent 0, nz 14 and a
 *    stream beginning 69f5134b 816d19e4 c2034e36 daeedecd 3adc3861 e02d0015
 *    1c2dc5d3 29544baf then zeros. That stream is used here as a REFERENCE
 *    SIGNATURE only: nothing established that it is true correct page content.
 *  - run33: the FIRST read after the probe, one armed 1,028-word transfer of
 *    the same page, returned nz 37 and a materially different stream
 *    (`0 0 0 8000`, then zero data words); twelve repeats degraded to
 *    0xcfcfcfcf then zeros. run32 and run33 were different boots AND different
 *    builds, so transfer LENGTH is a hypothesis, never an established cause.
 *  - The hardware-annotated N31 document's "PIO read and the gate-0 drain [H]"
 *    section documents the stock drain as sampling FIFOCTR BEFORE the pop and
 *    retaining word n only when the sampled low byte equals 4*n, and says the
 *    old 4*n+4 test "popped and discarded word zero of every transfer". Our
 *    historical drain used exactly that old test with the counter read after
 *    the pop, so pio_drain_mode's FIXED gate above is the candidate root cause;
 *    this entry point can stage the corrected gate and the historical gate as
 *    separate first-read experiments on separate boots.
 *
 * Drain variants (one arming, one drain; the command/address/ready sequence is
 * identical for all of them):
 *   0 NAND_WORDS_BULK          the FIXED gate: sample FIFOCTR before the pop,
 *                              low byte == 4*i, discard a mismatched candidate,
 *                              bounded 4,096-stall guard. The default headline.
 *   1 NAND_WORDS_NOWRAP_CHECK  pop `words` words unconditionally: no counter
 *                              comparison at all, the hard loop bound `words`
 *                              is the only cap. popped = words,
 *                              counter-confirmed = 0 by construction.
 *   2 NAND_WORDS_32BIT_COUNTER the FIXED gate compared as the full 32-bit
 *                              value FIFOCTR == 4*i, so it cannot alias at the
 *                              64-word/256-byte low-byte wrap.
 *   3 (WITHDRAWN)              was NAND_WORDS_STALL_RESYNC. A "dummy read" is
 *                              impossible: a pop always consumes the FIFO, and
 *                              the old gate already discarded a mismatched word
 *                              by overwriting buf[i] on the retry, so nothing
 *                              was ever committed at that index. A
 *                              scratch/commit-on-match variant is byte-for-byte
 *                              identical in output and tests nothing. The
 *                              request is rejected with NAND_WORDS_WITHDRAWN
 *                              (-19); its two useful roles are covered by
 *                              variant 2 (full-width gate) and variant 1
 *                              (unconditional pop, popped vs confirmed split).
 *   4 NAND_WORDS_HALF          the FIXED gate, transfer fixed at 2,176 words
 *                              (half of run32's 4,352); `words` is ignored.
 *   5 NAND_WORDS_DOUBLE        the FIXED gate, transfer fixed at 8,704 words
 *                              (double); `words` is ignored.
 *   6 NAND_WORDS_OLD_GATE      the HISTORICAL gate (pop, then low byte ==
 *                              4*i+4) as an explicit control. This is the gate
 *                              the document blames; it is kept only so a boot
 *                              can be spent on it in isolation.
 *   7 NAND_WORDS_BULK_77       exactly variant 0, then cmd(0x77) before
 *                              deselect: the identical transfer with only the
 *                              terminator changed.
 *
 * Every variant forces passive mode before any hardware access, performs no
 * write/erase/program (no 0x80/0x8a/0x87/0xb0/0x17 opcode is emitted), bounds
 * every wait (wt()) and every loop (`words` plus, where kept, the 4,096-stall
 * guard). nand_words_popped / nand_words_confirmed / nand_words_got expose the
 * number of FIFO pops, the number of words whose counter matched (committed by
 * the gate) and the number retained in the buffer, so a gate that discards
 * words is visible as popped > confirmed.
 *
 * Bounds/rejections BEFORE touching hardware: ce > 1 or null buf ->
 * NAND_WORDS_BADARGS (-3); unknown variant or the withdrawn slot ->
 * NAND_WORDS_BADVARIANT (-17) / NAND_WORDS_WITHDRAWN (-19); words = 0 or >
 * NAND_WORDS_MAX_WORDS (8,704) -> NAND_WORDS_BADWORDS (-18). Variants 4 and 5
 * fix their own length and ignore `words`. Return 0 / -1 / -2 as the normal
 * page read. */
volatile uint32_t nand_words_got;        /* retained / counter-confirmed words */
volatile uint32_t nand_words_popped;     /* FIFO pops (retries included) */
volatile uint32_t nand_words_confirmed;  /* words whose FIFOCTR matched */
static int words_drain(uint32_t *buf, unsigned len, unsigned variant) {
    unsigned mode = variant == NAND_WORDS_NOWRAP_CHECK ? PIO_GATE_NONE :
                    variant == NAND_WORDS_32BIT_COUNTER ? PIO_GATE_FULL32 :
                    variant == NAND_WORDS_OLD_GATE ? PIO_GATE_OLD : PIO_GATE_FIXED;
    int r = pio_drain_mode(buf, len, mode);
    nand_words_got = pio_words;
    nand_words_popped = pio_popped;
    nand_words_confirmed = pio_confirmed;
    return r;
}
/* one arming (the same writes single_data_in makes) then the variant's drain */
static int words_phase(uint32_t *buf, unsigned len, unsigned variant) {
    FMC(0x30) = len - 1u;
    FMC(0x14) = 1;
    FMC(0x24) = 0;
    FMC(0x04) = 0x22;
    int r = wt(8);
    if (r) {
        pio_words = 0;
        nand_words_got = 0;
        nand_words_popped = 0;
        nand_words_confirmed = 0;
        return r;
    }
    return words_drain(buf, len, variant);
}
int nand_read_words(unsigned ce, uint32_t raw_addr, void *buf, uint32_t words,
                    unsigned variant) {
    int r, st_fail;
    uint32_t eff;
    if (variant == NAND_WORDS_STALL_RESYNC) return NAND_WORDS_WITHDRAWN;
    if (variant >= NAND_WORDS_COUNT) return NAND_WORDS_BADVARIANT;
    if (ce > 1u || !buf) return NAND_WORDS_BADARGS;
    if (variant == NAND_WORDS_BOOT_STOCK &&
        (words != NAND_BOOT_WORDS || ((uintptr_t)buf & 3u)))
        return NAND_WORDS_BADWORDS;
    if (variant == NAND_WORDS_HALF) eff = NAND_WORDS_BASE / 2u;
    else if (variant == NAND_WORDS_DOUBLE) eff = NAND_WORDS_BASE * 2u;
    else {
        if (!words || words > NAND_WORDS_MAX_WORDS) return NAND_WORDS_BADWORDS;
        eff = words;
    }
    nand_words_got = 0;
    nand_words_popped = 0;
    nand_words_confirmed = 0;
    pio_words = 0;                      /* no drain yet: nothing delivered */
    nand.ph = 0;
    for (unsigned i = 0; i < 10; ++i) nand.pf[i] = 0;
    nand_passive = 1;
    FMC(0x38) = 7;
    sel_tm(ce, RUN_TM);
    FMC(0x48) = 0x0ff00ffeu;
    FMC(0x81c) = 1;
    r = cmd(0x0a);
    if (!r) r = addr_lo(4, raw_addr);
    if (!r) r = cmd(0x37);
    if (!r) r = cmd(0x77);
    if (!r) r = cmd(0x7d);
    if (!r) r = poll();
    st_fail = !r && (nand.st & 0x01);
    if (st_fail) failed(0x01);
    if (!r) r = cmd(0x7a);
    if (!r && variant == NAND_WORDS_BOOT_STOCK) {
        /* 0x084ed600, non-ECC branch, verified from snap6 instructions:
         * THREE 1024-byte blocks (cmp r5,#3 at 0x084ed744), no leading
         * metadata phase, then 0x77 (0x084ed74c). This is a boot-area
         * reader, NOT a 4096-byte SFTL sector or a 16 KiB PPN reader.
         * The timing table at 0x0896e904 contains 0x20010000 (bit 12 clear).
         * Keep the existing bounded waits and status-failure reporting. */
        uint32_t got = 0, popped = 0, confirmed = 0;
        for (unsigned k = 0; !r && k < 3; ++k) {
            pio_words = pio_popped = pio_confirmed = 0;
            r = data_in_retailos((uint32_t *)buf + k * 256u, 1024u);
            got += pio_words;
            popped += pio_popped;
            confirmed += pio_confirmed;
        }
        nand_words_got = got;
        nand_words_popped = popped;
        nand_words_confirmed = confirmed;
        if (!r) r = cmd(0x77);
    } else if (!r) r = words_phase((uint32_t *)buf, eff * 4u, variant);
    if (!r && variant == NAND_WORDS_BULK_77) r = cmd(0x77);   /* terminator only */
    desel();
    return r ? r : st_fail ? -2 : 0;
}
/* ======================================================================
 * Round 25 / nandscan10: SPARE-RECORD capture via REC_LATCH.
 *
 * The hardware-annotated N31 document's "Spare-record capture [H]" section:
 * draining the nominal spare beat from the PIO FIFO returns stale FIFO bytes,
 * not the record, and reading SPR0..3 immediately returns zero. The working
 * sequence mirrors stock:
 *   1. issue the stock spare-beat setup (FMLEN = 0xF, FMUNK58 = 0x20,
 *      FMCTRL1 = 0x32);
 *   2. set FMUNK58 = 1 and start the first data beat;
 *   3. write REC_LATCH = 0x1102 and poll bit 1 clear;
 *   4. read the sector's record from SPR0..3.
 * One 16-byte record is latched per step 3/4; four of them form the page's
 * 64-byte out-of-band area. The document does not give the length of "the
 * first data beat", so this uses a 4 KiB beat (FMLEN = 0xFFF); that choice is
 * stated as UNKNOWN, not evidence. The latch is repeated four times because the
 * document describes one record per latch and four records per page; the
 * records are reported with their parsed fields and no claim is made that the
 * repetition matches stock exactly. This path does NOT drain the page payload.
 *
 * Read-only: the command sequence is the page read (0x0a, four address bytes,
 * 0x37, 0x77, 0x7d, poll, 0x7a) plus the feature/beat setup above; no
 * write/erase/program opcode is emitted, every wait is bounded
 * (wait_until/WAIT_US) and passive mode is forced. rec[k][0..3] = SPR0..3 of
 * capture k, in register order (little-endian words). Returns 0 / -1 / -2 as
 * the page read, NAND_REC_BADARGS (-3) for ce >= 2 or a null rec. */
int nand_read_record(unsigned ce, uint32_t raw_addr, uint32_t rec[4][4]) {
    int r, st_fail;
    if (ce > 1u || !rec) return NAND_REC_BADARGS;
    for (unsigned k = 0; k < 4; ++k)
        for (unsigned i = 0; i < 4; ++i) rec[k][i] = 0;
    nand.ph = 0;
    for (unsigned i = 0; i < 10; ++i) nand.pf[i] = 0;
    nand_passive = 1;
    FMC(0x38) = 7;
    sel_tm(ce, RUN_TM);
    FMC(0x48) = 0x0ff00ffeu;
    FMC(0x81c) = 1;
    r = cmd(0x0a);
    if (!r) r = addr_lo(4, raw_addr);
    if (!r) r = cmd(0x37);
    if (!r) r = cmd(0x77);
    if (!r) r = cmd(0x7d);
    if (!r) r = poll();
    st_fail = !r && (nand.st & 0x01);
    if (st_fail) failed(0x01);
    if (!r) r = cmd(0x7a);
    if (!r) {
        /* 1. stock spare-beat setup */
        FMC(0x30) = 0xF;
        FMC(0x58) = 0x20;
        FMC(0x04) = 0x32;
        /* bounded settle only; the latch poll below is the real completion */
        (void)wait_until(&FMC(0x48), 2u, 2u, WAIT_US);
        FMC(0x48) = 2u;
        /* 2. FMUNK58 = 1 and start the first data beat (4 KiB; length UNKNOWN) */
        FMC(0x58) = 1;
        FMC(0x30) = 0xFFF;
        FMC(0x14) = 1;
        FMC(0x24) = 0;
        FMC(0x04) = 0x22;
        r = wt(8);
    }
    for (unsigned k = 0; !r && k < 4; ++k) {
        /* 3. extract the record and poll bit 1 clear (bounded) */
        FMC(0x40) = 0x1102;
        if (wait_until(&FMC(0x40), 2u, 0u, WAIT_US) != WAIT_OK) {
            failed(0x02);
            r = -1;
            break;
        }
        /* 4. the sector's record */
        for (unsigned i = 0; i < 4; ++i) rec[k][i] = FMC(0x60 + 4u * i);
    }
    desel();
    return r ? r : st_fail ? -2 : 0;
}
/* ======================================================================
 * Round 22 / nandscan8, PART B: RetailOS's ECC-branch ARMING combined with a
 * data-beat select. Never tried before: run28/29/31 varied the arming and
 * +0x18/+0x14 separately, and nandscan5's shape 3 used the ECC-style
 * +0x30 = 0x34/+0x24 = 0x400 arming with today's +0x14 = 1 (not 0x10) and
 * never with a +0x18 beat value. RetailOS's ECC branch sets
 * FMC+0x30 = 0x34, FMC+0x14 = 0x10, FMC+0x24 = 0 and then runs the BCH decode
 * sequence at 0x084cbd14 (which nandscan6 established is the hardware ECC
 * DECODE step, not a data mover - it is deliberately NOT run here; this tests
 * the ARMING plus a beat select, not the decoder).
 *
 * Variants (all on the same page read path as nand_read_beat; the arming is
 * applied to the 16 B metadata phase and all four 1 KiB payload phases, as
 * nand_read_phase does, so results are comparable):
 *   E0 NAND_ECC_E0  today's arming (control): +0x30 = len-1, +0x14 = 1,
 *                   +0x24 = 0, +0x04 = 0x22, drain +0x14 = 0x100
 *   E1 NAND_ECC_E1  +0x30 = 0x34, +0x14 = 0x10, +0x18 = 2
 *   E2 NAND_ECC_E2  +0x30 = 0x34, +0x14 = 0x10, +0x18 = 1
 *   E3 NAND_ECC_E3  +0x30 = 0x34, +0x14 = 0x10, +0x18 untouched
 *   E4 NAND_ECC_E4  +0x30 = 0x34, +0x14 = 0x100, +0x18 = 2
 *   E5 NAND_ECC_E5  +0x30 = 0x1023, +0x14 = 0x10, +0x18 = 2
 *   E6 NAND_ECC_E6  as E1 but the drain does NOT re-write +0x30 = 3: one
 *                   armed phase (+0x30 = 0x34) then the PIO loop, which still
 *                   applies the +0x00 mask and the +0x14 = 0x10 selector
 *   E7 NAND_ECC_E7  as E1 but deselect by writing FMC+0x00 = 0, not 1
 *                   (RetailOS's own page reader writes 0)
 * The drain's +0x14 is the variant's armed +0x14 for E1..E6 (E4: 0x100, the
 * rest 0x10) and today's 0x100 for E0, because the alternative - always 0x100
 * - would overwrite the +0x14 value the variant exists to test. This choice is
 * documented in the README; it is a deliberate, stated design decision, not
 * evidence about the register. */
/* one data phase: optional +0x18 write, explicit +0x30/+0x14 arming, then the
 * shared drain body; rearm30 = 0 keeps the armed +0x30 (E6) */
static int data_in_ecc(uint32_t *buf, unsigned len, unsigned variant) {
    uint32_t v30 = len - 1, v14 = 1, drain14 = 0x100;
    int set18 = 0, rearm30 = 1;
    uint32_t w18 = 0;
    switch (variant) {
    case NAND_ECC_E1: case NAND_ECC_E2: case NAND_ECC_E7:
        set18 = 1; w18 = variant == NAND_ECC_E2 ? 1u : 2u;
        v30 = 0x34; v14 = drain14 = 0x10;
        break;
    case NAND_ECC_E3:
        v30 = 0x34; v14 = drain14 = 0x10;
        break;
    case NAND_ECC_E4:
        set18 = 1; w18 = 2;
        v30 = 0x34; v14 = drain14 = 0x100;
        break;
    case NAND_ECC_E5:
        set18 = 1; w18 = 2;
        v30 = 0x1023; v14 = drain14 = 0x10;
        break;
    case NAND_ECC_E6:
        set18 = 1; w18 = 2;
        v30 = 0x34; v14 = drain14 = 0x10; rearm30 = 0;
        break;
    default: break;                     /* E0: today's arming */
    }
    if (set18) FMC(0x18) = w18;
    FMC(0x30) = v30;
    FMC(0x14) = v14;
    FMC(0x24) = 0;
    FMC(0x04) = 0x22;
    int r = wt(8);
    if (r) return r;                    /* arming failed: no FIFO pops */
    if (rearm30) FMC(0x30) = 3;
    FMC(0x14) = drain14;
    return pio_body(buf, len);
}
static int read_page_ecc(unsigned ce, uint32_t raw_addr, void *buf,
                         uint32_t meta[4], unsigned variant) {
    uint32_t *w = (uint32_t *)buf;
    nand.ph = 0;
    for (unsigned i = 0; i < 10; ++i) nand.pf[i] = 0;
    FMC(0x38) = 7;
    sel_tm(ce, RUN_TM);
    FMC(0x48) = 0x0ff00ffeu;
    FMC(0x81c) = 1;
    int r = cmd(0x0a);
    if (!r) r = addr_lo(4, raw_addr);
    if (!r) r = cmd(0x37);
    if (!r) r = cmd(0x77);
    if (!r) r = cmd(0x7d);
    if (!r) r = poll();
    int st_fail = !r && (nand.st & 0x01);
    if (st_fail) failed(0x01);
    if (!r) r = cmd(0x7a);
    if (!r) r = data_in_ecc(meta, 16, variant);
    for (unsigned k = 0; !r && k < 4; ++k)
        r = data_in_ecc(w + 256 * k, 1024, variant);
    if (variant == NAND_ECC_E7) FMC(0x00) = 0;   /* RetailOS writes 0 */
    else desel();
    return r ? r : st_fail ? -2 : 0;
}
int nand_read_ecc(unsigned ce, uint32_t raw_addr, void *buf, uint32_t meta[4],
                  unsigned variant) {
    if (ce > 1u || !buf || !meta) return NAND_ECC_BADARGS;
    if (variant >= NAND_ECC_COUNT) return NAND_ECC_BADVARIANT;
    nand_passive = 1;
    return read_page_ecc(ce, raw_addr, buf, meta, variant);
}
/* Round 18 / nandscan3: read-only register snapshot for the diagnosis. The
 * offsets are the ones that describe the FMC's command/status/data state
 * (+0x80 is the PIO FIFO data port; the transfer has ended by the time a
 * caller takes this snapshot, so the pop is only diagnostic). nand.st is the
 * last status byte poll() recorded. Reads only: no write/erase, no flash. */
void nand_regs_snapshot(uint32_t out[16]) {
    static const uint16_t at[15] = {
        0x00, 0x04, 0x10, 0x14, 0x18, 0x24, 0x28,
        0x30, 0x38, 0x48, 0x4c, 0x80, 0x81c, 0xc00, 0xc08
    };
    if (!out) return;
    for (unsigned i = 0; i < 15; ++i) out[i] = FMC(at[i]);
    out[15] = nand.st;
}
/* EXPERIMENT (read-only): the nand_read_sector command sequence exactly,
 * then, if extra_words > 0, further data_in() PIO pulls of extra_words more
 * 32-bit words into extra[] (<= 1 KB per data_in call, as RetailOS's own
 * limit), to see whether the rest of the 16 KB page (3 x 4 KB + 16 B meta
 * each) comes out of the same transfer. No new command, no write. extra_words
 * is clamped to NAND_EXT_MAX_WORDS. Returns as nand_read_sector (0, -1 timeout,
 * -2 status fail bit 0); a stalled/expired extra pull returns -1 and leaves
 * what was read; nand_ext_got = words actually delivered into extra[]. */
volatile uint32_t nand_ext_got;
int nand_read_ext(unsigned ce, uint32_t page, void *buf, uint32_t meta[4],
                  uint32_t *extra, uint32_t extra_words) {
    nand.ph = 0;
    for (unsigned i = 0; i < 10; ++i) nand.pf[i] = 0;
    nand_ext_got = 0;
    if (extra_words > NAND_EXT_MAX_WORDS) extra_words = NAND_EXT_MAX_WORDS;
    if (!extra) extra_words = 0;
    FMC(0x38) = 7;
    sel_tm(ce, RUN_TM);
    FMC(0x48) = 0x0ff00ffeu;
    FMC(0x81c) = 1;
    int r = cmd(0x0a);
    if (!r) r = addr_lo(4, page);
    if (!r) r = cmd(0x37);
    if (!r) r = cmd(0x77);
    if (!r) r = cmd(0x7d);
    if (!r) r = poll();
    int st_fail = !r && (nand.st & 0x01);
    if (st_fail) failed(0x01);
    if (!r) r = cmd(0x7a);
    if (!r) r = data_in(meta, 16);
    for (unsigned k = 0; !r && k < 4; ++k)
        r = data_in((uint32_t *)buf + 256 * k, 1024);
    for (uint32_t done = 0; !r && done < extra_words;) {
        uint32_t n = extra_words - done;
        if (n > 256) n = 256;
        r = data_in(extra + done, n * 4);
        if (!r) { done += n; nand_ext_got = done; }
    }
    desel();
    return r ? r : st_fail ? -2 : 0;
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
 * the probe's first failed phase (wt bit), +0x48 and +0x00 then, +0xc08 +0xc60 +0xc64
 * +0xc6c now (round 8); fmc: +0x00..+0x7c
 * (not the FIFO); clk: CLKCON +0x00..+0x1c; ck: +0x40 +0x44 +0x64, GPIO
 * banks 12/14 config and level (I2C1 pads), sequencer +0xc10 irq enable */
static void raw_dump(void) {
    uint32_t w[20] = { nand.probes, nand.attempt, nand.err_at[0], nand.err_at[1],
                       nand.err_at[2], nand.st48, wait_stats.timeouts, wait_stats.spins,
                       (uint32_t)clk_result, FMC(0xc00), FMC(0xc04), FMC(0xc38),
                       nand.init, nand.ph, nand.pf[8], nand.pf[0],
                       FMC(0xc08), FMC(0xc60), FMC(0xc64), FMC(0xc6c) };
    rows("dg", w, 20);
    rows("fmc", &FMC(0x00), 32);
    rows("clk", &CLK(0x00), 8);
    w[0] = CLK(0x40); w[1] = CLK(0x44); w[2] = CLK(0x64); w[3] = GP(0x3cf00180u);
    w[4] = GP(0x3cf00184u); w[5] = GP(0x3cf001c0u); w[6] = GP(0x3cf001c4u); w[7] = FMC(0xc10);
    rows("ck", w, 8);
    /* round 14, only when +0x00 did not latch: the state before
     * recover() (snap: pwr 7 words, clk 10, pin 5, as listed there) */
    if (nand.cs != CS_PROBE) {
        rows("pwr", snap, 7);
        rows("clk", snap + 7, 10);
        rows("pin", snap + 17, 5);
    }
    /* rc: the 17 rc_log words, order as documented at rc_log (words 0..9
     * as round 13b, 10..16 the round-14 steps 2 and 3) */
    rc_log[9] = nand.rcv;
    rows("rc", rc_log, 17);
    rows("pin2", pin2, 5);      /* round 16: GPIO +c0 +e0 +f4 +100 +114 after nand_pins() */
    rows("pw", &nand.pwr, 2);   /* round 15: SET FEATURES result (0 not run, 1 done,
                                 * 2..7 the step that timed out, set_power()), status */
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
    /* pf: the read's first failed phase (0 = none) and FMC +00 +14 +18 +24
     * +28 +30 +38 +3c +48 +4c right after it (round 10); meta preset 0
     * and data word 0 5a5a5a5a, so a failed read shows */
    for (unsigned t = 0; ok && t < 2; ++t) {
        uint32_t m[4], *mt = t ? m : meta, pg = t ? (nand.blocks / 2) << (31 - __builtin_clz(nand.pages | 1)) : 0;
        buf[0] = 0x5a5a5a5au;
        mt[0] = mt[1] = mt[2] = mt[3] = 0;
        int r = nand_read_sector(0, pg, 0, buf, mt);
        if (!t) { *rd = r; nand.p0 = nand.ph | nand.st << 24; }
        fmt_str("page "); fmt_hex(pg, 8); fmt_str(" rd "); fmt_sdec(r);
        fmt_str(" st "); fmt_hex(nand.st, 2); fmt_str(" er "); fmt_dec(nand.errors);
        fmt_line(0, 0, 0);
        rows("pf", &nand.ph, 11);
        hexes(mt, 4);
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
    /* round 17: the mailbox is write-back cached (mmu.c, section 46);
     * only reset_stockos() cleaned it, so a hard reset or power-off lost
     * R81. Clean now, on every Drivers entry. */
    dcache_clean_all();
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
    if (nand.st & 0x10) return NAND_W_NOTBLANK;     /* round 15: no longer -2 */
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
