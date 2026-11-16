#ifndef NANOOS_NAND_H
#define NANOOS_NAND_H
#include <stdint.h>
#define NAND_SECTOR 4096u
/* id/id2: the 8 READ ID bytes, little-endian ("PPN" 01 = 0x014e5050);
 * geometry from the parameter page; errors: expired waits / stalled PIO
 * words; cs: FMC +0x00 write-readback (round 13, below); st: last polled
 * status byte (bit 4 = error); tm: the chip-select timing that answered
 * READ ID */
struct nand_info {
    uint32_t id, id2, caus, blocks, pages, page_size, errors, cs, st, tm;
    /* round 7 (device regression): probes this boot, the attempt that
     * answered (1..3), FMC +0x48 as found, failed waits after each attempt */
    uint32_t probes, attempt, st48, err_at[3];
    /* round 8: ctl_init result (0 ok, else the step that timed out); the
     * first failed phase (wt bit: 2 cmd, 4 addr, 8 data arm, 0x100 PIO
     * stall, 0x800000 status; round 15: 0x01 status byte fail bit, read
     * still transferred) of the probe, or of the last page read (it
     * resets them, round 10); pf: FMC +00 +14 +18 +24 +28 +30 +38 +3c +48
     * +4c read right after that failure (pf[8] = +0x48, pf[0] = +0x00) */
    uint32_t init, ph, pf[10];
    /* round 11: page 0's ph | st << 24 (the self-test's first read; ph
     * is at most 0x800000), for the Drivers line */
    uint32_t p0;
    /* round 13 (device: "cs 00000000", every wait expired): cs is now
     * FMC +0x00 read back after writing CS_PROBE (nand.c), cs0 the value
     * as found; rcv (round 14, each step only after the previous failed):
     * 0 = latched at once, 1 = after the controller reset (ctl_init), 2 =
     * after CLKCON +0x1c clock setup, 3 = after the PSC domain-6 check /
     * wake, 4 = after the last-resort candidate clock gates, 5 = never */
    uint32_t cs0, rcv;
    /* round 15: PPN SET FEATURES power state 2 after the parameter page
     * (nand.c set_power): pwr 0 not run, 1 done, else the step that timed
     * out (2 0xef, 3 address, 4 data out, 5 e7/77/7d, 6 poll, 7 final 0x77);
     * pwr_st its status byte */
    uint32_t pwr, pwr_st;
    /* v16 normal PPN read diagnostics, separate from boot/FIFO counts. */
    uint32_t ppn_select, ppn_start, ppn_end, ppn_bytes, ppn_stage, ppn_power1;
};
extern struct nand_info nand;
/* Passive mode (nand.c): nonzero makes recover() stop after ctl_init's latch
 * check (no CLKCON/PSC writes). Game path sets it; default 0. */
extern volatile int nand_passive;
/* FIFOCTR trace - MEASUREMENT ONLY, changes no transfer behaviour. Records
 * exactly the counter value the old gate compares, so the open question
 * "what does FMC(+0x28) do at word 256?" can be answered from data rather
 * than inference. Enable it, do ONE read, then fetch. */
int nand_trace_enable(int on);
/* 4 words per entry: [0]=iteration i, [1]=sampled FMC(+0x28),
 * [2]=popped word, [3]=gate matched (1/0). Returns entries written. */
uint32_t nand_trace_fetch(uint32_t *dst, uint32_t max_entries);
/* iters, matches, mismatches, first_bad, last_bad, min_counter, max_counter,
 * samples_that_did_not_advance, stored_entries. */
void nand_trace_stats(uint32_t out[9]);
/* 1 if a PPN NAND answered on CE0 and its parameter page read. RetailOS's
 * controller reset first, then three attempts: slow timing after a 0xff
 * reset; runtime timing; slow 0xff reset after an FMC quiesce (nand.c).
 * nand.errors counts this probe's failed waits. */
int nand_probe(void);
/* Normal storage read, derived from live CS program 0x0896eb00.
 * Eight address bytes, sector 0..3, record latch + DMA, exactly 4096 bytes.
 * No NAND program/erase; passive mode is forced. Returns 0, -1 timeout,
 * -2 device failure, -3 invalid geometry/CE/row/sector/pointers.
 * Payload/meta are committed to caller buffers only after all phases finish.
 * DMA completion is transport evidence, not proof of correct file contents. */
int nand_read_ppn(unsigned ce, uint32_t row, unsigned sector,
                   void *buf, uint32_t meta[4]);
/* v17 full page from column zero: 16384 data + four 16-byte records.
 * Sector API above now selects its sector from this complete page. */
int nand_read_ppn_page(unsigned ce, uint32_t row, void *buf, uint32_t meta[16]);
/* N31 reserved-context discovery, read only. Fixed CE0, blocks 2086/2087,
 * pages 0..5; view 0=ordinary row, 1=reserved bit-20 view. Kept separate
 * from normal-row validation. count=1..4; unused output data/record slots
 * become 0xffffffff on success. Failure preserves both complete caller buffers.
 * Not a program/erase interface. */
int nand_read_ppn_context(unsigned block, unsigned page, unsigned view, unsigned count,
                          void *buf, uint32_t meta[16]);
