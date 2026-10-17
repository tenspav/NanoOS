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
    /* VFPv4-D16 on, as RetailOS does (CPACR CP10/CP11 full access, then
     * FPEXC.EN; docs/research/cpu-deep-dive.md): ViceNano's C++ is built
     * -mfpu=vfpv4-d16 -mfloat-abi=softfp (soft-float calls cost ~100x with
     * the D-cache off). No IRQs, so no VFP context to save. */
    mrc p15, 0, r0, c1, c0, 2
    orr r0, r0, #0x00f00000
    mcr p15, 0, r0, c1, c0, 2
    isb
    mov r0, #0x40000000
    .fpu vfpv4-d16
    vmsr fpexc, r0
    ldr sp, =_stack_top
    /* An asset pack (NPAK v1, asset.h) appended to the image starts at
     * __data_end, or at the next 4 KiB boundary when the loader handed over
     * the kernel file padded to its clusters (NanoEmu's disk overlays are),
     * inside the BSS: move it to __asset_base before the clear. Its size is
     * the end of its last entry (offset + padded size). With no pack, a zero
     * word at __asset_base keeps a stale one from being used. */
    ldr r0, =__data_end
    ldr r1, =__asset_base
    mov r2, #0
    ldr r4, =0x4b41504e             @ "NPAK"
    ldr r3, [r0]
    cmp r3, r4
    beq 6f
    add r0, r0, #0xff0
    add r0, r0, #0xf
    bic r0, r0, #0xff0
    bic r0, r0, #0xf
    ldr r3, [r0]
    cmp r3, r4
    bne 4f
6:  ldr r3, [r0, #8]                @ entry count
    sub r3, r3, #1
    cmp r3, #63
    bhi 4f                          @ none, or more than 64
    add r3, r0, r3, lsl #6
    ldr r2, [r3, #48]               @ last entry: offset
    ldr r3, [r3, #56]               @ + padded size
    add r2, r2, r3
    ldr r3, =__asset_max
    cmp r2, r3
    movhi r2, #0
4:  cmp r2, #0
    streq r2, [r1]
    beq 5f
    mov r5, r0
3:  ldr r3, [r0], #4
    str r3, [r1], #4
    subs r2, r2, #4
    bhi 3b
    mov r3, #0                      @ clear the source magic: a warm reset
    str r3, [r5]                    @ without a pack must not re-copy it
5:  ldr r0, =__bss_start
    ldr r1, =__bss_end
    mov r2, #0
1:  cmp r0, r1
    strlo r2, [r0], #4
    blo 1b
    bl kernel_main
2:  wfi
    b 2b
.size _start, .-_start
