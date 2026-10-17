#include "mem.h"
extern uint8_t __heap_start[], __heap_end[];
static uint32_t used;
void *kalloc(uint32_t size) {
    size = (size + 7) & ~7u;
    if (size > heap_total() - used) return 0;
    used += size;
    return __heap_start + used - size;
}
uint32_t heap_used(void) { return used; }
uint32_t heap_total(void) { return (uint32_t)(__heap_end - __heap_start); }
