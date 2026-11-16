/* Host test for bigload.c (mock heap/file). Build: see bigtest/run.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../bigload.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- mock heap: handed-out "addresses" are chosen by a list; the real
 * storage is host memory, mapped through env->addr ---- */
#define MAXB 256
struct blk { void *mem; uint32_t addr, size; int live; };
static struct {
    uint32_t list[MAXB]; int nlist, next;   /* addresses to return in order; 0xffffffff = return NULL */
    int cyc;                                /* repeat last entry forever if 1 */
    struct blk b[MAXB]; int nb;
    int allocs, frees;
    uint8_t *file; uint32_t fsize, fread_limit; int reads;
    uint32_t seen_nbufs, seen_lens[80];
} M;
#define NULLADDR 0xffffffffu
static void mock_reset(void) {
    for (int i = 0; i < M.nb; i++) free(M.b[i].mem);
    free(M.file);
    memset(&M, 0, sizeof M);
}
static uint32_t m_size(void *c, const char *p) { (void)c; (void)p; return M.fsize; }
static void *m_alloc(void *c, uint32_t n) {
    (void)c; M.allocs++;
    uint32_t a;
    if (M.next < M.nlist) a = M.list[M.next++];
    else if (M.cyc && M.nlist) a = M.list[M.nlist - 1];
    else a = NULLADDR;
    if (a == NULLADDR || M.nb >= MAXB) return 0;
    struct blk *b = &M.b[M.nb++];
    b->mem = calloc(1, n + 16); memset(b->mem, 0xA5, n);
    b->addr = a; b->size = n; b->live = 1;
    return b->mem;
}
static void m_free(void *c, void *p) {
    (void)c; M.frees++;
    for (int i = 0; i < M.nb; i++) if (M.b[i].mem == p) { CHECK(M.b[i].live); M.b[i].live = 0; return; }
    CHECK(!"free of unknown pointer");
}
static uint32_t m_addr(void *c, const void *p) {
    (void)c;
    for (int i = 0; i < M.nb; i++) if (M.b[i].mem == p) return M.b[i].addr;
    CHECK(!"addr of unknown ptr"); return 0;
}
static uint32_t m_read(void *c, const char *p, void *const *bufs, const uint32_t *lens, int n) {
    (void)c; (void)p; M.reads++; M.seen_nbufs = (uint32_t)n;
    uint32_t off = 0, tot = 0;
    for (int i = 0; i < n; i++) {
        M.seen_lens[i] = lens[i];
        for (uint32_t k = 0; k < lens[i]; k++) {
            if (off >= M.fsize || tot >= M.fread_limit) return tot;
            ((uint8_t *)bufs[i])[k] = M.file[off++]; tot++;
        }
    }
    return tot;
}
static bigload_env env = { 0, m_size, m_alloc, m_free, m_read, m_addr };

/* ---- game image builder ---- */
static uint32_t crc32_ref(const uint8_t *p, uint32_t n) {   /* copy of NanoOS/src/asset.c */
    uint32_t c = ~0u;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320u & -(c & 1)); }
    return ~c;
}
static void mkgame(uint32_t size) {
    M.file = calloc(1, size ? size : 4); M.fsize = size; M.fread_limit = 0xffffffffu;
    uint32_t *h = (uint32_t *)M.file;
    if (size < 32) return;
    for (uint32_t i = 32; i < size; i++) M.file[i] = (uint8_t)(i * 31u + (i >> 8));
    h[0] = 0x314d474e; h[1] = 1; h[2] = 0x0b000000; h[3] = size;
    h[4] = 0x0b000000 + size + 0x1000; h[5] = 0x0b000000 + 32; h[6] = crc32_ref(M.file + 32, size - 32);
}
static void set_list(int n, const uint32_t *l, int cyc) { memcpy(M.list, l, n * sizeof *l); M.nlist = n; M.cyc = cyc; }
/* generate good addresses at 0x08900000 + k*0x20000 .. skipping the game region by construction */
static void good_list(int n) {
    for (int i = 0; i < n; i++) M.list[i] = 0x08900000u + (uint32_t)i * 0x00030000u;   /* ends below 0x0b000000 for i<~55 */
    M.nlist = n;
}

