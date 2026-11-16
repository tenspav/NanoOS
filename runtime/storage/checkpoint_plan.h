#ifndef NS_CHECKPOINT_PLAN_H
#define NS_CHECKPOINT_PLAN_H
#include "sftl.h"
struct ns_cp_closed {uint32_t block,tail,role,statistics[4];};
struct ns_cp_plan {uint32_t phase,runs,nodes,checks,changed_blocks,first_gen,first_key,last_gen,last_key,refs[2084];};
/* RAM-ONLY FORMAT PLANNER. Does NOT allocate new checkpoint roots, reserve
 * its simulated weaves, call program/erase or publish metadata. Original
 * root IDs remain placeholders. NEVER program this buffer directly.
 * Builds full current mapping and live counts, preserving opaque template
 * fields/statistics. Only explicitly verified closed session blocks may
 * transition READY->closed; caller supplies tail size/entry statistics.
 * Source CP must exactly match map's base runs and checkpoint weave.
 * out196608,rec768,run8192andupdate workspaces are caller-owned/separate.
 */
int ns_checkpoint_plan(struct ns_cp_plan *,const struct ns_sftl *,const void *template196608,const uint32_t *template_records,
 const struct ns_cp_closed *,uint32_t nclosed,uint32_t *out,uint32_t *records,
 struct ns_run *runs,uint32_t run_capacity,struct ns_update *updates,uint32_t update_capacity);
#endif
