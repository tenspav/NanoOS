#ifndef NANOOS_GAMEABI_H
#define NANOOS_GAMEABI_H
#include <stdint.h>
/* Large-game ABI v1: a game is a separate DRAM image (game.bin, linked at
 * its load address inside [MM_GAME, MM_GAME_END), memmap.h) that NanoOS
 * calls from the launcher and that returns to it. Plain C, shared by the
 * kernel (game.c), NanoOSLoader and the game (game/). docs/memory-map.md
 * has the boot flow. */
#define NGAME_MAGIC 0x314d474eu  /* "NGM1" */
/* NGAME_ABI: the image format and load protocol (header, chunk list,
 * entry convention), checked by NanoOSLoader and game.c; unchanged. */
#define NGAME_ABI 1u
/* The services table version is in api->flags bits 16..23 (0 on a v1
 * kernel); api->abi stays NGAME_ABI, which v1 games test for equality.
 * Version 2 (2026-10-19, Vice City stage 2) appended the v2 block (clock,
 * D-cache clean, input levels, MBX driver); the v1 prefix is unchanged. A
 * game checks NGAME_API_TABLE(api) before it uses the v2 block, so one
 * image runs on every loader and on an older kernel (and says so).
 * Version 3 appends MBX halt/target setup for exclusive GLES game backends;
 * a v3 game checks the version before reading those appended fields.
 * Version 4 (read-only NAND scan) appends nand_probe/nand_read/nand_info and
 * the experimental nand_read_ext; NO write/erase is exposed. The game path
 * runs the NAND driver in passive mode (no CLKCON/PSC writes; only the NAND
 * pad function-select GPIO writes and one PPN SET FEATURES power command
 * remain). A v4 game checks NGAME_API_TABLE(api)>=4 first; v3 games unchanged.
 * Version 5 (nandscan2 address experiment) appends nand_read_raw, which is
 * nand_read's sequence with the 4-byte address passed through unchecked; the
 * v4 block and all earlier fields keep their order. Read-only, passive mode
 * forced. A v5 game checks NGAME_API_TABLE(api)>=5.
 * Version 6 (nandscan3 read-repeatability experiment) appends nand_read_mode,
 * nand_read's command sequence with one recovery pre-action chosen by mode
 * (0 none, 1 ctl_init, 2 desel+1ms, 3 W1C +0x48/+0x81c, 4 nand_probe, 5 PPN
 * SET FEATURES power state - a feature write, not program/erase, and already
 * run once by the probe, 6 desel+1ms+ctl_init; modes >=7 rejected with -4),
 * and nand_regs, a read-only FMC register snapshot. The v4/v5 fields and
 * their order are untouched; read-only, passive mode forced. A v6 game checks
 * NGAME_API_TABLE(api)>=6.
 * Version 7 (nandscan4 data-phase experiment) appends nand_read_phase,
 * nand_read_raw's exact page sequence with ONLY the data-phase arming chosen
 * by variant: 0 today's phase, 1 RetailOS's (+0x48=0x08000000 then +0x18=1),
 * 2 variant 1 without the +0x48 write, 3 today's phase with only +0x18=1
 * (unknown -> -6, bad args -> -3). The variant is applied to the metadata
 * read and all four 1 KiB data reads. The v4/v5/v6 fields keep their order;
 * read-only, passive mode forced, no write/erase is exposed. A v7 game checks
 * NGAME_API_TABLE(api)>=7.
 * Version 8 (nandscan5 transfer-shape experiment) appends nand_read_shape,
 * the same page sequence with the data PAYLOAD phase chosen by shape: 0
 * today's read (4 x 1 KiB), 1 one 4096-byte data_in, 2 0x7a re-issued per
 * 1 KiB block, 3 ECC-style 0x34 arming per 1 KiB block, 4 without the
 * +0x81c reset, 5 metadata only (no payload), 6 one 1 KiB block, 7 two
 * 2048-byte data_in calls (unknown -> -7, bad args -> -3). The v4..v7 fields
 * keep their order; read-only, passive mode forced, no write/erase is
 * exposed. A v8 game checks NGAME_API_TABLE(api)>=8.
 * Version 9 (nandscan6 ordering experiment) appends nand_read_order and
 * nand_read_dmaish. nand_read_order is the same page sequence with ONLY the
 * order/extent of the post-0x7a data reads chosen: 0 meta then 4x1 KiB
 * (today), 1 payload only (no metadata), 2 payload only as one 4096-byte
 * read, 3 payload then metadata, 4 metadata only, 5 one 1 KiB payload only,
 * 6 payload then 0x77+0x7d then metadata, 7 as 1 without FMC+0x81c=1
 * (unknown -> -9, bad args -> -3). nand_read_dmaish is the "RetailOS DMA
 * drain" experiment; the disassembly shows 0x084cbd14 is the hardware BCH ECC
 * decode routine, not a drain, so both variants return the distinct
 * unsupported code -8 without touching hardware (bad args -3, unknown
 * variant -10). Read-only, passive mode forced, no write/erase exposed. The
 * v4..v8 fields keep their order; a v9 game checks NGAME_API_TABLE(api)>=9.
 * Version 10 (nandscan7 beat-select experiment) appends nand_read_beat: the
 * same page sequence with ONLY the FMC+0x18 value written before each data
 * phase chosen, plus optional sub-flags. 0 today (never write +0x18), 1 split
 * (+0x18=1 metadata, 2 payload), 2 data only (+0x18=2 everywhere; sub-flag
 * 0x100 skips metadata), 3 spare both (+0x18=1), 4 split with metadata last,
 * 5 split plus trailing 0x77, 6 data-only no metadata plus 0x77, 7 split with
 * payload +0x14=0x200. Sub-flags 0x200/0x400 select payload +0x14 = 0x200 /
 * 0x010 instead of today's 0x100 (the correct value is UNKNOWN; see nand.h).
 * Unknown variant/flags -> -11, bad args -> -3. The +0x18 = beat-select claim
 * is from an EXTERNAL, LLM-GENERATED, POSSIBLY WRONG document; only its
 * geometry matches our own probe. Read-only, passive mode forced, no
 * write/erase exposed. The v4..v9 fields keep their order; a v10 game checks
 * NGAME_API_TABLE(api)>=10.
 * Version 11 (nandscan8 bulk-drain and ECC-arming experiments) appends
 * nand_read_bulk and nand_read_ecc. nand_read_bulk is today's exact
 * command/address/ready sequence plus ONE long PIO drain of `words` 32-bit
 * words with a single arming (no per-1 KiB re-arming); flags 0 = no metadata
 * phase (the stream starts at page byte 0), 1 = today's 16-byte metadata
 * read first. nand_read_ecc combines RetailOS's ECC-branch arming
 * (+0x30 = 0x34, +0x14 = 0x10, +0x24 = 0) with a +0x18 beat value: E0 today,
 * E1/E2/E3 = 0x34/0x10 with +0x18 2/1/untouched, E4 = +0x14 0x100, E5 =
 * +0x30 0x1023, E6 = E1 without the drain's +0x30 = 3 re-arm, E7 = E1 with
 * FMC+0x00 = 0 deselect. Bad words/flags -> -13/-12, unknown ECC variant ->
 * -14, bad ce/pointers -> -3. The BCH decode at 0x084cbd14 is NOT run (it is
 * an ECC decode step, not a data mover). Read-only, passive mode forced, no
 * write/erase exposed. The v4..v10 fields keep their order; a v11 game checks
 * NGAME_API_TABLE(api)>=11.
 * Version 12 (nandscan9 single-armed-phase read) appends nand_read_single and
 * nand_single_delivered. nand_read_single performs today's exact
 * command/address/ready sequence (0x0a, four address bytes, 0x37, 0x77, 0x7d,
 * status poll, 0x7a) and then exactly ONE arming (+0x30 = words*4-1, +0x14 = 1,
 * +0x24 = 0, +0x04 = 0x22, wait bit 8) and ONE PIO drain of `words` 32-bit
 * words, i.e. exactly what nand_read_bulk does, with a variant for the framing:
 * 0 plain (no trailing command), 1 cmd(0x77) after the drain, 2 0x77 then 0x7d,
 * 3 leave the chip selected (no deselect), 4 read the FULL page
 * (nand.page_size/4 words, ignoring `words`) and report the delivered count.
 * nand_single_delivered() returns the number of words the last drain actually
 * delivered (0 if the data phase never armed). Bad ce/pointer -> -3, unknown
 * variant -> -15, words out of range / unusable page size -> -16, all before
 * any hardware access; 0/-1/-2 as the normal page read. Passive mode forced,
 * raw_addr passed through unchanged, no write/erase exposed. The v4..v11 fields
 * keep their order; a v12 game checks NGAME_API_TABLE(api)>=12.
 * Version 13 (nandscan10 gate/length/record experiment) appends
 * nand_read_words, nand_words_delivered, nand_words_popped,
 * nand_words_confirmed and nand_read_record. nand_read_words is
 * nand_read_single's exact command/address/ready sequence plus ONE arming and
 * ONE PIO drain, but the drain GATE and the transfer LENGTH are chosen by
 * variant: 0 the FIXED gate (sample FIFOCTR before the pop, low byte == 4*i,
 * discard a mismatch, 4,096-stall guard), 1 pop ALL words with no counter
 * comparison (hard loop bound only), 2 the FIXED gate compared as the full
 * 32-bit counter (no 64-word alias), 3 WITHDRAWN (returns -19; a dummy read is
 * impossible, a pop always consumes the FIFO), 4 the FIXED gate at 2,176 words
 * (half of run32's 4,352), 5 the FIXED gate at 8,704 words (double), 6 the
 * HISTORICAL gate (pop, then low byte == 4*i+4) as the control, 7 exactly
 * variant 0 plus a trailing 0x77. Variants 4/5 ignore `words`.
 * nand_words_delivered() = retained/counter-confirmed words,
 * nand_words_popped() = FIFO pops (retries included) and
 * nand_words_confirmed() = words whose FIFOCTR matched, so a gate that
 * discards words shows popped > confirmed. The "+0x28 low byte == 4*n" gate
 * and the note that the old 4*n+4 test discarded word zero of every transfer
 * come from the hardware-annotated N31 document; run32's stream is used only
 * as a REFERENCE SIGNATURE, and length remains a hypothesis because run32 and
 * run33 were different boots and builds.
 * nand_read_record() captures the four 16-byte spare records via the
 * REC_LATCH path (spare-beat setup, FMUNK58=1, REC_LATCH=0x1102, poll bit 1
 * clear, read SPR0..3) because the records are NOT in the PIO stream. Bad
 * ce/pointer -> -3, variant out of range -> -17, words out of range -> -18,
 * withdrawn variant -> -19, all before any hardware access; 0/-1/-2 as the
 * normal page read. Passive mode forced, raw_addr passed through unchanged,
 * every wait/loop bounded, no write/erase exposed. The v4..v12 fields keep
 * their order; a v13 game checks NGAME_API_TABLE(api)>=13. */