/* Diagnostic inspection: 0=normal read, -2=complete transport with array error;
 * both return all16KiB+64B. -1/-3 preserve output. An errored non-FF page is
 * NOT valid file data. Kept separate from normal transactional reads. */
int nand_inspect_ppn_page(unsigned ce, uint32_t row, void *buf, uint32_t meta[16]);
/* Raw read of 4 KB (no FTL): page = cau << 19 | block << 7 | page.
 * 0 ok, -1 timeout, -2 status fail bit 0 (data still read). Round 15
 * (RetailOS 0x084ed600): the page's first 4 KB whatever sector says;
 * status bit 4 (GEB / uECC, erased pages too) is left in nand.st. meta
 * gets the 16 bytes of metadata (sftl: +0 type 1 = data, +1 bit 1 = uECC,
 * +8 LBA). */
int nand_read_sector(unsigned ce, uint32_t page, unsigned sector,
                     void *buf, uint32_t meta[4]);
/* Read-only experiment (nandscan2): byte-for-byte nand_read_sector's command
 * sequence (0x0a, 4 address bytes, 0x37, 0x77/0x7d, status poll, 0x7a,
 * 16 B meta + first 4 KB via the same data_in calls) but raw_addr is passed
 * to addr_lo unchanged and is not range-checked, so a caller can try
 * alternative address bit fields. Never clears buf/meta before the
 * transfer: on a read that delivers nothing (arming failure) they are left
 * untouched, on a PIO stall they may be partially written, and a transfer
 * of zeros writes zeros. That distinction is the experiment. */
int nand_read_raw(unsigned ce, uint32_t raw_addr, void *buf, uint32_t meta[4]);
/* Read entry point with a recovery pre-action (nandscan3). Modes:
 *   0 none (byte-for-byte read_page_raw, the baseline)
 *   1 ctl_init() immediately before (FMC controller reset)
 *   2 desel(); delay_us(1000)
 *   3 FMC(0x48) = 0xffffffff (W1C); FMC(0x81c) = 1
 *   4 nand_probe() before the read (heavy, diagnostic; it also does ctl_init)
 *   5 set_power(0): PPN SET FEATURES power state. A FEATURE write only, never
 *     a program/erase: the flash array is not accessed. The probe already
 *     ran it once this boot (nand.pwr), so this repeats a known-safe step.
 *   6 desel(); delay_us(1000); ctl_init()
 * -> mode's pre-action, then exactly read_page_raw()'s command sequence, so
 * only the pre-read action differs between modes. raw_addr is passed to the
 * 4-byte address field unchanged (unvalidated, like nand_read_raw).
 * Returns read_page_raw's 0/-1/-2, -3 for ce >= 2 or a null buffer, and
 * NAND_RM_BAD (-4) for an unknown mode; NAND_RM_UNAVAIL (-5) when mode 5 was
 * compiled out. No write/erase is reachable from here; the caller is still
 * responsible for nand_passive, which this function also sets. */
int nand_read_mode(unsigned ce, uint32_t raw_addr, void *buf, uint32_t meta[4],
                   unsigned mode);
enum {
    NAND_RM_NONE = 0, NAND_RM_CTL_INIT = 1, NAND_RM_DESEL_DELAY = 2,
    NAND_RM_W1C = 3, NAND_RM_PROBE = 4, NAND_RM_POWER = 5, NAND_RM_DESEL_CTL = 6,
    NAND_RM_MODES = 7,
    NAND_RM_BAD = -4,       /* unknown mode: distinct negative code */
    NAND_RM_UNAVAIL = -5,   /* mode 5 built out (NAND_SET_POWER = 0) */
};
/* Data-phase variants for nand_read_phase (nandscan4). The command, address,
 * status and 0x7a phases are identical; ONLY the data-phase arming differs:
 *   0 NAND_RP_TODAY          today's data_in() (all earlier rounds)
 *   1 NAND_RP_RETAILOS       FMC(0x48)=0x08000000, FMC(0x18)=1, then today's
 *   2 NAND_RP_RETAILOS_NO48  FMC(0x18)=1 only (variant 1 minus +0x48)
 *   3 NAND_RP_TODAY_18       today's phase + FMC(0x18)=1 only
 * Variants 2 and 3 are the same writes by construction; both are kept so the
 * report can show run-to-run consistency. */
enum {
    NAND_RP_TODAY = 0, NAND_RP_RETAILOS = 1, NAND_RP_RETAILOS_NO48 = 2,
    NAND_RP_TODAY_18 = 3, NAND_RP_VARIANTS = 4,
    NAND_RP_BADARGS = -3,       /* ce >= 2 or null buffer/meta */
    NAND_RP_BADVARIANT = -6,    /* unknown variant: distinct negative code */
};
/* Read-only data-phase experiment (nandscan4). nand_read_raw's exact page
 * sequence with the chosen data-phase variant applied to the 16 B metadata
 * read and all four 1 KiB data reads. raw_addr is passed to the 4-byte address
 * field unchanged (unvalidated, like nand_read_raw). Returns read_page_raw's
 * 0/-1/-2, NAND_RP_BADARGS, or NAND_RP_BADVARIANT. Passive mode is forced; no
 * write/erase is reachable. */
int nand_read_phase(unsigned ce, uint32_t raw_addr, void *buf, uint32_t meta[4],
                    unsigned variant);
