#ifndef NS_FMC_ERASE_H
#define NS_FMC_ERASE_H
#include "fmc_program.h"
struct ns_fmc_erase {uint32_t phase,event,status,elapsed,poisoned,completed,last_ce,last_row;};
/* Caller owns complete-map/unreferenced/blank proof. Fixed N31 packed row,
 * whole block only. Successful operation includes full NAND bring-up before
 * any later read/program; bringup returns NONZERO on success and must preserve
 * passive clock policy. */
int ns_fmc_erase_block(const struct ns_fmc_io *,struct ns_fmc_erase *,uint32_t ce,
 uint32_t block_row,int (*bringup)(void *));
#endif
