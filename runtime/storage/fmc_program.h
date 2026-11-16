#ifndef NS_FMC_PROGRAM_H
#define NS_FMC_PROGRAM_H
#include "sftl.h"
struct ns_fmc_io {
 void *opaque;
 uint32_t (*read)(void *,uint32_t offset);
 void (*write)(void *,uint32_t offset,uint32_t value);
 uint32_t (*usec)(void *);
 void (*clean)(void *);
 uint32_t (*bus)(void *,const void *,uint32_t bytes);
};
struct ns_fmc_program {
 uint32_t rows[16] __attribute__((aligned(64)));
 uint32_t counts[16] __attribute__((aligned(64)));
 uint32_t ring[16] __attribute__((aligned(64)));
 uint32_t failures[16] __attribute__((aligned(64)));
 uint32_t irq,verdict,pc,phase,elapsed,poisoned;
};
/* Low-level full-page controller backend. Caller MUST establish exclusive,
 * current READY ownership and blank proof via ns_writer; this is not a raw
 * arbitrary-address public app service. Any failure poisons this controller
 * context and requires a full bring-up/fresh mount before any later mutation.
 * No erase operation is implemented here. */
int ns_fmc_program_page(const struct ns_fmc_io *,struct ns_fmc_program *,
 uint32_t ce,uint32_t row,const void *data16384,const uint32_t records16[16]);
#endif