/* Transfer-SHAPE matrix for nand_read_shape (nandscan5). The command,
 * address, status and 0x7a phases are identical for every shape; ONLY how the
 * 4 KB payload is armed, transferred and ended differs:
 *   0 TODAY      exactly today's read: meta then 4 x 1024-byte data_in()
 *   1 ONE_4096   meta, then ONE 4096-byte data_in()
 *   2 REARM_7A   meta, then 0x7a re-issued before each of four 1024-byte reads
 *   3 ECC_34     meta, then four 1024-byte blocks armed ECC-style
 *                (+0x30=0x34, +0x14=1, +0x24=0x400, +0x04=0x22, wait bit 8)
 *                and drained by the same PIO loop
 *   4 NO_81C     today's read without the FMC+0x81c = 1 FIFO/ECC reset
 *   5 META_ONLY  after 0x7a read ONLY the 16-byte metadata (no payload)
 *   6 ONE_1024   meta, then a SINGLE 1024-byte block
 *   7 2X2048     meta, then two 2048-byte data_in() calls
 * Shape 0 is today's behaviour byte-for-byte and is the control; 1/6/7 keep
 * today's arming and change only the chaining, 3 changes the arming, 4 the
 * pre-read reset, 5 isolates metadata from payload. */
enum {
    NAND_SHAPE_TODAY = 0, NAND_SHAPE_ONE_4096 = 1, NAND_SHAPE_REARM_7A = 2,
    NAND_SHAPE_ECC_34 = 3, NAND_SHAPE_NO_81C = 4, NAND_SHAPE_META_ONLY = 5,
    NAND_SHAPE_ONE_1024 = 6, NAND_SHAPE_2X2048 = 7,
    NAND_SHAPE_COUNT = 8,
    NAND_SHAPE_BADARGS = -3,    /* ce >= 2 or null buf/meta */
    NAND_SHAPE_BAD = -7,        /* unknown shape: distinct negative code */
};
/* Read-only transfer-shape experiment (nandscan5). nand_read_raw's exact page
 * sequence with the payload phase chosen by shape (NAND_SHAPE_*). raw_addr is
 * passed to the 4-byte address field unchanged (unvalidated, like
 * nand_read_raw); the caller must have probed. Passive mode is forced, no
 * write/erase is reachable and every wait is bounded. Returns 0 / -1 / -2 as
 * nand_read, NAND_SHAPE_BADARGS for bad arguments, NAND_SHAPE_BAD for an
 * unknown shape (which touches no hardware). */
int nand_read_shape(unsigned ce, uint32_t raw_addr, void *buf, uint32_t meta[4],
                    unsigned shape);
/* Read-ORDERING variants for nand_read_order (nandscan6). Every variant runs
 * the SAME command/address/ready sequence as today's page read and differs
 * ONLY in the order/extent of the data reads after 0x7a:
 *   0 TODAY             16 B metadata, then 4 x 1024 B payload (today)
 *   1 PAYLOAD_ONLY      NO metadata read at all; 4 x 1024 B payload directly
 *                       after 0x7a. The caller's meta[] is never written.
 *   2 PAYLOAD_ONLY_4096 NO metadata; ONE 4096-byte payload data_in()
 *   3 META_AFTER        4 x 1024 B payload FIRST, then the 16 B metadata
 *   4 META_ONLY         16 B metadata only, no payload (control: the small
 *                       post-0x7a read on its own)
 *   5 PAYLOAD_1K_ONLY   NO metadata; a single 1024-byte payload block
 *   6 SPLIT             payload first, then 0x77 + 0x7d (ready wait) and only
 *                       then the 16 B metadata
 *   7 NO_81C_PAYLOAD_ONLY as variant 1 but WITHOUT writing FMC+0x81c = 1
 *                       before the read (the pre-read FIFO/ECC reset)
 * Everything (arming, drain, end-of-phase wait) is data_in(), so only the
 * order/extent differs. Variants 1/2/5/7 deliberately leave meta[] untouched;
 * a caller that does not want a metadata read may pass meta == NULL for those.
 * An unknown order returns NAND_ORD_BAD and touches NOTHING. */
enum {
    NAND_ORD_TODAY = 0, NAND_ORD_PAYLOAD_ONLY = 1,
    NAND_ORD_PAYLOAD_ONLY_4096 = 2, NAND_ORD_META_AFTER = 3,
    NAND_ORD_META_ONLY = 4, NAND_ORD_PAYLOAD_1K_ONLY = 5,
    NAND_ORD_SPLIT = 6, NAND_ORD_NO_81C_PAYLOAD_ONLY = 7,
    NAND_ORD_COUNT = 8,
    NAND_ORD_BADARGS = -3,  /* ce >= 2, null buf, or null meta where needed */
    NAND_ORD_BAD = -9,      /* unknown order: distinct negative code */
};
/* Read-only ORDERING experiment (nandscan6). nand_read_raw's exact page
 * sequence; only the data-read order/extent after 0x7a is chosen by order.
 * raw_addr goes to the 4-byte address field unchanged (nandscan2 property).
 * Passive mode is forced before anything runs; no write/erase is reachable and
 * every wait is bounded. Returns 0 / -1 / -2 as the normal page read,
 * NAND_ORD_BADARGS or NAND_ORD_BAD. */
