#include "game.h"
#include "gameabi.h"
#include "memmap.h"
#include "mmu.h"
#include "asset.h"
#include "lcd.h"
#include "input.h"
#include "touch.h"
#include "timer.h"
#include "mbx.h"
static const struct ngame_hdr *game;
static uint32_t usec(void) { return USEC; }
static void dcache_clean(void) { dcache_clean_all(); }
static const struct nanoos_api api = {
    NGAME_ABI, NGAME_API_VERSION << 16 | (NANOOS_MMU ? NGAME_API_MMU : 0),
    lcd_back, lcd_present, lcd_service, lcd_ready, lcd_flush, lcd_clear,
    lcd_fill_rows, lcd_text, lcd_center, input_poll, touch_poll,
    (const struct ngame_touch *)&touch, delay_us,
    usec, dcache_clean, input_held, input_service,
    mbx_init, mbx_begin, mbx_submit, mbx_set_background, mbx_render, mbx_wait_done,
};

static int outside(uint32_t a, uint32_t n, uint32_t lo, uint32_t hi) {
    return a + n <= lo || a >= hi;
}

/* Every check is against values the loader wrote under RetailOS, so a
 * stale or partial block (no game.bin, a warm reset) just means no game. */
int game_boot(void) {
    const volatile uint32_t *h = (const volatile uint32_t *)N7_HANDOFF_ADDR;
    if (h[0] != N7_HANDOFF_MAGIC || h[1] != 1 || h[6] != sizeof(struct nboot_game)) return 0;
    const struct nboot_game *b = (const struct nboot_game *)h[5];
    if ((uint32_t)b < MM_STAGE || (uint32_t)b > MM_DRAM_END - sizeof *b ||
        b->magic != NBOOT_MAGIC || b->version != NBOOT_VERSION || b->nchunks > NBOOT_CHUNKS ||
        !b->nchunks || b->chunk[0].len < sizeof(struct ngame_hdr)) return 0;
    uint32_t total = 0;
    for (uint32_t i = 0; i < b->nchunks; ++i) {
        uint32_t a = b->chunk[i].addr, n = b->chunk[i].len;
        if (a < MM_STAGE || a & 3 || n > MM_DRAM_END - a || !outside(a, n, MM_GAME, MM_GAME_END) ||
            (n & 3 && i + 1 < b->nchunks)) return 0;
        total += n;
    }
    const struct ngame_hdr *g = (const struct ngame_hdr *)b->chunk[0].addr;
    uint32_t load = g->load, size = g->size;
    if (g->magic != NGAME_MAGIC || g->abi != NGAME_ABI || size != b->size || size != total || size & 3 ||
        load - MM_GAME >= MM_GAME_END - MM_GAME || load & 3 || size > MM_GAME_END - load ||
        g->bss_end < load + size || g->bss_end > MM_GAME_END ||
        g->entry - load - sizeof *g >= size - sizeof *g) return 0;
    /* no chunk overlaps the image: plain word copies in file order */
    uint32_t *d = (uint32_t *)load;
    for (uint32_t i = 0; i < b->nchunks; ++i) {
        const uint32_t *s = (const uint32_t *)b->chunk[i].addr;
        for (uint32_t n = (b->chunk[i].len + 3) / 4; n--;) *d++ = *s++;
    }
    g = (const struct ngame_hdr *)load;
    if (crc32((const uint8_t *)g + sizeof *g, size - sizeof *g) != g->crc) return 0;
    game = g;
    return 1;
}

void game_run(void) {
    const struct ngame_hdr *g = game;
    if (!g) return;
    for (uint32_t *p = (uint32_t *)(g->load + g->size); p < (uint32_t *)g->bss_end; ++p) *p = 0;
    /* the copied code is still in the D-cache: to DRAM, then fetch fresh */
    dcache_clean_all();
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 0\n mcr p15, 0, %0, c7, c5, 6\n dsb sy\n isb"
                     :: "r"(0) : "memory");
    ((int (*)(const struct nanoos_api *))g->entry)(&api);
}