/* v15 adds nand_read_words variant 8 (stock 3072-byte boot reader).
 * No table fields move; existing variants keep their numeric values. */
#define NGAME_API_VERSION 23u
#define NGAME_API_TABLE(api) (((api)->flags >> 16) & 0xffu)
/* The first 32 bytes of game.bin (game/mkgame.py fills size and crc). */
struct ngame_hdr {
    uint32_t magic, abi;
    uint32_t load;       /* link address of byte 0 */
    uint32_t size;       /* image bytes, a multiple of 4 */
    uint32_t bss_end;    /* [load + size, bss_end) is zeroed at each launch */
    uint32_t entry;      /* int entry(const struct nanoos_api *), ARM or Thumb */
    uint32_t crc;        /* CRC-32 of bytes [32, size) */
    uint32_t rsv;
};

/* Route (a), NanoOSLoader: game.bin read in file order into RetailOS heap
 * chunks (VA = PA), described by this block, which the chainloader passes
 * as n7_handoff.initrd_start/initrd_size (probe/chainloader/handoff.h).
 * Every chunk but the last is a multiple of 4 bytes. A kernel FAT32
 * reader (route b) will fill the image itself and skip this block. */
#define NBOOT_MAGIC 0x444c474eu  /* "NGLD" */
#define NBOOT_VERSION 1u
#define NBOOT_CHUNKS 64u
struct nboot_game {
    uint32_t magic, version;
    uint32_t size;       /* bytes in all chunks (= ngame_hdr.size) */
    uint32_t nchunks;
    struct { uint32_t addr, len; } chunk[NBOOT_CHUNKS];
};
/* the chainloader's boot block (handoff.h struct n7_handoff), as read back */
#define N7_HANDOFF_ADDR 0x22009000u
#define N7_HANDOFF_MAGIC 0x4c43374eu  /* "N7CL" */