int nand_read_order(unsigned ce, uint32_t raw_addr, void *buf, uint32_t meta[4],
                    unsigned order);

/* DMA-ish experiment (nandscan6, task 2): the hypothesised "RetailOS DMA
 * drain" entry point. Variants:
 *   0 META_DMA    metadata via today's path, then the payload "by DMA"
 *   1 DMA_ONLY    payload only "by DMA"
 * DISASSEMBLY RESULT (private snapshot, analysis only): the function the
 * hypothesis was based on, 0x084cbd14, is RetailOS's hardware BCH **ECC
 * decode** step on the FMC sub-block at 0x38a00800 (writes +0x81c=1,
 * +0x818=0x04000400, +0x80c=1, +0x810=0x3f1f73af, +0x820=0x80000000,
 * +0x808=0x02000000, +0x804 = flags | ((len & 0x7ff) << 16) | 0x1e1; polls
 * +0x810 bit 0 for up to 20 ms; reads +0x80c/+0x818; acks +0x810=1). It moves
 * NO bytes, and it writes neither +0x30/+0x14/+0x24 (arming) nor +0x34/+0x38
 * (destination address). Its two callees 0x0833526c/0x08335284 are ARM
 * veneers into IRAM 0x22000fb8/0x22000fbe: a timestamp getter (reads
 * 0x3c700084) and an "elapsed >= timeout" helper. They are not opaque and
 * contain no FMC access. The only call in the page reader 0x084ed600 is on
 * the ECC path (ecc argument nonzero); the non-ECC path arms +0x18=1 etc. and
 * then drains through 0x080d33fc per 1024 B - exactly today's PIO path.
 * So there is NO evidenced DMA drain to reproduce, and the +0x34 DMA in
 * RetailOS/BootROM is a different, raw-page-parity sequence. This entry point
 * therefore does NOT touch hardware: both variants return
 * NAND_RD_UNSUPPORTED. A "pretend" implementation would be a guess. */
enum {
    NAND_RD_META_DMA = 0, NAND_RD_DMA_ONLY = 1, NAND_RD_VARIANTS = 2,
    NAND_RD_BADARGS = -3,     /* ce >= 2, null buf, or meta when variant 0 */
    NAND_RD_UNSUPPORTED = -8, /* evidenced: no DMA drain exists in 0x084cbd14 */
    NAND_RD_BADVARIANT = -10, /* unknown variant: distinct negative code */
};
/* Returns NAND_RD_UNSUPPORTED for the two known variants WITHOUT touching any
 * hardware; NAND_RD_BADARGS / NAND_RD_BADVARIANT for bad input. Passive mode
 * is forced. No write/erase is reachable. */
int nand_read_dmaish(unsigned ce, uint32_t raw_addr, void *buf, uint32_t meta[4],
                     unsigned variant);

/* Round 21 / nandscan7: the FMC+0x18 BEAT-SELECT experiment. Every variant runs
 * nand_read_raw's exact command/address/ready/data machinery; only the value
 * written to FMC+0x18 immediately before each data phase differs (plus the
 * optional +0x14 PIO-drain selector below):
 *   0 NAND_BEAT_TODAY              never write +0x18 (today's read, control)
 *   1 NAND_BEAT_SPLIT              +0x18=1 before metadata(16 B), +0x18=2
 *                                  before each 1,024-byte payload read
 *   2 NAND_BEAT_DATA_ONLY          +0x18=2 before all reads incl. metadata;
 *                                  OR in NAND_BEAT_NOMETA to skip metadata
 *   3 NAND_BEAT_SPARE_BOTH         +0x18=1 before all reads (inverse control)
 *   4 NAND_BEAT_SPLIT_META_LAST    payload first (+0x18=2), metadata last
 *                                  (+0x18=1)
 *   5 NAND_BEAT_SPLIT_PLUS_77      variant 1 plus a trailing 0x77 after the
 *                                  payload, before deselect
 *   6 NAND_BEAT_DATA_ONLY_PLUS_77  variant 2 with no metadata, plus trailing
 *                                  0x77
 *   7 NAND_BEAT_SPLIT_14_200       variant 1 with the payload drained at
 *                                  FMC+0x14 = 0x200 instead of 0x100
 * Sub-flags OR-ed into `variant` (the low nibble is the base variant):
 *   NAND_BEAT_NOMETA   0x100  skip the metadata read (base 2 and 6 only)
 *   NAND_BEAT_14_200   0x200  payload PIO drain +0x14 = 0x200 (bit 9)
 *   NAND_BEAT_14_010   0x400  payload PIO drain +0x14 = 0x010 (bit 4)
 * The +0x14 value is UNKNOWN: RetailOS's drain helper computes
 * 1 << (arg + 8), and only 0x100 (arg 0) has ever been observed. 0x200 would
 * be arg 1 and 0x010 cannot be produced by that formula. The external document
 * (LLM-generated, untrusted) says nothing about +0x14; this is a probe.
 * The +0x18 = beat-select claim comes from that EXTERNAL, POSSIBLY WRONG
 * document; only its geometry agrees with our probe. See nand.c / the
 * nandscan7 README.
 * Returns 0 / -1 (timeout) / -2 (status fail bit 0) as nand_read,
 * NAND_BEAT_BADARGS (-3) for ce >= 2 or a needed NULL pointer, and
 * NAND_BEAT_BAD (-11) for an unknown variant or flag combination (which touches
 * NO hardware). Passive mode is forced; raw_addr is passed to the 4-byte
 * address field unchanged; no write/erase is reachable. */
