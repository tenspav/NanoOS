.syntax unified
.cpu cortex-a5
.arm
.section .text.entry,"ax"
.global _start
.type _start,%function
_start:
    cpsid aif
    /* Instruction cache and branch prediction on (MMU and data cache stay
     * off): with SCTLR.I clear every fetch went to uncached DRAM. Invalidate
     * first, the chainloader may leave stale lines. */
    mov r0, #0
    mcr p15, 0, r0, c7, c5, 0       @ ICIALLU
    mcr p15, 0, r0, c7, c5, 6       @ BPIALL
    dsb
    isb
    mrc p15, 0, r0, c1, c0, 0
    orr r0, r0, #(1 << 12)          @ SCTLR.I
    orr r0, r0, #(1 << 11)          @ SCTLR.Z
    mcr p15, 0, r0, c1, c0, 0
    isb
    ldr sp, =_stack_top
    ldr r0, =__bss_start
    ldr r1, =__bss_end
    mov r2, #0
1:  cmp r0, r1
    strlo r2, [r0], #4
    blo 1b
    bl kernel_main
2:  wfi
    b 2b
.size _start, .-_start
