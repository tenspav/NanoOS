/* bigload.c -- see bigload.h. Freestanding C99, no libc. */
#include "bigload.h"

uint32_t bigload_crc32_update(uint32_t c, const void *data, uint32_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return c;   /* caller starts with ~0u and finishes with ~ */
}

const char *bigload_reason_name(int r)
{
    switch (r) {
    case BIGLOAD_OK: return "ok";
    case BIGLOAD_E_ARGS: return "args";
    case BIGLOAD_E_SIZE_ZERO: return "size0";
    case BIGLOAD_E_SIZE_SMALL: return "size<32";
    case BIGLOAD_E_SIZE_ALIGN: return "size%4";
    case BIGLOAD_E_SIZE_BIG: return "size>region";
    case BIGLOAD_E_TOO_MANY_CHUNKS: return "chunks>64";
    case BIGLOAD_E_ALLOC_NULL: return "heap-exhausted";
    case BIGLOAD_E_TOO_MANY_LEAKS: return "too-many-leaks";
    case BIGLOAD_E_ATTEMPTS: return "attempts";
    case BIGLOAD_E_SHORT_READ: return "short-read";
    case BIGLOAD_E_BAD_MAGIC: return "bad-magic";
    case BIGLOAD_E_BAD_ABI: return "bad-abi";
    case BIGLOAD_E_BAD_HEADER: return "bad-header";
    case BIGLOAD_E_BAD_CRC: return "bad-crc";
    }
    return "?";
}

/* may the block [a, a+n) be handed to the kernel as a chunk/descriptor? */
static int bl_addr_ok(uint32_t a, uint32_t n)
{
    if (a < BIGLOAD_MIN_ADDR || (a & 3u)) return 0;
    if (n == 0 || n > 0xffffffffu - a) return 0;           /* 32-bit overflow */
    if (a + n > BIGLOAD_DRAM_END) return 0;
    if (!(a + n <= BIGLOAD_GAME_LO || a >= BIGLOAD_GAME_HI)) return 0;
    return 1;
}

static uint32_t bl_addr(const bigload_env *e, const void *p)
{
    return e->addr ? e->addr(e->ctx, p) : (uint32_t)(uintptr_t)p;
}

/* Get one acceptable block. Rejected blocks are kept (leaks[]) so the
 * allocator cannot return them again. Returns NULL and sets *reason on stop. */
static void *bl_get(const bigload_env *e, uint32_t n, void **leaks,
                    bigload_result *r, int *reason, uint32_t *addr_out)
{
    for (;;) {
        if (r->attempts >= BIGLOAD_MAX_ATTEMPTS) { *reason = BIGLOAD_E_ATTEMPTS; return 0; }
        r->attempts++;
        void *p = e->alloc(e->ctx, n);
        if (!p) { *reason = BIGLOAD_E_ALLOC_NULL; return 0; }
        uint32_t a = bl_addr(e, p);
        if (bl_addr_ok(a, n)) { *addr_out = a; return p; }
        if (r->leaks >= BIGLOAD_MAX_LEAKS) {
            /* no slot to track this block: it stays allocated (never freed),
             * so the allocator cannot hand it out again; stop staging. */
            *reason = BIGLOAD_E_TOO_MANY_LEAKS; return 0;
        }
        leaks[r->leaks++] = p;
    }
}