enum {
    NAND_BEAT_TODAY = 0, NAND_BEAT_SPLIT = 1, NAND_BEAT_DATA_ONLY = 2,
    NAND_BEAT_SPARE_BOTH = 3, NAND_BEAT_SPLIT_META_LAST = 4,
    NAND_BEAT_SPLIT_PLUS_77 = 5, NAND_BEAT_DATA_ONLY_PLUS_77 = 6,
    NAND_BEAT_SPLIT_14_200 = 7, NAND_BEAT_COUNT = 8,
    NAND_BEAT_VARIANT_MASK = 0x0fu,   /* low nibble = base variant */
    NAND_BEAT_NOMETA = 0x100u,        /* sub-flag: skip metadata (base 2/6) */
    NAND_BEAT_14_MASK = 0x600u,       /* sub-flag bits: payload +0x14 selector */
    NAND_BEAT_14_SHIFT = 9u,
    NAND_BEAT_BADARGS = -3,           /* ce >= 2 or a needed NULL pointer */
    NAND_BEAT_BAD = -11,              /* unknown variant/flags: touches nothing */
};
int nand_read_beat(unsigned ce, uint32_t raw_addr, void *buf, uint32_t meta[4],
                   unsigned variant);

/* Round 22 / nandscan8, Part A: ONE long bulk drain of the page stream.
 * Today's EXACT command/address/ready sequence (0x0a, four address bytes,
 * 0x37, 0x77, 0x7d, status poll, 0x7a), then exactly ONE data phase of
 * `words` 32-bit words: one arming (+0x30 = words*4 - 1, +0x14 = 1,
 * +0x24 = 0, +0x04 = 0x22, wait bit 8) and ONE pio_body() drain of the whole
 * length - no per-1 KiB re-arming. This is the only way to see the page's
 * linear stream instead of the 4 KiB slice five data_in() phases happen to
 * return. `flags`:
 *   0                        no metadata phase: the drain starts at stream
 *                            byte 0 (the normal read's metadata bytes are then
 *                            the FIRST bytes of the returned stream)
 *   NAND_BULK_META_FIRST     today's 16-byte metadata data_in() first (meta4
 *                            must be non-null), then the bulk: the comparison
 *                            shape (does a 16-byte phase shift the stream?)
 * `words` must be 1..NAND_BULK_MAX_WORDS (8,192 = 32 KiB). The buffer is
 * never cleared, so a word that is never popped keeps the caller's prefill -
 * the fixture uses that to count words NOT delivered.
 * Returns 0 / -1 (timeout or PIO stall) / -2 (status fail bit 0; data still
 * transferred), NAND_BULK_BADARGS (-3) for ce >= 2, a null buf, or a null
 * meta4 with META_FIRST, NAND_BULK_BADFLAGS (-12) for an unknown flag bit,
 * and NAND_BULK_BADWORDS (-13) for words = 0 or > the cap (all rejected
 * BEFORE touching hardware). Passive mode is forced, raw_addr goes to the
 * 4-byte address field unchanged, and no write/erase is reachable. */
#define NAND_BULK_MAX_WORDS 8192u
enum {
    NAND_BULK_NO_META = 0u,        /* no metadata phase (the fixture's Part A) */
    NAND_BULK_META_FIRST = 1u,     /* 16 B metadata data_in() first */
    NAND_BULK_BADARGS = -3,        /* ce >= 2, null buf, null meta4 with META */
    NAND_BULK_BADFLAGS = -12,      /* unknown flag bit: touches nothing */
    NAND_BULK_BADWORDS = -13,      /* words = 0 or > NAND_BULK_MAX_WORDS */
};
int nand_read_bulk(unsigned ce, uint32_t raw_addr, void *buf, uint32_t words,
                   uint32_t *meta4, unsigned flags);

/* Round 22 / nandscan8, Part B: RetailOS's ECC-branch ARMING combined with a
 * data-beat select, on the same page read path as nand_read_beat (the arming
 * is applied to the 16 B metadata phase and all four 1 KiB payload phases).
 * RetailOS's ECC branch sets FMC+0x30 = 0x34, FMC+0x14 = 0x10, FMC+0x24 = 0
 * and runs the BCH decode at 0x084cbd14 - which nandscan6 established is the
 * hardware ECC DECODE step, not a data mover, so it is deliberately NOT run
 * here. This tests the arming plus the beat select, never a decoder:
 *   E0 today's arming (control)
 *   E1 +0x30=0x34 +0x14=0x10 +0x18=2
 *   E2 +0x30=0x34 +0x14=0x10 +0x18=1
 *   E3 +0x30=0x34 +0x14=0x10 +0x18 untouched
 *   E4 +0x30=0x34 +0x14=0x100 +0x18=2
 *   E5 +0x30=0x1023 +0x14=0x10 +0x18=2
 *   E6 as E1, drain WITHOUT the +0x30=3 re-arm (one armed phase, then the
 *      PIO loop; the +0x00 mask and +0x14 are still applied)
 *   E7 as E1, deselect by FMC+0x00 = 0 instead of 1 (RetailOS writes 0)
 * The drain's +0x14 is the variant's armed value for E1..E6 and today's 0x100
 * for E0 (always writing 0x100 would overwrite the value the variant tests;
 * see the README). Unknown variant -> NAND_ECC_BADVARIANT (-14) before any
 * hardware access; ce >= 2 or null buf/meta -> NAND_ECC_BADARGS (-3). Passive
 * mode forced; read-only; no write/erase reachable. */
