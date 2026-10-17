#ifndef NANOOS_REPORT_H
#define NANOOS_REPORT_H
#include <stdint.h>
/* RAM handoff of the DevMode GPU test reports to NanoOSLoader, which exports
 * them to /Apps/NanoOS/reports/ under RetailOS on its next launch. The
 * region was all-zero and unchanged in RAM snapshots 3/4/5 (0x0ae02000..
 * 0x0af38000, untouched by RetailOS at runtime, outside the GPU's 32 MB
 * window). Whether it survives the WDT reset and the RetailOS boot is the
 * open question: no magic at the loader = it did not. Shared with
 * apps/NanoOSLoader/loader.c; the D-cache is off in NanoOS, so plain stores
 * reach DRAM before the reset. */
#define RPT_BASE 0x0ae10000u
#define RPT_SLOTS 5u
#define RPT_DATA 0x1000u      /* header page, then the slots */
#define RPT_SLOT_SIZE 0x30000u
/* ViceNano M8 GPU diag (the "MBX unavailable" / preview lines): DevMode
 * uses slots 0..3 (R50..R53), M8 owns the last slot -> R80.txt */
#define RPT_M8_SLOT 4u
#define RPT_M8_ID 80u
#define RPT_MAGIC0 0x5258424du /* "MBXRPT1\0" */
#define RPT_MAGIC1 0x00315450u
struct rpt_hdr {
    uint32_t magic[2];
    uint32_t slot_size;
    uint32_t id[RPT_SLOTS];   /* case number (R50..R53 in round 5), 0 = empty */
    uint32_t len[RPT_SLOTS];  /* text bytes in the slot */
    uint32_t sum[RPT_SLOTS];  /* rpt_sum() of those bytes */
    uint32_t hdr_sum;         /* rpt_sum() of everything above */
};
#define RPT_HDR ((volatile struct rpt_hdr *)RPT_BASE)
#define RPT_SLOT(i) ((char *)(RPT_BASE + RPT_DATA + (i) * RPT_SLOT_SIZE))
/* FNV-1a */
static inline uint32_t rpt_sum(const volatile void *p, uint32_t n) {
    const volatile uint8_t *b = (const volatile uint8_t *)p; /* C++ (ViceNano) too */
    uint32_t s = 0x811c9dc5u;
    while (n--) s = (s ^ *b++) * 16777619u;
    return s;
}
static inline uint32_t rpt_hdr_sum(void) {
    return rpt_sum(RPT_HDR, __builtin_offsetof(struct rpt_hdr, hdr_sum));
}
static inline int rpt_valid(void) {
    volatile struct rpt_hdr *h = RPT_HDR;
    return h->magic[0] == RPT_MAGIC0 && h->magic[1] == RPT_MAGIC1 &&
           h->slot_size == RPT_SLOT_SIZE && h->hdr_sum == rpt_hdr_sum();
}
/* Round 12 mirrors of the M8 slot (R80). Snapshots 2..6 show 0x0ae10000 is
 * NOT a hole: it is a free block of a RetailOS allocator chain (1.3 MB blocks
 * linked from 0x0ae00000; SQLite/FS buffers come from it), so whether R80
 * survives depends on what RetailOS allocated before the read. Each mirror
 * is a small copy (same rpt_hdr, slot_size = RPT_MIR_SIZE marks it, only
 * slot RPT_M8_SLOT set, its text at base + RPT_DATA) in a different region
 * that was all-zero in every snapshot, in different allocator blocks/pools
 * and outside the GPU's 32 MB window (0x08000000..0x0a000000). Whichever
 * survives is read; pull_reports.py also prints which ones did (a survival
 * map that picks the next round's safe region). */
#define RPT_MIR_SIZE 0x4000u  /* text cap per mirror (R80 is ~3..6 KB) */
#define RPT_MIRRORS 6u
static const uint32_t rpt_mirror_base[RPT_MIRRORS] = {
    0x0af60000u, /* last block of the chain (next = 0) */
    0x0acb0000u, /* block 0x0acad000 */
    0x0a0d0000u, 0x0a300000u, /* zero windows below the chain */
    0x0ba00000u, /* below the 0x0ba5f000.. page-frame pool */
    0x0be60000u, /* zero window inside that pool */
};
static inline int rpt_mirror_valid(uint32_t base) {
    volatile struct rpt_hdr *h = (volatile struct rpt_hdr *)base;
    uint32_t n = h->len[RPT_M8_SLOT];
    return h->magic[0] == RPT_MAGIC0 && h->magic[1] == RPT_MAGIC1 &&
           h->slot_size == RPT_MIR_SIZE && n && n <= RPT_MIR_SIZE &&
           h->hdr_sum == rpt_sum(h, __builtin_offsetof(struct rpt_hdr, hdr_sum)) &&
           h->sum[RPT_M8_SLOT] == rpt_sum((const volatile void *)(base + RPT_DATA), n);
}
/* copy the M8 slot text (len bytes, sum) into every mirror */
static inline void rpt_mirror_write(const char *text, uint32_t len, uint32_t sum) {
    if (len > RPT_MIR_SIZE) { len = RPT_MIR_SIZE; sum = rpt_sum(text, len); }
    for (uint32_t m = 0; m < RPT_MIRRORS; ++m) {
        volatile struct rpt_hdr *h = (volatile struct rpt_hdr *)rpt_mirror_base[m];
        volatile char *d = (volatile char *)(rpt_mirror_base[m] + RPT_DATA);
        for (uint32_t i = 0; i < sizeof *h / 4; ++i) ((volatile uint32_t *)h)[i] = 0;
        for (uint32_t i = 0; i < len; ++i) d[i] = text[i];
        h->slot_size = RPT_MIR_SIZE;
        h->id[RPT_M8_SLOT] = RPT_M8_ID;
        h->len[RPT_M8_SLOT] = len;
        h->sum[RPT_M8_SLOT] = sum;
        h->magic[0] = RPT_MAGIC0;
        h->magic[1] = RPT_MAGIC1;
        h->hdr_sum = rpt_sum(h, __builtin_offsetof(struct rpt_hdr, hdr_sum));
    }
}
/* first valid mirror (base) or 0; NanoOSLoader exports from it directly
 * when the main mailbox is gone (no bulk writes into live RetailOS RAM) */
static inline uint32_t rpt_mirror_find(void) {
    for (uint32_t m = 0; m < RPT_MIRRORS; ++m)
        if (rpt_mirror_valid(rpt_mirror_base[m])) return rpt_mirror_base[m];
    return 0;
}
/* after a successful export: mirrors whose header is still intact lose
 * their magic (a reused block is left alone) */
static inline void rpt_mirror_clear(void) {
    for (uint32_t m = 0; m < RPT_MIRRORS; ++m)
        if (rpt_mirror_valid(rpt_mirror_base[m]))
            ((volatile struct rpt_hdr *)rpt_mirror_base[m])->magic[0] = 0;
}
#endif
