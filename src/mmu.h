#ifndef NANOOS_MMU_H
#define NANOOS_MMU_H
/* CPU MMU: flat 1:1 map of 1 MB sections (memmap.h attributes), D-cache on.
 * NANOOS_MMU=0 builds the old MMU/D-cache off kernel (all memory
 * Strongly-ordered, I-cache and branch prediction on as startup.s left
 * them). The GPU's own MMU (mbx.c) is separate and unchanged. */
#ifndef NANOOS_MMU
#define NANOOS_MMU 1
#endif
#if NANOOS_MMU
void mmu_init(void);
/* clean + invalidate the whole D-cache to DRAM (before a reset, before
 * running freshly copied code) */
void dcache_clean_all(void);
#else
static inline void mmu_init(void) {}
static inline void dcache_clean_all(void) {}
#endif
#endif