enum {
    NAND_ECC_E0 = 0, NAND_ECC_E1 = 1, NAND_ECC_E2 = 2, NAND_ECC_E3 = 3,
    NAND_ECC_E4 = 4, NAND_ECC_E5 = 5, NAND_ECC_E6 = 6, NAND_ECC_E7 = 7,
    NAND_ECC_COUNT = 8,
    NAND_ECC_BADARGS = -3,       /* ce >= 2 or a null buffer/meta */
    NAND_ECC_BADVARIANT = -14,   /* unknown variant: touches nothing */
};
int nand_read_ecc(unsigned ce, uint32_t raw_addr, void *buf, uint32_t meta[4],
                  unsigned variant);

/* Round 23 / nandscan9: the SINGLE-ARMED-PHASE page read.
 * run32 Part A (hardware) proved that ONE armed 17,408-byte transfer of CE0
 * page 0 delivers rc 0, sent 0, and a stream that begins with the 16 metadata
 * bytes 69f5134b 816d19e4 c2034e36 daeedecd followed by the data
 * 3adc3861 e02d0015 1c2dc5d3 29544baf, i.e. the page layout is linear
 * metadata-then-data and a many-KB one-armed-phase drain works. The normal page
 * read instead arms FIVE phases (16 B metadata + 4 x 1 KiB) and every later read
 * of a boot returns 0xCF filler then zeros (run25..31). The leading hypothesis
 * is that REPEATED ARMING is what breaks the datapath, so this entry point
 * performs today's EXACT command/address/ready sequence (0x0a, 4 address bytes,
 * 0x37, 0x77, 0x7d, poll, 0x7a) and then exactly ONE arming (+0x30 =
 * words*4 - 1, +0x14 = 1, +0x24 = 0, +0x04 = 0x22, wait bit 8) and ONE
 * pio_body() drain of `words` 32-bit words - exactly what nand_read_bulk does.
 * The PIO body / drain / bounded stall counter are shared; only the framing
 * differs by variant:
 *   0 NAND_SINGLE_PLAIN      drain then deselect (no trailing command)
 *   1 NAND_SINGLE_77        drain, then cmd(0x77), then deselect
 *   2 NAND_SINGLE_77_7D     drain, then 0x77 and 0x7d, then deselect
 *   3 NAND_SINGLE_NO_DESEL  drain and STOP, leaving the chip selected for the
 *                           caller to continue
 *   4 NAND_SINGLE_FULL_PAGE ignore `words`; read the FULL page,
 *                           nand.page_size / 4 words (16,448 B = 4,112 words
 *                           on the hardware part, 16,384 B = 4,096 in NanoEmu),
 *                           and report the words actually delivered through
 *                           nand_single_got.
 * nand_single_got is the number of words the LAST drain delivered (words whose
 * +0x28 PIO counter advanced); 0 when the data phase never armed. Buffers are
 * never cleared, so undelivered words keep the caller's prefill.
 * Returns 0 / -1 (timeout or bounded PIO stall) / -2 (status fail bit 0; the
 * data is still transferred), NAND_SINGLE_BADARGS (-3) for ce >= 2 or a null
 * out, NAND_SINGLE_BADVARIANT (-15) for an unknown variant and
 * NAND_SINGLE_BADWORDS (-16) for words = 0, words > NAND_SINGLE_MAX_WORDS, or
 * an unusable nand.page_size for variant 4 - all three rejected BEFORE touching
 * hardware. Passive mode is forced (no CLKCON/PSC writes), raw_addr goes to the
 * 4-byte address field unchanged, every wait is bounded, and no write/erase/
 * program call is reachable. */
#define NAND_SINGLE_MAX_WORDS 8192u
extern volatile uint32_t nand_single_got;
enum {
    NAND_SINGLE_PLAIN = 0, NAND_SINGLE_77 = 1, NAND_SINGLE_77_7D = 2,
    NAND_SINGLE_NO_DESEL = 3, NAND_SINGLE_FULL_PAGE = 4,
    NAND_SINGLE_COUNT = 5,
    NAND_SINGLE_BADARGS = -3,       /* ce >= 2 or a null out */
    NAND_SINGLE_BADVARIANT = -15,   /* unknown variant: touches nothing */
    NAND_SINGLE_BADWORDS = -16,     /* words out of range / no page size */
};
int nand_read_single(unsigned ce, uint32_t raw_addr, void *out, uint32_t words,
                     unsigned variant);