int bigload_stage(const bigload_env *e, const char *path, bigload_result *out)
{
    bigload_result r;
    void *leaks[BIGLOAD_MAX_LEAKS];
    void *cptr[BIGLOAD_MAX_CHUNKS];
    uint32_t clen[BIGLOAD_MAX_CHUNKS];
    uint32_t caddr[BIGLOAD_MAX_CHUNKS];
    struct bigload_desc *desc = 0;
    uint32_t daddr = 0, nch = 0, size, i;
    int reason = BIGLOAD_OK;
    char *rz = (char *)&r;
    for (i = 0; i < sizeof r; i++) rz[i] = 0;
    r.status = -1;

    if (!e || !path || !out || !e->size || !e->alloc || !e->free || !e->read_scatter) {
        if (out) { char *o = (char *)out; for (i = 0; i < sizeof *out; i++) o[i] = 0;
                   out->status = -1; out->reason = BIGLOAD_E_ARGS; }
        return -1;
    }
    size = e->size(e->ctx, path);
    r.game_size = size;
    if (size == 0) reason = BIGLOAD_E_SIZE_ZERO;
    else if (size < BIGLOAD_HDR_BYTES) reason = BIGLOAD_E_SIZE_SMALL;
    else if (size & 3u) reason = BIGLOAD_E_SIZE_ALIGN;
    else if (size > BIGLOAD_MAX_SIZE) reason = BIGLOAD_E_SIZE_BIG;
    else if ((size + BIGLOAD_CHUNK_BYTES - 1u) / BIGLOAD_CHUNK_BYTES > BIGLOAD_MAX_CHUNKS)
        reason = BIGLOAD_E_TOO_MANY_CHUNKS;
    if (reason) goto fail;

    /* descriptor first (small), then chunks */
    desc = (struct bigload_desc *)bl_get(e, (uint32_t)sizeof *desc, leaks, &r, &reason, &daddr);
    if (!desc) goto fail;
    {
        uint32_t *z = (uint32_t *)desc;
        for (i = 0; i < sizeof *desc / 4; i++) z[i] = 0;
    }
    {
        uint32_t left = size;
        while (left) {
            uint32_t n = left > BIGLOAD_CHUNK_BYTES ? BIGLOAD_CHUNK_BYTES : left;
            void *p = bl_get(e, n, leaks, &r, &reason, &caddr[nch]);
            if (!p) goto fail;
            cptr[nch] = p; clen[nch] = n; nch++; r.chunks = nch;
            left -= n;
        }
    }

    r.bytes = e->read_scatter(e->ctx, path, (void *const *)cptr, clen, (int)nch);
    if (r.bytes != size) { reason = BIGLOAD_E_SHORT_READ; goto fail; }

    {   /* header checks: same as game.c valid_header (+ NGM1 abi) */
        const uint32_t *h = (const uint32_t *)cptr[0];
        uint32_t load = h[2];
        if (h[0] != BIGLOAD_NGM1_MAGIC) { reason = BIGLOAD_E_BAD_MAGIC; goto fail; }
        if (h[1] != 1u) { reason = BIGLOAD_E_BAD_ABI; goto fail; }
        if (h[3] != size || (size & 3u) ||
            load - BIGLOAD_GAME_LO >= BIGLOAD_GAME_HI - BIGLOAD_GAME_LO || (load & 3u) ||
            size > BIGLOAD_GAME_HI - load ||
            h[4] < load + size || h[4] > BIGLOAD_GAME_HI ||
            h[5] - load - BIGLOAD_HDR_BYTES >= size - BIGLOAD_HDR_BYTES) {
            reason = BIGLOAD_E_BAD_HEADER; goto fail;
        }
        /* CRC over file bytes [32,size), streaming across chunks */
        uint32_t c = ~0u;
        for (i = 0; i < nch; i++) {
            const uint8_t *p = (const uint8_t *)cptr[i];
            uint32_t n = clen[i];
            if (i == 0) { p += BIGLOAD_HDR_BYTES; n -= BIGLOAD_HDR_BYTES; }
            c = bigload_crc32_update(c, p, n);
        }
        if (~c != h[6]) { reason = BIGLOAD_E_BAD_CRC; goto fail; }
    }

    desc->magic = BIGLOAD_NGLD_MAGIC; desc->version = 1u;
    desc->size = size; desc->nchunks = nch;
    for (i = 0; i < nch; i++) { desc->chunk[i].addr = caddr[i]; desc->chunk[i].len = clen[i]; }
    r.status = 0; r.reason = BIGLOAD_OK;
    r.desc = desc; r.desc_addr = daddr; r.desc_size = (uint32_t)sizeof *desc;
    *out = r;
    return 0;

fail:
    r.status = -1; r.reason = reason;
    for (i = 0; i < nch; i++) e->free(e->ctx, cptr[i]);
    if (desc) e->free(e->ctx, desc);
    for (i = 0; i < r.leaks; i++) e->free(e->ctx, leaks[i]);
    *out = r;
    return -1;
}
