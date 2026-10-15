#ifndef NANOOS_MEM_H
#define NANOOS_MEM_H
#include <stdint.h>
/* Bump allocator over [__heap_start, __heap_end) (kernel.ld); no free. */
void *kalloc(uint32_t size);
uint32_t heap_used(void);
uint32_t heap_total(void);
#endif