/* Round 25 / nandscan10: the SAME one-arming + one-drain page read as
 * nand_read_single/nand_read_bulk, exposing the drain GATE and the transfer
 * LENGTH, plus a separate spare-record capture path.
 *
 * The hardware-annotated N31 document's "PIO read and the gate-0 drain [H]"
 * section documents the stock drain as sampling FIFOCTR BEFORE the pop and
 * retaining word n only when the sampled low byte equals **4*n**, and states
 * that the older 4*n+4 test (with the counter read after the pop), which this
 * driver used, "popped and discarded word zero of every transfer" and produced
 * the fictional 1,020-byte beat / 15,300-byte page / blank-page signatures.
 * The low byte is a byte count, so it repeats every 64 words (64 x 4 = 256
 * bytes), not every 256 words; the gate therefore aliases at 64-word
 * boundaries, and a pop count wrong by a multiple of 64 words can still satisfy
 * it. pio_drain_mode's FIXED gate is the candidate fix; variants here let a
 * boot stage the corrected gate, the historical gate, the length probes and an
 * unconditional pop as separate first-read experiments.
 *
 * Variants (identical command/address/ready sequence and one arming; only the
 * drain differs):
 *   0 NAND_WORDS_BULK          FIXED gate: sample FIFOCTR before the pop, low
 *                              byte == 4*i, discard a mismatched candidate,
 *                              4,096-stall guard (the default headline).
 *   1 NAND_WORDS_NOWRAP_CHECK  pop `words` words unconditionally; no counter
 *                              comparison, hard loop bound only. popped =
 *                              words and counter-confirmed = 0 by construction.
 *   2 NAND_WORDS_32BIT_COUNTER FIXED gate compared as the full 32-bit value
 *                              FIFOCTR == 4*i, so it cannot alias at the
 *                              64-word low-byte wrap.
 *   3 (WITHDRAWN)              was NAND_WORDS_STALL_RESYNC. A "dummy read" is
 *                              impossible - a pop always consumes the FIFO - and
 *                              the old gate already discarded a mismatched word
 *                              by overwriting buf[i] on the retry, so a
 *                              scratch/commit-on-match variant would be
 *                              byte-for-byte identical in output and test
 *                              nothing. The slot is rejected with
 *                              NAND_WORDS_WITHDRAWN (-19); its useful roles are
 *                              covered by variant 2 (full-width gate) and
 *                              variant 1 (unconditional pop).
 *   4 NAND_WORDS_HALF          FIXED gate, transfer fixed at 2,176 words (half
 *                              of run32's 4,352); the `words` argument is
 *                              IGNORED.
 *   5 NAND_WORDS_DOUBLE        FIXED gate, transfer fixed at 8,704 words
 *                              (double); `words` IGNORED.
 *   6 NAND_WORDS_OLD_GATE      HISTORICAL gate (pop, then low byte == 4*i+4),
 *                              the explicit control for the documented bug.
 *   7 NAND_WORDS_BULK_77       exactly variant 0, then cmd(0x77) before
 *                              deselect: identical transfer, only the terminator
 *                              changes.
 * Every variant forces passive mode before touching hardware, performs no
 * write/erase/program, and bounds every wait and loop. `raw_addr` goes to the
 * 4-byte address field unchanged. The buffer is never cleared; an undelivered
 * word keeps the caller's prefill.
 * nand_words_got = retained (counter-confirmed) words; nand_words_popped = FIFO
 * pops including discarded candidates; nand_words_confirmed = words whose
 * FIFOCTR matched. For variant 1 popped = words, confirmed = 0, got = words.
 * All three are 0 when the data phase never armed.
 * Returns 0 / -1 (timeout or PIO stall) / -2 (status fail bit 0; data still
 * transferred) as the normal page read; NAND_WORDS_BADARGS (-3) for ce >= 2 or
 * a null buf, NAND_WORDS_BADVARIANT (-17) for an out-of-range variant,
 * NAND_WORDS_WITHDRAWN (-19) for the withdrawn slot 3, and NAND_WORDS_BADWORDS
 * (-18) for words = 0 or > NAND_WORDS_MAX_WORDS - all rejected BEFORE any
 * hardware access. */
#define NAND_WORDS_BASE 4352u        /* run32's 4,352-word (17,408 B) length */
#define NAND_WORDS_MAX_WORDS 8704u   /* 8,704 words = 34,816 B (variant 5) */
#define NAND_BOOT_WORDS 768u        /* stock boot reader: 3 x 1024 bytes */
extern volatile uint32_t nand_words_got;
extern volatile uint32_t nand_words_popped;
extern volatile uint32_t nand_words_confirmed;
enum {
    NAND_WORDS_BULK = 0, NAND_WORDS_NOWRAP_CHECK = 1,
    NAND_WORDS_32BIT_COUNTER = 2, NAND_WORDS_STALL_RESYNC = 3,
    NAND_WORDS_HALF = 4, NAND_WORDS_DOUBLE = 5,
    NAND_WORDS_OLD_GATE = 6, NAND_WORDS_BULK_77 = 7,
    /* v15: saved RetailOS boot reader's non-ECC framing, 3 x 1024 B,
     * no metadata phase, original post-pop counter gate, trailing 0x77.
     * Requires exactly NAND_BOOT_WORDS and a word-aligned buffer. */
    NAND_WORDS_BOOT_STOCK = 8, NAND_WORDS_COUNT = 9,
    NAND_WORDS_BADARGS = -3,         /* ce >= 2 or a null buf */
    NAND_WORDS_BADVARIANT = -17,     /* variant >= COUNT: touches nothing */
    NAND_WORDS_BADWORDS = -18,       /* words = 0 or > NAND_WORDS_MAX_WORDS */
    NAND_WORDS_WITHDRAWN = -19,      /* slot 3: withdrawn, touches nothing */
};
int nand_read_words(unsigned ce, uint32_t raw_addr, void *buf, uint32_t words,
                    unsigned variant);