/* ---- host re-implementation of game.c game_boot checks (copied logic) ---- */
#define MM_STAGE 0x08800000u
#define MM_DRAM_END 0x0c000000u
#define MM_GAME 0x0b000000u
#define MM_GAME_END 0x0ba00000u
static int outside(uint32_t a, uint32_t n, uint32_t lo, uint32_t hi) { return a + n <= lo || a >= hi; }
static int valid_header(const uint32_t *g, uint32_t size) {
    uint32_t load = g[2];
    return size >= 32 && g[0] == 0x314d474eu && g[1] == 1 && g[3] == size && !(size & 3u) &&
        load - MM_GAME < MM_GAME_END - MM_GAME && !(load & 3u) && size <= MM_GAME_END - load &&
        g[4] >= load + size && g[4] <= MM_GAME_END &&
        g[5] - load - 32 < size - 32;
}
/* returns 1 if game_boot route (a) would accept AND CRC of reassembled image ok */
static int kernel_accepts(uint32_t dptr, uint32_t dsz, const bigload_result *r) {
    if (dptr == 0 || dsz != 528) return 0;      /* fall back to asset route, not route (a) */
    const struct bigload_desc *b = r->desc;     /* host mapping of dptr */
    if (dptr < MM_STAGE || dptr > MM_DRAM_END - sizeof *b ||
        b->magic != 0x444c474eu || b->version != 1 || b->nchunks > 64 || !b->nchunks ||
        b->chunk[0].len < 32) return 0;
    if (!outside(dptr, sizeof *b, MM_GAME, MM_GAME_END)) return 0;   /* read during the copy */
    uint32_t total = 0;
    for (uint32_t i = 0; i < b->nchunks; i++) {
        uint32_t a = b->chunk[i].addr, n = b->chunk[i].len;
        if (a < MM_STAGE || (a & 3) || n > MM_DRAM_END - a || !outside(a, n, MM_GAME, MM_GAME_END) ||
            ((n & 3) && i + 1 < b->nchunks)) return 0;
        total += n;
    }
    /* find host memory of chunk 0 by address, reassemble in file order */
    uint8_t *img = malloc(total); uint32_t off = 0;
    for (uint32_t i = 0; i < b->nchunks; i++) {
        uint8_t *src = 0;
        for (int k = 0; k < M.nb; k++) if (M.b[k].addr == b->chunk[i].addr && M.b[k].live) src = M.b[k].mem;
        if (!src) { free(img); return 0; }
        memcpy(img + off, src, b->chunk[i].len); off += b->chunk[i].len;
    }
    const uint32_t *g = (const uint32_t *)img;
    int ok = g[3] == b->size && g[3] == total && valid_header(g, g[3]) &&
             crc32_ref(img + 32, total - 32) == g[6] && !memcmp(img, M.file, total);
    free(img);
    return ok;
}

static int live_count(void) { int n = 0; for (int i = 0; i < M.nb; i++) n += M.b[i].live; return n; }
static int live_in_game(void) { int n = 0; for (int i = 0; i < M.nb; i++) if (M.b[i].live && !outside(M.b[i].addr, M.b[i].size, MM_GAME, MM_GAME_END)) n++; return n; }
static int all_live_in_game(void) { /* every live block must be a leaked bad one */
    int n = 0; for (int i = 0; i < M.nb; i++) if (M.b[i].live) n++; return n; }
static void fail_expect(int status, bigload_result *r, int reason, const char *name) {
    CHECK(status == -1); CHECK(r->status == -1); CHECK(r->reason == reason);
    CHECK(r->desc_addr == 0 && r->desc_size == 0);
    if (r->reason != reason) printf("  (%s) reason=%s\n", name, bigload_reason_name(r->reason));
}

