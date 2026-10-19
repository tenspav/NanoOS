#pragma GCC optimize("Os")   /* register pokes, no hot path: size first */
#include "nand.h"
#include "timer.h"
#include "fmt.h"
#include "report.h"
#include "clk.h"
#include "mmu.h"
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
 * type 1 = LBA) is the next, read-only step. */
#define FMC(o) (*(volatile uint32_t *)(0x38a00000u + (o)))
#define CLK(o) (*(volatile uint32_t *)(0x3c500000u + (o)))
#define WAIT_US 20000u
#define POLL 0x800000u
#define RPT_NAND_ID 81u
#define RUN_TM 0x20010000u
struct nand_info nand;
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
static int data_in(uint32_t *buf, unsigned len) {
    FMC(0x30) = len - 1;
    FMC(0x14) = 1;
    FMC(0x24) = 0;
    FMC(0x04) = 0x22;
    int r = wt(8);
    if (r) return r;                    /* arming failed: no FIFO pops */
    FMC(0x30) = 3;
    FMC(0x14) = 0x100;
    FMC(0x00) &= 0xfefffbffu;          /* bits 10, 24 (0x080d341a pool) */
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
 * nand.pwr_st the status byte. Once per boot (pwr_set); NAND_SET_POWER 0
 * leaves it out. */
#ifndef NAND_SET_POWER
#define NAND_SET_POWER 1
#endif
#if NAND_SET_POWER
static int pwr_set;
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
    if (ok && !pwr_set) { nand.pwr = set_power(0); pwr_set = 1; }
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
int nand_read_sector(unsigned ce, uint32_t page, unsigned sector,
                     void *buf, uint32_t meta[4]) {
    (void)sector;
    nand.ph = 0;
    for (unsigned i = 0; i < 10; ++i) nand.pf[i] = 0;
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
