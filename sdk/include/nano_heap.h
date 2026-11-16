#ifndef NANOOS_SDK_HEAP_H
#define NANOOS_SDK_HEAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Query multi-region CPU heap statistics in bytes / block count. */
void nano_heap_stats(uint32_t *used_bytes, uint32_t *free_bytes,
                     uint32_t *largest_free, uint32_t *free_blocks);

#ifdef __cplusplus
}
#endif

#endif /* NANOOS_SDK_HEAP_H */
