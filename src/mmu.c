#include <stdint.h>
#include "mmu.h"
#include "memmap.h"
#if NANOOS_MMU
/* short-descriptor sections, domain 0, AP=11 (TEX remap off): */
#define SEC_WB 0x1c0eu   /* Normal, inner/outer write-back write-allocate */
#define SEC_SO 0x0c02u   /* Strongly-ordered */
#define SEC_XN 0x10u
static uint32_t ttb[4096] __attribute__((aligned(16384)));

/* DCCISW over every set/way of L1 (the A5 has no L2, cpu-deep-dive.md) */
void dcache_clean_all(void) {
    uint32_t id;
    __asm__ volatile("mcr p15, 2, %1, c0, c0, 0\n isb\n mrc p15, 1, %0, c0, c0, 0"
                     : "=r"(id) : "r"(0));
    unsigned line = (id & 7) + 4, ways = id >> 3 & 0x3ff, shift = __builtin_clz(ways);
    for (int w = (int)ways; w >= 0; --w)
        for (int s = (int)(id >> 13 & 0x7fff); s >= 0; --s)
            __asm__ volatile("mcr p15, 0, %0, c7, c14, 2"
                             :: "r"((uint32_t)w << shift | (uint32_t)s << line));
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Entered with the MMU and D-cache off (the chainloader cleaned and
 * invalidated the D-cache before turning it off); the table is written
 * uncached, so the walker (non-cacheable walks) reads it from DRAM. */
void mmu_init(void) {
    for (uint32_t i = 0; i < 4096; ++i) {
        uint32_t s = i - (MM_DRAM >> 20), d = SEC_SO;
        if (s < 64) d = s < 32 && (MM_UNCACHED >> s & 1) ? SEC_SO : SEC_WB;
        else if (i >= 0x380) d = SEC_SO | SEC_XN;   /* MMIO 0x38000000.. */
        ttb[i] = i << 20 | d;
    }
    dcache_clean_all();   /* no line may be valid when C goes on */
    uint32_t r;
    __asm__ volatile(
        "mcr p15, 0, %1, c8, c7, 0\n"    /* TLBIALL */
        "mcr p15, 0, %1, c2, c0, 2\n"    /* TTBCR = 0: TTBR0 only */
        "mcr p15, 0, %2, c2, c0, 0\n"    /* TTBR0, non-cacheable walks */
        "mcr p15, 0, %3, c3, c0, 0\n"    /* DACR: domain 0 client */
        "dsb sy\n isb\n"
        "mrc p15, 0, %0, c1, c0, 0\n"
        /* off: A (alignment faults), TRE (TEX remap), AFE; on: M, C */
        "bic %0, %0, #0x30000000\n"
        "bic %0, %0, #2\n"
        "orr %0, %0, #5\n"
        "mcr p15, 0, %0, c1, c0, 0\n"
        "isb\n"
        "mcr p15, 0, %1, c7, c5, 0\n"    /* ICIALLU */
        "mcr p15, 0, %1, c7, c5, 6\n"    /* BPIALL */
        "dsb sy\n isb"
        : "=&r"(r) : "r"(0), "r"(ttb), "r"(1) : "memory");
}
#endif
