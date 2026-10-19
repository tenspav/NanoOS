/*
 * kernel.ld -- Linker script for NanoOS at physical 0x08000000
 *
 * Cortex-A5, DRAM base 0x08000000, MMU flat 1:1 (src/mmu.c). The whole
 * 64 MB map: src/memmap.h, docs/memory-map.md.
 * Freestanding, no libc, no dynamic allocation.
 */

OUTPUT_FORMAT("elf32-littlearm", "elf32-littlearm", "elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY
{
    DRAM (rwx) : ORIGIN = 0x08000000, LENGTH = 2M
}

SECTIONS
{
    . = 0x08000000;

    .text : {
        __text_start = .;
        *(.text.entry)
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
        __text_end = .;
    } > DRAM

    .data : {
        __data_start = .;
        *(.data*)
        . = ALIGN(4);
        __data_end = .;
    } > DRAM

    /* data from 0x08010000 on: NanoEmu's real iPod mode charges uncached
     * DRAM time there, while code below runs at full speed */
    ASSERT(. <= 0x08010000, "code/data past 0x08010000")
    /* Buffers a DMA master or the GPU uses (MM_DMA_BUF, src/memmap.h):
     * their own uncached sections MM_DMA..MM_DMA_END, so the rest of the
     * kernel's 2 MB can be cached (src/mmu.c). Listed before .bss so that
     * .bss.dma lands here; startup.s zeroes it like .bss. */
    .dma 0x08500000 (NOLOAD) : {
        __dma_start = .;
        *(.bss.dma*)
        . = ALIGN(4);
        __dma_end = .;
    }
    ASSERT(__dma_end <= 0x08800000, "kernel .dma past MM_DMA_END")
    .bss 0x08010000 (NOLOAD) : {
        . = ALIGN(4);
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > DRAM

    .stack (NOLOAD) : {
        . = ALIGN(8);
        . += 0x4000; /* 16 KiB DRAM stack */
        _stack_top = .;
    } > DRAM

    ASSERT(_stack_top <= 0x08200000, "kernel/stack overlaps framebuffer")

    /* kalloc() heap: the rest of DRAM up to the panel framebuffer (CPU
     * only, cached: DMA buffers belong in .dma) */
    __heap_start = ALIGN(_stack_top, 8);
    __heap_end = 0x08200000;

    /* Asset pack (NPAK v1, src/asset.h) appended to kernel.bin by the loader:
     * startup.s moves it here before the BSS clear. Free DRAM above the LCD
     * buffers (end 0x082e5400) and mbx_test's target/trace (0x08300000..
     * 0x08365400), inside the GPU's 32 MB. */
    __asset_base = 0x08400000;
    __asset_max = 0x00100000;

    /DISCARD/ : {
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.note*)
        *(.comment)
    }
}
