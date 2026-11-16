.syntax unified
.cpu cortex-a5
.thumb
.section .game_header,"a",%progbits
.balign 4
/* struct ngame_hdr, canonical gameabi.h: size and CRC patched by mkgame.py. */
.word 0x314d474e, 1, 0x0b000000, 0
.word __bss_end, app_entry, 0, 0
.section .text.app_entry,"ax",%progbits
.global app_entry
.type app_entry,%function
.thumb_func
app_entry:
    push {r4, lr}
    mov r4, sp
    ldr r1, =__stack_top
    mov sp, r1
    bl nano_example_main
    mov sp, r4
    pop {r4, pc}
.size app_entry, .-app_entry
.section .note.GNU-stack,"",%progbits
