#ifndef NANOOS_GAMEABI_H
#define NANOOS_GAMEABI_H
#include <stdint.h>
/* Large-game ABI v1: a game is a separate DRAM image (game.bin, linked at
 * its load address inside [MM_GAME, MM_GAME_END), memmap.h) that NanoOS
 * calls from the launcher and that returns to it. Plain C, shared by the
 * kernel (game.c), NanoOSLoader and the game (game/). docs/memory-map.md
 * has the boot flow. */
#define NGAME_MAGIC 0x314d474eu  /* "NGM1" */
/* NGAME_ABI: the image format and load protocol (header, chunk list,
 * entry convention), checked by NanoOSLoader and game.c; unchanged. */
#define NGAME_ABI 1u
/* The services table version is in api->flags bits 16..23 (0 on a v1
 * kernel); api->abi stays NGAME_ABI, which v1 games test for equality.
 * Version 2 (2026-10-19, Vice City stage 2) appended the v2 block (clock,
 * D-cache clean, input levels, MBX driver); the v1 prefix is unchanged. A
 * game checks NGAME_API_TABLE(api) before it uses the v2 block, so one
 * image runs on every loader and on an older kernel (and says so). */
#define NGAME_API_VERSION 2u
#define NGAME_API_TABLE(api) (((api)->flags >> 16) & 0xffu)
/* The first 32 bytes of game.bin (game/mkgame.py fills size and crc). */
struct ngame_hdr {
    uint32_t magic, abi;
    uint32_t load;       /* link address of byte 0 */
    uint32_t size;       /* image bytes, a multiple of 4 */
    uint32_t bss_end;    /* [load + size, bss_end) is zeroed at each launch */
    uint32_t entry;      /* int entry(const struct nanoos_api *), ARM or Thumb */
    uint32_t crc;        /* CRC-32 of bytes [32, size) */
    uint32_t rsv;
};

/* Route (a), NanoOSLoader: game.bin read in file order into RetailOS heap
 * chunks (VA = PA), described by this block, which the chainloader passes
 * as n7_handoff.initrd_start/initrd_size (probe/chainloader/handoff.h).
 * Every chunk but the last is a multiple of 4 bytes. A kernel FAT32
 * reader (route b) will fill the image itself and skip this block. */
#define NBOOT_MAGIC 0x444c474eu  /* "NGLD" */
#define NBOOT_VERSION 1u
#define NBOOT_CHUNKS 64u
struct nboot_game {
    uint32_t magic, version;
    uint32_t size;       /* bytes in all chunks (= ngame_hdr.size) */
    uint32_t nchunks;
    struct { uint32_t addr, len; } chunk[NBOOT_CHUNKS];
};
/* the chainloader's boot block (handoff.h struct n7_handoff), as read back */
#define N7_HANDOFF_ADDR 0x22009000u
#define N7_HANDOFF_MAGIC 0x4c43374eu  /* "N7CL" */

/* Services the kernel hands to the game: the existing drivers, no copies.
 * v1 and v2 are one table; NGAME_API_TABLE(api) says which blocks are
 * there.
 * Drawing: lcd_back() is the 240x432 ARGB8888 panel buffer, logical (x, y)
 * of the 432x240 landscape screen at [x * 240 + 239 - y]; lcd_present()
 * queues it, lcd_service() pushes in slices (call every loop pass),
 * lcd_ready() says the next frame may be drawn. Input: input_poll() edges
 * (NGAME_HOME/LEFT/RIGHT), touch_poll() then *touch. */
#define NGAME_HOME 1u
#define NGAME_LEFT 2u
#define NGAME_RIGHT 4u
#define NGAME_API_MMU 1u   /* flags: MMU, D-cache on (memmap.h attributes) */
/* flags bits 16..23: the table version (see NGAME_API_TABLE) */
struct ngame_touch { int x, y; unsigned state; };   /* touch.h prefix */
struct nanoos_api {
    uint32_t abi, flags;
    uint32_t *(*lcd_back)(void);
    void (*lcd_present)(void);
    void (*lcd_service)(void);
    int (*lcd_ready)(void);
    void (*lcd_flush)(void);
    void (*lcd_clear)(uint32_t color);
    void (*lcd_fill_rows)(int y0, int y1, uint32_t color);
    void (*lcd_text)(int x, int y, const char *s, uint32_t color);
    void (*lcd_center)(int y, const char *s, uint32_t color);
    unsigned (*input_poll)(void);
    int (*touch_poll)(void);
    const struct ngame_touch *touch;
    void (*delay_us)(uint32_t us);
    /* v2. usec(): the free-running 1 MHz counter (timer.h USEC), wraps
     * every 71.6 min. dcache_clean(): every dirty D-cache line to DRAM
     * (no-op without the MMU), before a non-CPU master reads a cached
     * buffer. input_held(): button levels (NGAME_*); input_service():
     * sample buttons during long work (edges go to the next input_poll).
     * mbx_*: the kernel MBX driver (mbx.h), as ViceNano M8 calls it; every
     * GPU-visible buffer must lie in [MM_GAME_GPU, MM_GAME_GPU_END) or be
     * lcd_back(). */
    uint32_t (*usec)(void);
    void (*dcache_clean)(void);
    unsigned (*input_held)(void);
    void (*input_service)(void);
    int (*mbx_init)(void);
    void (*mbx_begin)(void);
    void (*mbx_submit)(const uint32_t *fifo, uint32_t words);
    void (*mbx_set_background)(uint32_t word);
    int (*mbx_render)(const uint32_t *target, uint32_t w, uint32_t h, uint32_t stride);
    int (*mbx_wait_done)(uint32_t timeout_us);
};
#endif
