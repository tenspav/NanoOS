#ifndef NANOOS_NAND_H
#define NANOOS_NAND_H
#include <stdint.h>
#define NAND_SECTOR 4096u
/* id: first 4 ID bytes, little-endian ("PPN" 01 = 0x014e5050);
 * geometry from the parameter page; errors: expired waits */
struct nand_info { uint32_t id, caus, blocks, pages, page_size, errors; };
extern struct nand_info nand;
int nand_probe(void);   /* 1 if a PPN NAND answered on CE0 */
/* Raw read of one 4 KB sector (no FTL): page = cau << 19 | block << 7 |
 * page. 0 ok, -1 timeout. meta gets the sector's 16 bytes of metadata. */
int nand_read_sector(unsigned ce, uint32_t page, unsigned sector,
                     void *buf, uint32_t meta[4]);
/* Refused: see nand.c. */
int nand_write_sector(unsigned ce, uint32_t page, unsigned sector,
                      const void *buf, const uint32_t meta[4]);
#endif