/* Services the kernel hands to the game: the existing drivers, no copies.
 * v1..v5 share one table prefix; NGAME_API_TABLE(api) says which blocks are
 * there.
 * Drawing: lcd_back() is the 240x432 ARGB8888 panel buffer, logical (x, y)
 * of the 432x240 landscape screen at [x * 240 + 239 - y]; lcd_present()
 * queues it, lcd_service() pushes in slices (call every loop pass),
 * lcd_ready() says the next frame may be drawn. Input: input_poll() edges
 * (NGAME_HOME/LEFT/RIGHT), touch_poll() then *touch. */
#define NGAME_HOME 1u
#define NGAME_LEFT 2u
#define NGAME_RIGHT 4u
#define NGAME_API_MMU 1u   /* flags: MMU, D-cache on (memmap.h attributes) */
/* flags bits 16..23: the table version (see NGAME_API_TABLE) */
struct ngame_touch { int x, y; unsigned state; };   /* touch.h prefix */
/* v4: NAND geometry as probed (nand.h struct nand_info prefix, same order) */
/* ...plus the probe's recovery trace, so a failed probe is diagnosable.
 * rcv: 0 = controller latched at once (no recovery needed), 1 = after
 * ctl_init, 2/3/4 = after each later recovery step, 5 = never. Passive
 * mode (game path) performs ONLY ctl_init, so rcv>1 means the controller
 * needs a clock/PSC step that this path deliberately does not do. */
