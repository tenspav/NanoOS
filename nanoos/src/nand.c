#include "nand.h"
#include "timer.h"
/* FMC 0x38a00000, PPN ("managed") NAND, direct register path (NanoEmu
 * nano7_fmc.c:6-16, 375-610; the model follows RetailOS's driver):
 *   +0x00 CE select: (1 << ce) << 1 | 1; 1 = none
 *   +0x04 go: 1 = send the address bytes; 0x32 = next sector's meta in
 *   +0x08 command byte (issued on write)
 *   +0x0c/+0x10 address bytes LSB first, +0x2c = count - 1
 *   +0x34 DRAM address: the sector moves there (DMA)
 *   +0x48 status, W1C: bits 1,2,3,20,23,24 done
 *   +0x60..+0x6c sector metadata, +0x80 PIO data FIFO (32-bit pops)
 * Commands: 0x90 ID (address 0: "PPN" 01 ..), 0x92 parameter page,
 * 0x07 queue a read (+0x0c byte0 = count << 4, bits 23:20 = first sector,
 * +0x10 = page), 0x7a data out of the oldest queued read.
 *
 * No FTL: NanoEmu feeds the disk image only to RetailOS's sequencer path
 * (FMSS, via its own FTL tables in DRAM), so here unwritten pages read as
 * 0xff. A logical layer (RetailOS's sftl, metadata type 1 = LBA) is the
 * next step; it is read-only work. */
#define FMC(o) (*(volatile uint32_t *)(0x38a00000u + (o)))
#define DONE 0x2u
#define WAIT_US 5000u
struct nand_info nand;
static int go(uint32_t v) {
    FMC(0x48) = 0xffffffffu;
    FMC(0x04) = v;
    if (wait_any(&FMC(0x48), DONE, WAIT_US) == WAIT_OK) return 0;
    ++nand.errors;
    return -1;
}
static void fmc_select(unsigned ce) { FMC(0x00) = (1u << ce) << 1 | 1u; }
static void fmc_deselect(void) { FMC(0x00) = 1; }
static int cmd_addr(unsigned cmd, unsigned count, uint32_t a0, uint32_t a1) {
    FMC(0x08) = cmd;
    FMC(0x2c) = (count - 1) & 7;
    FMC(0x0c) = a0;
    FMC(0x10) = a1;
    return go(1);
}
int nand_probe(void) {
    fmc_select(0);
    int r = cmd_addr(0x90, 1, 0, 0);
    nand.id = FMC(0x80);
    if (!r && cmd_addr(0x92, 1, 0, 0) == 0) {
        uint32_t p[14];
        for (unsigned i = 0; i < 14; ++i) p[i] = FMC(0x80);
        nand.caus = p[4];           /* +0x10 CAUs per channel */
        nand.blocks = p[6];         /* +0x18 blocks per CAU */
        nand.pages = p[8];          /* +0x20 pages per block */
        nand.page_size = p[13];     /* +0x34 */
    }
    fmc_deselect();
    return !r && (nand.id & 0xffffff) == 0x4e5050;   /* "PPN" */
}
int nand_read_sector(unsigned ce, uint32_t page, unsigned sector,
                     void *buf, uint32_t meta[4]) {
    fmc_select(ce);
    int r = cmd_addr(0x07, 8, 1u << 4 | (sector & 15) << 20, page);
    FMC(0x08) = 0x7a;
    if (!r) r = go(0x32);
    if (!r) {
        /* the sector moves on this write; RetailOS waits on the go's
         * status only (the model sets none here), so the caller checks
         * the data: drivers.c pre-fills a sentinel */
        FMC(0x34) = (uint32_t)buf;
        __asm__ volatile("dsb sy" ::: "memory");
        for (unsigned i = 0; i < 4; ++i) meta[i] = FMC(0x60 + 4 * i);
    }
    FMC(0x48) = 0xffffffffu;
    fmc_deselect();
    return r;
}
/* Programming (cmd 0x8a/0x87, meta, go 0x34, 4 x 1 KB DMA, confirm 0x17;
 * nano7_fmc.c:355-485) is deliberately not implemented: NanoOS also runs
 * on the user's iPod, and a write there changes the flash RetailOS's FTL
 * owns. It stays refused until it can be gated to the emulator and
 * reviewed. */
int nand_write_sector(unsigned ce, uint32_t page, unsigned sector,
                      const void *buf, const uint32_t meta[4]) {
    (void)ce; (void)page; (void)sector; (void)buf; (void)meta;
    return -1;
}
