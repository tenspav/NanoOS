ENTRY(app_entry)
MEMORY { GAME (rwx) : ORIGIN = 0x0b000000, LENGTH = 10M }
SECTIONS
{
    . = ORIGIN(GAME);
    .game_header : { KEEP(*(.game_header)) } > GAME
    .text : { *(.text.app_entry) *(.text*) *(.rodata*) } > GAME
    .data : { *(.data*) . = ALIGN(4); } > GAME
    __image_end = .;
    .bss (NOLOAD) : {
        . = ALIGN(64);
        __bss_start = .;
        *(.bss*) *(COMMON)
        . = ALIGN(8);
        __stack_bottom = .;
        . += 65536;
        __stack_top = .;
        __bss_end = .;
    } > GAME
    /DISCARD/ : { *(.comment) *(.note*) *(.ARM.exidx*) *(.ARM.extab*) }
    ASSERT(SIZEOF(.game_header) == 32, "canonical game header must be 32 bytes")
    ASSERT(__bss_end <= ORIGIN(GAME) + LENGTH(GAME), "game region overflow")
}