struct ngame_nand_info { uint32_t id, id2, caus, blocks, pages, page_size, errors, st;
                         uint32_t cs, cs0, st48, rcv, probes, attempt, init, ph, passive;
                         uint32_t ppn_select, ppn_start, ppn_end, ppn_bytes, ppn_stage, ppn_power1; };
struct nanoos_api {
    uint32_t abi, flags;
    uint32_t *(*lcd_back)(void);
    void (*lcd_present)(void);
    void (*lcd_service)(void);
    int (*lcd_ready)(void);
    void (*lcd_flush)(void);
    void (*lcd_clear)(uint32_t color);
    void (*lcd_fill_rows)(int y0, int y1, uint32_t color);
    void (*lcd_text)(int x, int y, const char *s, uint32_t color);
    void (*lcd_center)(int y, const char *s, uint32_t color);
    unsigned (*input_poll)(void);
    int (*touch_poll)(void);
    const struct ngame_touch *touch;
    void (*delay_us)(uint32_t us);
    /* v2. usec(): the free-running 1 MHz counter (timer.h USEC), wraps
     * every 71.6 min. dcache_clean(): every dirty D-cache line to DRAM
     * (no-op without the MMU), before a non-CPU master reads a cached
     * buffer. input_held(): button levels (NGAME_*); input_service():
     * sample buttons during long work (edges go to the next input_poll).
     * mbx_*: the kernel MBX driver (mbx.h), as ViceNano M8 calls it; every
     * GPU-visible buffer must lie in [MM_GAME_GPU, MM_GAME_GPU_END) or be
     * lcd_back(). */
    uint32_t (*usec)(void);
    void (*dcache_clean)(void);
    unsigned (*input_held)(void);
    void (*input_service)(void);
    int (*mbx_init)(void);
    void (*mbx_begin)(void);
    void (*mbx_submit)(const uint32_t *fifo, uint32_t words);
    void (*mbx_set_background)(uint32_t word);
    int (*mbx_render)(const uint32_t *target, uint32_t w, uint32_t h, uint32_t stride);
    int (*mbx_wait_done)(uint32_t timeout_us);
    /* v3: exclusive game backend cleanup/target setup. v1/v2 prefix unchanged.
     * halt resets the GPU; ONLY use while the NanoOS game owns it. */
    void (*mbx_halt)(void);
    void (*mbx_set_target)(const uint32_t *target, uint32_t w, uint32_t h, uint32_t stride);
    /* v4, read-only NAND. nand_probe(): 1 ok, 0 fail (sets passive mode).
     * nand_read(ce, page, buf, meta): page = cau<<19 | block<<7 | page; buf
     * 4096 bytes, meta 4 words; 0 ok, -1 timeout, -2 status fail, -3 bad
     * argument / not probed. nand_info: live geometry/error record.
     * nand_read_ext: experiment, as nand_read plus extra_words more words from
     * the same transfer into extra[] (same -3 checks; extra_words clamped). */
    int (*nand_probe)(void);
    int (*nand_read)(unsigned ce, uint32_t page, void *buf, uint32_t meta[4]);
    const struct ngame_nand_info *nand_info;
    int (*nand_read_ext)(unsigned ce, uint32_t page, void *buf, uint32_t meta[4],
                         uint32_t *extra, uint32_t extra_words);
    /* v5, read-only NAND address experiment. Exactly nand_read's sequence,
     * but raw_addr is passed to the 4-byte page address field unchanged (no
     * page/CAU/block decoding, no upper-bit check). Caller must have probed.
     * -3 on bad ce/pointers/not probed only; 0/-1/-2 as nand_read. */
    int (*nand_read_raw)(unsigned ce, uint32_t raw_addr, void *buf, uint32_t meta[4]);
    /* v6, read-only NAND read with a recovery pre-action (nandscan3). The
     * command sequence is exactly nand_read's; mode changes only what runs
     * immediately before it: 0 none (baseline), 1 ctl_init (FMC controller
     * reset), 2 desel + 1 ms, 3 W1C FMC(0x48) + FMC(0x81c)=1, 4 nand_probe
     * (heavy), 5 set_power(0) (PPN SET FEATURES: a FEATURE write, NOT a
     * program/erase; already run once by the probe), 6 desel+1ms+ctl_init.
     * Unknown mode -> -4; mode 5 compiled out -> -5; -3 on bad ce/pointers/
     * not probed; 0/-1/-2 as nand_read. Passive mode forced by the wrapper. */
    int (*nand_read_mode)(unsigned ce, uint32_t raw_addr, void *buf,
                          uint32_t meta[4], unsigned mode);
    /* v6: read-only snapshot, out[0..14] = FMC +0x00 +0x04 +0x10 +0x14
     * +0x18 +0x24 +0x28 +0x30 +0x38 +0x48 +0x4c +0x80 +0x81c +0xc00 +0xc08,
     * out[15] = the last polled NAND status byte (nand.st). The wrapper forces
     * passive mode and ignores a null pointer. It has no probe precondition:
     * it only reads registers, so it is usable to diagnose a failed probe. */
    void (*nand_regs)(uint32_t out[16]);
    /* v7, read-only NAND data-phase experiment (nandscan4). Exactly
     * nand_read_raw's command/address/status/0x7a sequence; only the data
     * phase differs, applied to the 16 B metadata read and all four 1 KiB
     * data reads:
     *   0 today's data_in(), 1 RetailOS's non-ECC phase (+0x48 = 0x08000000,
     *   then +0x18 = 1), 2 variant 1 without the +0x48 write, 3 today's phase
     *   with only +0x18 = 1. Unknown variant -> -6; -3 on bad ce/pointers/not
     *   probed; 0/-1/-2 as nand_read. Passive mode forced, raw_addr passed
     *   through unchanged (the v5 property). No write/erase is reachable. */
    int (*nand_read_phase)(unsigned ce, uint32_t raw_addr, void *buf,
                           uint32_t meta[4], unsigned variant);
    /* v8, read-only NAND transfer-shape experiment (nandscan5). Exactly
     * nand_read_phase's command/address/status/0x7a sequence; only how the
     * 4096-byte payload is armed, transferred and ended differs:
     *   0 today's read: meta then 4 x 1024-byte data_in()
     *   1 meta then ONE 4096-byte data_in()
     *   2 0x7a re-issued before each of the four 1024-byte reads
     *   3 ECC-style arming per 1024-byte block (+0x30=0x34, +0x14=1,
     *     +0x24=0x400, +0x04=0x22, wait bit 8), same PIO drain
     *   4 today's read without the FMC+0x81c = 1 FIFO/ECC reset
     *   5 metadata only, no payload
     *   6 ONE 1024-byte block only
     *   7 two 2048-byte data_in() calls
     * Unknown shape -> -7; -3 on bad ce/pointers; 0/-1/-2 as nand_read.
     * Passive mode forced, raw_addr passed through unchanged, no write/erase
     * reachable. A v8 game checks NGAME_API_TABLE(api) >= 8. */
    int (*nand_read_shape)(unsigned ce, uint32_t raw_addr, void *buf,
                           uint32_t meta[4], unsigned shape);
    /* v9, read-only NAND read-ORDERING experiment (nandscan6). Exactly
     * nand_read_shape's command/address/ready sequence; only the order and
     * extent of the data reads after 0x7a differ:
     *   0 metadata (16 B) then four 1 KiB payload blocks (today)
     *   1 NO metadata read at all; four 1 KiB payload blocks
     *   2 NO metadata; one 4096-byte payload read
     *   3 four 1 KiB payload blocks first, then the 16 B metadata
     *   4 metadata only, no payload
     *   5 NO metadata; a single 1024-byte payload block
     *   6 payload first, then 0x77+0x7d, then the 16 B metadata
     *   7 as 1 but without the pre-read FMC+0x81c = 1 FIFO/ECC reset
     * Orders 1/2/5/7 never write meta[] (a NULL meta is allowed for them).
     * Unknown order -> -9; -3 on bad ce/pointers; 0/-1/-2 as nand_read.
     * Passive mode forced, raw_addr passed through unchanged, no write/erase
     * reachable. A v9 game checks NGAME_API_TABLE(api) >= 9. */
    int (*nand_read_order)(unsigned ce, uint32_t raw_addr, void *buf,
                           uint32_t meta[4], unsigned order);
    /* v9, the "RetailOS DMA drain" experiment (nandscan6 task 2). Variant 0 =
     * metadata via today's path then payload "by DMA"; variant 1 = payload
     * only "by DMA". DISASSEMBLY RESULT: 0x084cbd14 (the routine the
     * hypothesis named) is the FMC's hardware BCH ECC DECODE step at
     * +0x800..+0x820 - it writes +0x81c/+0x818/+0x80c/+0x810/+0x820/+0x808/
     * +0x804, polls +0x810 bit 0 and reads +0x80c/+0x818, and moves no data;
     * its callees are a timestamp getter and an elapsed-time helper. There is
     * no evidenced DMA payload drain, so this entry point does NOT touch
     * hardware and returns the distinct unsupported code -8 for both known
     * variants. Unknown variant -> -10; -3 on bad ce/pointers. Read-only,
     * passive mode forced. */
    int (*nand_read_dmaish)(unsigned ce, uint32_t raw_addr, void *buf,
                            uint32_t meta[4], unsigned variant);
    /* v10, read-only NAND BEAT-SELECT experiment (nandscan7). Exactly
     * nand_read_order's command/address/ready sequence and the same
     * data_in/pio_drain machinery; only the FMC+0x18 value written immediately
     * before each data phase differs:
     *   0 never write +0x18 (today, control)
     *   1 +0x18=1 before the 16 B metadata read, 2 before each 1 KiB payload
     *   2 +0x18=2 before all reads including metadata; sub-flag 0x100 skips
     *     metadata entirely (four 1 KiB payload reads only)
     *   3 +0x18=1 before all reads (inverse control)
     *   4 payload first (+0x18=2), metadata last (+0x18=1)
     *   5 variant 1 plus a trailing 0x77 after the payload before deselect
     *   6 variant 2 with no metadata plus trailing 0x77
     *   7 variant 1 with the payload PIO drain at FMC+0x14=0x200
     * Sub-flags OR-ed into variant (low nibble = base): 0x100 = NOMETA,
     * 0x200 = payload +0x14 0x200, 0x400 = payload +0x14 0x010. The +0x14
     * value is UNKNOWN (RetailOS's helper computes 1 << (arg+8); only 0x100
     * has ever been observed and the external document says nothing about it).
     * meta may be NULL only when no metadata read happens (base 2 + 0x100, or
     * base 6). Unknown variant/flags -> -11 without touching hardware; -3 on
     * bad ce/pointers/not probed; 0/-1/-2 as nand_read. The +0x18 beat-select
     * claim is EXTERNAL, LLM-GENERATED and POSSIBLY WRONG. Read-only, passive
     * mode forced, no write/erase reachable. A v10 game checks
     * NGAME_API_TABLE(api) >= 10. */
    int (*nand_read_beat)(unsigned ce, uint32_t raw_addr, void *buf,
                          uint32_t meta[4], unsigned variant);
    /* v11, read-only NAND BULK-DRAIN experiment (nandscan8 Part A). Today's
     * exact command/address/ready sequence (0x0a, four address bytes, 0x37,
     * 0x77, 0x7d, status poll, 0x7a) and then ONE data phase of `words` 32-bit
     * words: one arming (+0x30 = words*4-1, +0x14 = 1, +0x24 = 0,
     * +0x04 = 0x22, wait bit 8) and ONE PIO drain of the whole length, with NO
     * per-1 KiB re-arming. flags 0 = no metadata phase (the returned stream
     * starts at page byte 0, so the normal read's 16 metadata bytes are its
     * first bytes); NAND_BULK_META_FIRST (1) = today's 16-byte metadata
     * data_in() first, then the bulk. `words` 1..8192. The buffer is never
     * cleared, so undelivered words keep the caller's prefill. 0/-1/-2 as
     * nand_read; -3 bad ce/pointers/null meta with META_FIRST; -12 unknown
     * flag; -13 words out of range (all rejected before touching hardware).
     * Passive mode forced, raw_addr passed through unchanged, read-only.
     * A v11 game checks NGAME_API_TABLE(api) >= 11. */
    int (*nand_read_bulk)(unsigned ce, uint32_t raw_addr, void *buf,
                          uint32_t words, uint32_t *meta4, unsigned flags);
    /* v11, read-only NAND ECC-ARMING + BEAT experiment (nandscan8 Part B).
     * Exactly nand_read_beat's command/address/ready path; only the data-phase
     * arming differs, applied to the 16 B metadata phase and all four 1 KiB
     * payload phases:
     *   E0 today's arming (control)
     *   E1 +0x30=0x34 +0x14=0x10 +0x18=2
     *   E2 +0x30=0x34 +0x14=0x10 +0x18=1
     *   E3 +0x30=0x34 +0x14=0x10 +0x18 untouched
     *   E4 +0x30=0x34 +0x14=0x100 +0x18=2
     *   E5 +0x30=0x1023 +0x14=0x10 +0x18=2
     *   E6 as E1, drain WITHOUT the +0x30=3 re-arm (one armed phase then the
     *      PIO loop; the +0x00 mask and +0x14 = 0x10 are still written)
     *   E7 as E1, deselect by FMC+0x00 = 0 instead of 1 (RetailOS writes 0)
     * RetailOS's ECC branch sets +0x30 = 0x34/+0x14 = 0x10/+0x24 = 0 and then
     * runs the BCH decode at 0x084cbd14; that decode is an ECC step, NOT a
     * data mover (nandscan6), so it is deliberately not executed here. The
     * drain's +0x14 is the variant's armed value for E1..E6 (0x100 for E4,
     * 0x10 otherwise) and today's 0x100 for E0. Unknown variant -> -14
     * without touching hardware; -3 on bad ce/pointers. Read-only, passive
     * mode forced, no write/erase reachable. */
    int (*nand_read_ecc)(unsigned ce, uint32_t raw_addr, void *buf,
                         uint32_t meta[4], unsigned variant);
    /* v12, read-only SINGLE-ARMED-PHASE page read (nandscan9). run32 Part A
     * (hardware) proved ONE armed 17,408-byte transfer of CE0 page 0 delivers
     * rc 0, sent 0, and a stream beginning with the 16 metadata bytes
     * 69f5134b 816d19e4 c2034e36 daeedecd followed by the data
     * 3adc3861 e02d0015 1c2dc5d3 29544baf. Our normal page read arms FIVE
     * separate phases (16 B metadata + 4 x 1 KiB) and every read after the
     * first of a boot returns 0xCF filler then zeros (run25..31). This entry
     * point runs today's EXACT command/address/ready sequence and then exactly
     * ONE arming (as nand_read_bulk) and ONE PIO drain of `words` words. The
     * variant chooses only the framing:
     *   0 plain: drain then deselect (no trailing command)
     *   1 cmd(0x77) after the drain, then deselect
     *   2 cmd(0x77), cmd(0x7d), then deselect
     *   3 leave the chip selected (no deselect) so the caller can continue
     *   4 read the FULL page: nand.page_size/4 words (16,448 B -> 4,112 words
     *     on the hardware part, 16,384 B -> 4,096 in NanoEmu); `words` is
     *     ignored and nand_single_delivered() reports the words actually
     *     delivered
     * nand_single_delivered() = words the LAST drain delivered (words whose
     * +0x28 PIO counter advanced), 0 if the data phase never armed. Buffers are
     * never cleared: undelivered words keep the caller's prefill.
     * 0/-1/-2 as nand_read; -3 on bad ce/pointer; -15 on an unknown variant;
     * -16 on words out of range or an unusable page size (all rejected before
     * touching hardware). Passive mode forced, raw_addr passed through
     * unchanged, no write/erase exposed. A v12 game checks
     * NGAME_API_TABLE(api) >= 12. */
    int (*nand_read_single)(unsigned ce, uint32_t raw_addr, void *out,
                            uint32_t words, unsigned variant);
    uint32_t (*nand_single_delivered)(void);
    /* v13, read-only GATE/LENGTH page read (nandscan10). The hardware-annotated
     * N31 document documents the stock drain as sampling FIFOCTR BEFORE the
     * pop and retaining word n only when the sampled low byte is 4*n, and says
     * the older 4*n+4 test (counter read after the pop) discarded word zero of
     * every transfer. The low byte repeats every 64 words (64 x 4 = 256 bytes),
     * so that gate aliases every 64 words. This entry point runs
     * nand_read_single's exact command/address/ready sequence and ONE arming,
     * and varies only the single drain's GATE and LENGTH:
     *   0 FIXED gate: sample before pop, low byte == 4*i, discard a mismatch,
     *     4,096-stall guard
     *   1 pop `words` words unconditionally: no counter check, hard loop bound
     *     only; popped = words, counter-confirmed = 0
     *   2 FIXED gate compared as the full 32-bit value FIFOCTR == 4*i
     *   3 WITHDRAWN: returns -19 without touching hardware (a dummy read is
     *     impossible; a pop always consumes the FIFO, and the old gate already
     *     discarded a mismatched word by overwriting it on retry)
     *   4 FIXED gate at 2,176 words (half of run32's 4,352); `words` ignored
     *   5 FIXED gate at 8,704 words (double); `words` ignored
     *   6 HISTORICAL gate (pop, then low byte == 4*i+4), the explicit control
     *   7 exactly variant 0 plus a trailing cmd(0x77) before deselect
     * nand_words_delivered() = retained/counter-confirmed words,
     * nand_words_popped() = FIFO pops (retries included) and
     * nand_words_confirmed() = words whose FIFOCTR matched; all are 0 if the
     * phase never armed. Buffers are never cleared: undelivered words keep the
     * caller's prefill. run32's stream is a REFERENCE SIGNATURE only, and the
     * length reading is a hypothesis (run32/run33 were different boots and
     * builds).
     * 0/-1/-2 as nand_read; -3 on bad ce/pointer; -17 on an out-of-range
     * variant; -18 on words out of range; -19 on the withdrawn variant (all
     * rejected before touching hardware). Passive mode forced, raw_addr passed
     * through unchanged, every wait and loop bounded, no write/erase/program.
     * A v13 game checks NGAME_API_TABLE(api) >= 13. */
    int (*nand_read_words)(unsigned ce, uint32_t raw_addr, void *buf,
                           uint32_t words, unsigned variant);
    uint32_t (*nand_words_delivered)(void);
    uint32_t (*nand_words_popped)(void);
    uint32_t (*nand_words_confirmed)(void);
    /* v13, read-only SPARE-RECORD capture (nandscan10). The 16-byte sector
     * record is not in the PIO data stream; this performs the page-read setup
     * plus the stock spare-beat setup (FMLEN=0xF, FMUNK58=0x20, FMCTRL1=0x32),
     * FMUNK58=1 and a data beat, REC_LATCH=0x1102 with a bounded poll of bit 1
     * clear, then reads SPR0..3 four times (four records per page). rec[k][0..3]
     * = SPR0..3 of capture k. The data-beat length is not specified by the
     * document (4 KiB is used and recorded as UNKNOWN). No payload is drained,
     * no write/erase/program is reachable, every wait is bounded, passive mode
     * is forced. Returns 0/-1/-2 as the page read, -3 on bad ce/pointer. */
    int (*nand_read_record)(unsigned ce, uint32_t raw_addr, uint32_t rec[4][4]);
    /* v14: FIFOCTR trace, MEASUREMENT ONLY (no behaviour change). Enable,
     * do one read, then fetch. See nand.h for the field meanings. */
    int (*nand_trace_enable)(int on);
    uint32_t (*nand_trace_fetch)(uint32_t *dst, uint32_t max_entries);
    void (*nand_trace_stats)(uint32_t out[9]);
    /* v16: normal PPN storage sector, eight address bytes, SPR record + DMA.
     * Exactly 4096 bytes, sector 0..3, no NAND write/erase. See nand.h.
     * ngame_nand_info appends ppn_* transport diagnostics; prefix unchanged. */
    int (*nand_read_ppn)(unsigned ce, uint32_t row, unsigned sector,
                          void *buf, uint32_t meta[4]);
    /* v17 full PPN page, 16384 bytes and 16 metadata words. */
    int (*nand_read_ppn_page)(unsigned ce, uint32_t row, void *buf, uint32_t meta[16]);
    /* v18 reserved VFL context reads; no write/erase operation exposed. */
    int (*nand_read_ppn_context)(unsigned block, unsigned page, unsigned view, unsigned count,
                                 void *buf, uint32_t meta[16]);
    /* v19 inspection: -2 exposes fully transferred diagnostic bytes; other
     * failures preserve outputs. Errored non-erased bytes must not be used. */
    int (*nand_inspect_ppn_page)(unsigned ce, uint32_t row, void *buf, uint32_t meta[16]);
    /* v20: last-scene MBX timings in microseconds. Read-only snapshot, no GPU
     * commands. out[0..11]: begin,submit,TAwait,region,ISPsetup,ISPwait,extraWait,
     * poolScan,renderTotal,bufferInit,contextSetup,rearmed(0/1). */
    void (*mbx_timings)(uint32_t out[12]);
    /* v21: consume a header and native interleaved vertex span immediately,
     * preserving one draw packet/doorbell. No retained caller pointers. */
    void (*mbx_submit_parts)(const uint32_t *,uint32_t,const void *,uint32_t);

    /* v22: optional one-ISP queue while another TA frame is prepared. Check
     * capacity before calling render_async; wait_done collects completion.
     * Client arrays are still consumed immediately by submit(_parts). */
    uint32_t (*mbx_queue_capacity)(void);
    int (*mbx_render_async)(const uint32_t *,uint32_t,uint32_t,uint32_t);
    /* v23: no-copy native240x432 ARGB8888 scanout,128-byte aligned uncached
     * game-GPU source. Acceptance reserves its bytes until buffer_busy=0;
     * call lcd_service to progress/retry.0 accepted,-1 invalid,-2 busy. */
    int (*lcd_present_buffer)(const uint32_t *);
    int (*lcd_buffer_busy)(const uint32_t *,uint32_t);
};
#endif
