#ifndef NANOOS_MEMMAP_H
#define NANOOS_MEMMAP_H
/* The 64 MB DRAM map (docs/memory-map.md has the why of each region).
 * The MMU (mmu.c) maps everything 1:1 in 1 MB sections: DRAM Normal
 * write-back, except the sections in MM_UNCACHED (Strongly-ordered, as
 * all memory was with the MMU off), which non-CPU masters read or write.
 * Shared with kernel.ld (by value), NanoOSLoader and the game (game/). */
#define MM_DRAM          0x08000000u
#define MM_DRAM_END      0x0c000000u
/* kernel text+data (<= 64 KB), .bss, stack, MMU table, kalloc heap */
#define MM_KERNEL        0x08000000u
/* LCD scan-out BUF0/BUF1 (lcd.c, display DMA) */
#define MM_LCD           0x08200000u
/* mbx_test render target + trace ring (GPU writes) */
#define MM_GPUTEST       0x08300000u
/* NPAK asset pack (startup.s, asset.c; CPU only) */
#define MM_ASSET         0x08400000u
/* kernel .dma (kernel.ld): MBX tables/PB/parameter pool, ViceNano target
 * and texels, audio DMA passes; 3 MB, all inside the GPU's 32 MB */
#define MM_DMA           0x08500000u
#define MM_DMA_END       0x08800000u
/* RetailOS leftovers NanoOS still reads: bt.c's patchram blob
 * 0x0890c884..0x08914203. Nothing may be loaded here. */
#define MM_RETAIL_KEEP   0x08800000u
/* game GPU arena: textures, targets, MBX pools of the game (GPU VA
 * 0x00a00000..0x02000000 = MBX_VA). Uncached for now. */
#define MM_GAME_GPU      0x08a00000u
#define MM_GAME_GPU_END  0x0a000000u
/* game heap / streaming arena (CPU only). Holds the report mailbox and
 * mirrors (report.h): those are only live from a report write to the
 * next reset, and the game owns the RAM while it runs. */
#define MM_GAME_HEAP     0x0a000000u
#define MM_GAME_HEAP_END 0x0b000000u
/* game image: code, data, bss, stack (game.h header load..bss_end) */
#define MM_GAME          0x0b000000u
#define MM_GAME_END      0x0ba00000u
/* second CPU-only heap, holds two report mirrors (0x0ba00000, 0x0be60000) */
#define MM_GAME_HEAP2    0x0ba00000u
/* NanoOSLoader's game.bin chunks (RetailOS heap, VA = PA) must lie in
 * [MM_STAGE, MM_DRAM_END) outside [MM_GAME, MM_GAME_END): game.c copies
 * them to the image before anything else writes there. */
#define MM_STAGE         0x08800000u
/* bit s: section MM_DRAM + s MB is uncached: LCD, GPU test, kernel .dma,
 * game GPU arena (sections 32..63 are all cached) */
#define MM_UNCACHED      0xfffffcecu
/* a zero-initialised buffer that a DMA master or the GPU uses */
#define MM_DMA_BUF __attribute__((section(".bss.dma")))
#endif
