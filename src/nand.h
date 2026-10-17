#ifndef NANOOS_NAND_H
#define NANOOS_NAND_H
#include <stdint.h>
#define NAND_SECTOR 4096u
/* id/id2: the 8 READ ID bytes, little-endian ("PPN" 01 = 0x014e5050);
 * geometry from the parameter page; errors: expired waits / stalled PIO
 * words; cs: FMC +0x00 as found; st: last polled status byte (bit 4 =
 * error); tm: the chip-select timing that answered READ ID */
struct nand_info {
    uint32_t id, id2, caus, blocks, pages, page_size, errors, cs, st, tm;
    /* round 7 (device regression): probes this boot, the attempt that
     * answered (1..3), FMC +0x48 as found, failed waits after each attempt */
    uint32_t probes, attempt, st48, err_at[3];
    /* round 8: ctl_init result (0 ok, else the step that timed out); the
     * probe's first failed phase (wt bit: 2 cmd, 4 addr, 8 data arm, 0x100
     * PIO stall), +0x48 and +0x00 read at that failure */
    uint32_t init, ph, ph48, ph00;
};
extern struct nand_info nand;
/* 1 if a PPN NAND answered on CE0 and its parameter page read. RetailOS's
 * controller reset first, then three attempts: slow timing after a 0xff
 * reset; runtime timing; slow 0xff reset after an FMC quiesce (nand.c).
 * nand.errors counts this probe's failed waits. */
int nand_probe(void);
/* Raw read of one 4 KB sector (no FTL): page = cau << 19 | block << 7 |
 * page. 0 ok, -1 timeout, -2 error status. meta gets the sector's 16 bytes
 * of metadata (sftl: +0 type 1 = data, +1 bit 1 = uECC, +8 LBA). */
int nand_read_sector(unsigned ce, uint32_t page, unsigned sector,
                     void *buf, uint32_t meta[4]);
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
