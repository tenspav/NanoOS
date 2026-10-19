#ifndef NANOOS_ASSET_H
#define NANOOS_ASSET_H
#include <stdint.h>
/* Asset pack handed over with the kernel: the loader appends an NPAK v1
 * pack (nanoos/installer/npak.py) to kernel.bin, startup.s moves it to
 * __asset_base (kernel.ld). Returns the named entry once its CRC32 matches,
 * or 0 (no pack, no such entry, damaged). */
const void *asset_find(const char *name, uint32_t *size);
/* CRC-32 (IEEE, as zlib), also game.c's image check */
uint32_t crc32(const void *data, uint32_t n);
#endif
