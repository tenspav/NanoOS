/*
 * bigload.h -- chunked large-game staging planner for the RetailOS-side
 * chainloader (route (a) of NanoOS/src/gameabi.h). Pure logic, freestanding
 * C99, no libc: every OS service comes in through bigload_env so the planner
 * is host-testable. HARDWARE-UNVERIFIED; see bigtest/README.md.
 *
 * bigload_stage():
 *   - game size from env->size(); rejects 0, < 32, not a multiple of 4,
 *     > 0x00a00000 (game region), or needing > 64 chunks of 128 KiB;
 *   - allocates a 528-byte descriptor, then 128 KiB chunks (last = remainder);
 *     a block that is below 0x08800000, unaligned, overflowing, beyond
 *     0x0c000000 or overlapping [0x0b000000,0x0ba00000) is REJECTED but KEPT
 *     allocated (so the allocator cannot return it again) while retrying;
 *   - env->alloc returning NULL (exhaustion) ends staging at once;
 *   - leaks capped (BIGLOAD_MAX_LEAKS) and total alloc calls capped;
 *   - env->read_scatter fills the chunks in file order, byte count must equal
 *     size; the NGM1 header (magic, abi, size, load/bss/entry as game.c
 *     valid_header) and CRC-32 over [32,size) are verified here, so a corrupt
 *     file never reaches the kernel;
 *   - on any failure everything allocated (legit chunks, descriptor, leaks)
 *     is freed and an error is returned; on success nothing is freed and
 *     out->desc_addr / out->desc_size are what the handoff block gets.
 */
#ifndef NANOOS_BIGLOAD_H
#define NANOOS_BIGLOAD_H
#include <stdint.h>

#define BIGLOAD_CHUNK_BYTES   0x00020000u   /* 128 KiB */
#define BIGLOAD_MAX_CHUNKS    64u           /* NBOOT_CHUNKS */
#define BIGLOAD_MAX_LEAKS     24u
#define BIGLOAD_MAX_ATTEMPTS  128u          /* total env->alloc calls */
#define BIGLOAD_MIN_ADDR      0x08800000u   /* MM_STAGE */
#define BIGLOAD_DRAM_END      0x0c000000u   /* MM_DRAM_END */
#define BIGLOAD_GAME_LO       0x0b000000u   /* MM_GAME */
#define BIGLOAD_GAME_HI       0x0ba00000u   /* MM_GAME_END */
#define BIGLOAD_MAX_SIZE      (BIGLOAD_GAME_HI - BIGLOAD_GAME_LO)
#define BIGLOAD_NGM1_MAGIC    0x314d474eu
#define BIGLOAD_NGLD_MAGIC    0x444c474eu
#define BIGLOAD_HDR_BYTES     32u

/* identical layout to struct nboot_game in NanoOS/src/gameabi.h (528 bytes) */
struct bigload_desc {
    uint32_t magic, version;
    uint32_t size;
    uint32_t nchunks;
    struct { uint32_t addr, len; } chunk[BIGLOAD_MAX_CHUNKS];
};
typedef char bigload_desc_size_check[(sizeof(struct bigload_desc) == 528u) ? 1 : -1];

enum bigload_reason {
    BIGLOAD_OK = 0,
    BIGLOAD_E_ARGS,          /* bad env / path */
    BIGLOAD_E_SIZE_ZERO,
    BIGLOAD_E_SIZE_SMALL,    /* < 32 */
    BIGLOAD_E_SIZE_ALIGN,    /* not a multiple of 4 */
    BIGLOAD_E_SIZE_BIG,      /* > game region */
    BIGLOAD_E_TOO_MANY_CHUNKS,
    BIGLOAD_E_ALLOC_NULL,    /* heap exhausted */
    BIGLOAD_E_TOO_MANY_LEAKS,
    BIGLOAD_E_ATTEMPTS,
    BIGLOAD_E_SHORT_READ,
    BIGLOAD_E_BAD_MAGIC,
    BIGLOAD_E_BAD_ABI,
    BIGLOAD_E_BAD_HEADER,    /* size field / load / bss_end / entry */
    BIGLOAD_E_BAD_CRC
};

typedef struct bigload_env {
    void *ctx;
    uint32_t (*size)(void *ctx, const char *path);
    void *(*alloc)(void *ctx, uint32_t n);
    void (*free)(void *ctx, void *p);
    uint32_t (*read_scatter)(void *ctx, const char *path, void *const *bufs,
                             const uint32_t *lens, int nbufs);
    /* pointer -> 32-bit physical/virtual address (VA = PA under RetailOS).
     * NULL means a plain cast (the target); hosts supply a mapping. */
    uint32_t (*addr)(void *ctx, const void *p);
} bigload_env;

typedef struct bigload_result {
    int      status;        /* 0 ok, -1 failed */
    int      reason;        /* enum bigload_reason */
    uint32_t chunks;        /* legit chunks allocated */
    uint32_t leaks;         /* rejected blocks kept allocated */
    uint32_t bytes;         /* game bytes read into chunks */
    uint32_t attempts;      /* env->alloc calls */
    uint32_t desc_addr;     /* descriptor address, 0 unless success */
    uint32_t desc_size;     /* sizeof(struct bigload_desc), 0 unless success */
    uint32_t game_size;     /* env->size() result */
    struct bigload_desc *desc;
} bigload_result;

int bigload_stage(const bigload_env *env, const char *path, bigload_result *out);
const char *bigload_reason_name(int reason);
/* exposed for tests: CRC-32 IEEE/zlib, table-free, identical to NanoOS asset.c */
uint32_t bigload_crc32_update(uint32_t state, const void *data, uint32_t n);
#endif