int main(void) {
    bigload_result r; int rc;

    /* crc matches reference */
    { uint8_t t[100]; for (int i = 0; i < 100; i++) t[i] = (uint8_t)(i * 7);
      CHECK(~bigload_crc32_update(~0u, t, 100) == crc32_ref(t, 100));
      CHECK(~bigload_crc32_update(~0u, "123456789", 9) == 0xcbf43926u); }

    /* success: 300000 bytes -> 3 chunks (128K,128K,remainder) */
    mock_reset(); mkgame(300000); good_list(10);
    rc = bigload_stage(&env, "g", &r);
    CHECK(rc == 0); CHECK(r.status == 0); CHECK(r.reason == 0);
    CHECK(r.chunks == 3); CHECK(r.leaks == 0); CHECK(r.bytes == 300000);
    CHECK(r.desc_size == 528 && r.desc_addr == M.list[0]);
    CHECK(r.desc->nchunks == 3 && r.desc->size == 300000 && r.desc->magic == 0x444c474e && r.desc->version == 1);
    CHECK(r.desc->chunk[0].addr == M.list[1] && r.desc->chunk[0].len == 131072);
    CHECK(r.desc->chunk[1].addr == M.list[2] && r.desc->chunk[1].len == 131072);
    CHECK(r.desc->chunk[2].addr == M.list[3] && r.desc->chunk[2].len == 300000 - 262144);
    CHECK(r.desc->chunk[3].addr == 0 && r.desc->chunk[63].len == 0);   /* zeroed tail */
    CHECK(M.reads == 1 && M.seen_nbufs == 3 && M.seen_lens[0] == 131072);
    CHECK(live_count() == 4 && M.frees == 0);
    CHECK(kernel_accepts(r.desc_addr, r.desc_size, &r));

    /* exact multiple of chunk, 1 chunk, minimum 32 */
    mock_reset(); mkgame(262144); good_list(10);
    CHECK(bigload_stage(&env, "g", &r) == 0 && r.chunks == 2 && r.desc->chunk[1].len == 131072 && kernel_accepts(r.desc_addr, r.desc_size, &r));
    mock_reset(); mkgame(36); good_list(4);  /* 32 can never pass valid_header (entry range empty) */
    CHECK(bigload_stage(&env, "g", &r) == 0 && r.chunks == 1 && r.desc->chunk[0].len == 36 && kernel_accepts(r.desc_addr, r.desc_size, &r));
    /* max size 10 MiB = 80 chunks > 64 -> rejected as too many chunks (kernel limit is 64 x 128K = 8 MiB) */
    mock_reset(); mkgame(0x00a00000); good_list(3);
    rc = bigload_stage(&env, "g", &r); fail_expect(rc, &r, BIGLOAD_E_TOO_MANY_CHUNKS, "10MiB"); CHECK(M.allocs == 0);
    /* 8 MiB exactly = 64 chunks: ok */
    mock_reset(); mkgame(0x00800000);
    for (int i = 0; i < 65; i++) M.list[i] = 0x08900000u + (uint32_t)i * 0x00020000u;   /* 0x08900000 .. up to 0x0b0xxxxx */
    /* make addresses avoid the game region: spread below it using 0x40000 step from 0x08900000 -> exceeds. use two ranges */
    for (int i = 0; i < 65; i++) M.list[i] = (i < 40) ? 0x08900000u + (uint32_t)i * 0x00020000u : 0x0ba00000u + (uint32_t)(i - 40) * 0x00020000u;
    M.nlist = 65;
    rc = bigload_stage(&env, "g", &r);
    CHECK(rc == 0 && r.chunks == 64 && kernel_accepts(r.desc_addr, r.desc_size, &r));

    /* size rejections: no allocation at all */
    uint32_t bad[] = { 0, 4, 28, 33, 0x00a00004u, 0x01000000u, 0xfffffffcu, 0xffffffffu };
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
        mock_reset(); M.fsize = bad[i]; M.fread_limit = 0xffffffffu; good_list(4);
        rc = bigload_stage(&env, "g", &r);
        CHECK(rc == -1); CHECK(M.allocs == 0 && M.reads == 0);
        CHECK(r.reason == (bad[i] == 0 ? BIGLOAD_E_SIZE_ZERO : bad[i] < 32 ? BIGLOAD_E_SIZE_SMALL :
              (bad[i] & 3) ? BIGLOAD_E_SIZE_ALIGN : BIGLOAD_E_SIZE_BIG));
        CHECK(r.desc_addr == 0);
    }

    /* blocks landing in / around the game region, below stage, unaligned, overflow: rejected+kept */
    {
        uint32_t l[] = {
            0x0b000000u,            /* descriptor inside game region start */
            0x0afffff0u,            /* desc 528 bytes straddles into game */
            0x08700000u,            /* below MM_STAGE */
            0x09000001u,            /* unaligned */
            0x09100000u,            /* desc OK */
            0x0b100000u,            /* chunk inside */
            0x0afe0000u + 0x1c000u, /* straddles lo: 0x0affc000+128K crosses 0x0b000000 */
            0x0b9f0000u,            /* straddles hi end inside */
            0xfffe0000u,            /* overflow */
            0x0bfe0000u + 0x10000u, /* 0x0bff0000+128K > DRAM_END */
            0x0ba00000u,            /* first legit (starts exactly at GAME_END) */
            0x0afe0000u,            /* legit: ends exactly at GAME_LO */
            0x09200000u,            /* legit (last chunk) */
        };
        mock_reset(); mkgame(300000); set_list(13, l, 0);
        rc = bigload_stage(&env, "g", &r);
        CHECK(rc == 0); CHECK(r.desc_addr == 0x09100000u); CHECK(r.leaks == 9);
        CHECK(r.chunks == 3);
        CHECK(r.desc->chunk[0].addr == 0x0ba00000u && r.desc->chunk[1].addr == 0x0afe0000u && r.desc->chunk[2].addr == 0x09200000u);
        CHECK(kernel_accepts(r.desc_addr, r.desc_size, &r));
        CHECK(live_count() == 4 + (int)r.leaks);       /* leaks kept allocated */
        CHECK(M.frees == 0);
        /* now do the same failing at the read stage to check cleanup */
    }
    {   /* desc leaks 4, chunk leak 1 (0x0b100000), chunk at 0x0ba00000 is legit */
        uint32_t l[] = { 0x0b000000u, 0x0afffff0u, 0x08700000u, 0x09000001u, 0x09100000u,
                         0x0b100000u, 0x0ba00000u, 0x0b9f0000u, 0x09200000u };
        mock_reset(); mkgame(131072); set_list(9, l, 0);
        rc = bigload_stage(&env, "g", &r);
        CHECK(rc == 0 && r.leaks == 5 && r.chunks == 1 && r.attempts == 7);
        CHECK(r.desc_addr == 0x09100000u && r.desc->chunk[0].addr == 0x0ba00000u);
        CHECK(kernel_accepts(r.desc_addr, r.desc_size, &r) && M.frees == 0);
    }

    /* failure after leaks: legit freed, leaks freed too (per spec: freed, not handed back while retrying) */
    {
        uint32_t l[] = { 0x0b000000u, 0x09100000u, 0x0b200000u, 0x09200000u, 0x09300000u, 0x09400000u };
        mock_reset(); mkgame(300000); set_list(6, l, 0);
        M.fread_limit = 1000;   /* short read */
        rc = bigload_stage(&env, "g", &r);
        fail_expect(rc, &r, BIGLOAD_E_SHORT_READ, "short");
        CHECK(r.leaks == 2 && r.chunks == 3 && r.bytes == 1000);
        CHECK(live_count() == 0 && M.frees == M.nb);
    }
    /* exhaustion (NULL) mid-way: free all */
    {
        uint32_t l[] = { 0x09100000u, 0x09200000u, 0x0b100000u, 0x09300000u, NULLADDR };
        mock_reset(); mkgame(500000); set_list(5, l, 0);
        rc = bigload_stage(&env, "g", &r);
        fail_expect(rc, &r, BIGLOAD_E_ALLOC_NULL, "null");
        CHECK(live_count() == 0); CHECK(M.reads == 0); CHECK(r.leaks == 1 && r.chunks == 2);
    }
    /* NULL immediately for descriptor */
    mock_reset(); mkgame(100000); M.nlist = 0;
    rc = bigload_stage(&env, "g", &r); fail_expect(rc, &r, BIGLOAD_E_ALLOC_NULL, "null0"); CHECK(live_count() == 0 && M.allocs == 1);

    /* allocator only returns bad blocks: bounded by leak cap, no hang */
    {
        uint32_t l[] = { 0x0b100000u };
        mock_reset(); mkgame(300000); set_list(1, l, 1);
        rc = bigload_stage(&env, "g", &r);
        fail_expect(rc, &r, BIGLOAD_E_TOO_MANY_LEAKS, "badonly");
        CHECK(r.leaks == BIGLOAD_MAX_LEAKS); CHECK(M.allocs == (int)BIGLOAD_MAX_LEAKS + 1);
        CHECK(M.allocs <= (int)BIGLOAD_MAX_ATTEMPTS);
        /* the over-cap block stays allocated (cannot be tracked); all tracked ones freed */
        CHECK(live_count() == 1);
        CHECK(live_in_game() == 1);
    }
    /* attempts cap: alternate bad blocks cannot exceed cap (use leak cap higher? attempts>=leaks so just assert bound) */
    /* the same good block returned twice is the allocator's bug; ignore */

    /* file corruption cases */
    {   /* bad crc */
        mock_reset(); mkgame(300000); good_list(10); M.file[200000] ^= 1;
        rc = bigload_stage(&env, "g", &r); fail_expect(rc, &r, BIGLOAD_E_BAD_CRC, "crc");
        CHECK(live_count() == 0 && r.bytes == 300000);
    }
    {   /* bad magic */
        mock_reset(); mkgame(300000); good_list(10); M.file[0] ^= 1;
        rc = bigload_stage(&env, "g", &r); fail_expect(rc, &r, BIGLOAD_E_BAD_MAGIC, "magic"); CHECK(live_count() == 0);
    }
    {   /* bad abi */
        mock_reset(); mkgame(300000); good_list(10); M.file[4] = 2;
        rc = bigload_stage(&env, "g", &r); fail_expect(rc, &r, BIGLOAD_E_BAD_ABI, "abi"); CHECK(live_count() == 0);
    }
    {   /* header size != file size */
        mock_reset(); mkgame(300000); good_list(10); ((uint32_t *)M.file)[3] = 299996;
        rc = bigload_stage(&env, "g", &r); fail_expect(rc, &r, BIGLOAD_E_BAD_HEADER, "hsize"); CHECK(live_count() == 0);
    }
    {   /* load outside game region */
        mock_reset(); mkgame(300000); good_list(10); ((uint32_t *)M.file)[2] = 0x0c000000;
        rc = bigload_stage(&env, "g", &r); fail_expect(rc, &r, BIGLOAD_E_BAD_HEADER, "load"); CHECK(live_count() == 0);
    }
    {   /* entry outside image */
        mock_reset(); mkgame(300000); good_list(10); ((uint32_t *)M.file)[5] = 0x0b000000;
        rc = bigload_stage(&env, "g", &r); fail_expect(rc, &r, BIGLOAD_E_BAD_HEADER, "entry"); CHECK(live_count() == 0);
    }
    {   /* file shorter than reported size: size mismatch */
        mock_reset(); mkgame(300000); good_list(10); M.fsize = 300000; M.fread_limit = 299996;
        rc = bigload_stage(&env, "g", &r); fail_expect(rc, &r, BIGLOAD_E_SHORT_READ, "mismatch"); CHECK(live_count() == 0 && r.bytes == 299996);
    }
    {   /* leaked blocks stay allocated and reported while a later stage fails? (freed on failure by design) */
        uint32_t l[] = { 0x09100000u, 0x0b100000u, 0x09200000u, 0x0b300000u, 0x09300000u, 0x09400000u, 0x09500000u };
        mock_reset(); mkgame(300000); set_list(7, l, 0); M.file[100] ^= 1;
        rc = bigload_stage(&env, "g", &r); fail_expect(rc, &r, BIGLOAD_E_BAD_CRC, "crc2");
        CHECK(live_count() == 0 && r.leaks == 2 && r.chunks == 3);
    }
    {   /* argument validation */
        mock_reset(); mkgame(300000);
        CHECK(bigload_stage(0, "g", &r) == -1); CHECK(bigload_stage(&env, 0, &r) == -1);
        CHECK(bigload_stage(&env, "g", 0) == -1);
    }
    (void)all_live_in_game;
    mock_reset();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
