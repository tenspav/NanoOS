#include <stdint.h>
#include "mmu.h"
__attribute__((noreturn)) void reset_stockos(void) {
    /* the RAM reports (report.h) must reach DRAM before the reset */
    __asm__ volatile("cpsid aif" ::: "memory");
    dcache_clean_all();
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)0x3c800000u = 0x00104a00u;
    *(volatile uint32_t *)0x3c500050u = 0xa5;
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)0x3c800000u = 0xa5;
    for (;;) __asm__ volatile("wfi");
}