/* Round 25 / nandscan10: SPARE-RECORD capture via REC_LATCH. Per the
 * hardware-annotated N31 document's "Spare-record capture [H]" section, the
 * 16-byte sector record is NOT in the PIO data stream (draining the nominal
 * spare beat returns stale FIFO bytes). The working sequence is: page read
 * (0x0a, four address bytes, 0x37, 0x77, 0x7d, poll, 0x7a), then
 *  1. stock spare-beat setup: FMLEN(+0x30) = 0xF, FMUNK58(+0x58) = 0x20,
 *     FMCTRL1(+0x04) = 0x32;
 *  2. FMUNK58 = 1 and start the first data beat (this driver uses a 4 KiB beat,
 *     FMLEN = 0xFFF; the document does NOT specify the beat length, so that
 *     value is UNKNOWN, not evidence);
 *  3. REC_LATCH(+0x40) = 0x1102, then poll bit 1 clear (bounded);
 *  4. read SPR0..3 (+0x60..+0x6C).
 * The latch is repeated four times because the document describes one 16-byte
 * record per latch and four records per page; the fixture reports all four
 * captures and their parsed fields (type/subtype/key/gen/value/gen2) and makes
 * no claim that the repetition matches stock exactly. No page payload is
 * drained and no flash write/erase/program is reachable; passive mode forced;
 * every wait bounded. rec[k][0..3] = SPR0..3 of capture k in register order.
 * Returns 0 / -1 / -2 as the page read, NAND_REC_BADARGS (-3) for ce >= 2 or a
 * null rec. */
enum { NAND_REC_BADARGS = -3 };
int nand_read_record(unsigned ce, uint32_t raw_addr, uint32_t rec[4][4]);

/* Read-only snapshot for diagnosis (nandscan3): out[0..14] = FMC +0x00 +0x04
 * +0x10 +0x14 +0x18 +0x24 +0x28 +0x30 +0x38 +0x48 +0x4c +0x80 +0x81c +0xc00
 * +0xc08, out[15] = nand.st. Reads only, in that order; reading +0x80 pops
 * the PIO FIFO (the transfer has already ended when this is called). */
void nand_regs_snapshot(uint32_t out[16]);
/* Experimental extended read (nand.c): nand_read_sector's sequence, then
 * extra_words (<= NAND_EXT_MAX_WORDS) more words from the same transfer into
 * extra[]. Read only. nand_ext_got = words delivered. */
#define NAND_EXT_MAX_WORDS 4096u
extern volatile uint32_t nand_ext_got;
int nand_read_ext(unsigned ce, uint32_t page, void *buf, uint32_t meta[4],
                  uint32_t *extra, uint32_t extra_words);
/* Probe, read test pages into buf (4 KB) and log both to RAM report R81
 * (report.h); returns nand_probe()'s result. *rd/meta: the first read. */
int nand_selftest(uint32_t *buf, int *rd, uint32_t meta[4]);
/* Raw writes. Default build: NANOOS_NAND_WRITE is 0, nand_write_sector is
 * the old hard stub (-1) and no erase/program code exists in kernel.bin.
 * Compiling with -DNANOOS_NAND_WRITE=1 (build_payload.py: env
 * NANOOS_NAND_WRITE=1) adds the guarded path below, which is still
 * non-destructive as shipped: the block allowlist is empty and the erase/
 * program steps return NAND_W_NOERASE / NAND_W_NOPROG (see nand.c for why:
 * no hardware-proven erase command, no FTL-free block identified). */
#ifndef NANOOS_NAND_WRITE
#define NANOOS_NAND_WRITE 0
#endif
enum {
    NAND_W_OFF = -1,       /* built without NANOOS_NAND_WRITE */
    NAND_W_DENY = -3,      /* ce/block outside the allowlist or in the LLB denylist */
    NAND_W_NOTBLANK = -4,  /* target page not provably erased (read error or non-0xff) */
    NAND_W_NOERASE = -5,   /* erase refused: no PPN erase sequence known for this part */
    NAND_W_NOPROG = -6,    /* program step not ported (DMA path unproven on hardware) */
    NAND_W_VERIFY = -7,    /* readback after program differs */
};
/* Absolute physical blocks that may be erased/programmed: inclusive block
 * range of one CAU on one CE. first > last = disabled entry. */
struct nand_range { uint8_t ce, cau; uint16_t first, last; };
/* 0 if the page's block is allowlisted and not LLB, else NAND_W_DENY */
int nand_write_allowed(unsigned ce, uint32_t page);
/* Erase the block holding page (page address as nand_read_sector). Always
 * NAND_W_NOERASE for now. */
int nand_erase_block(unsigned ce, uint32_t page);
/* Guards -> blank check -> program -> readback verify. Returns 0 or a
 * NAND_W_* code; -1/-2 from the reads it does. Never reaches the program
 * step as shipped. */
int nand_write_sector(unsigned ce, uint32_t page, unsigned sector,
                      const void *buf, const uint32_t meta[4]);
#endif
