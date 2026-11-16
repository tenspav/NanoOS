#ifndef NS_SFTL_H
#define NS_SFTL_H
#include <stdint.h>
/* Portable mapping primitives. No MMIO, allocation, or implicit device writes.
 * Mount code must supply a complete checkpoint plus its complete replay tail.
 * Calling these primitives alone does not constitute a recovered mount. */
enum { NS_OK=0, NS_ARGUMENT=-1, NS_FORMAT=-2, NS_SPACE=-3,
       NS_CONFLICT=-4, NS_NOT_READY=-5, NS_UNMAPPED=-6, NS_UNSUPPORTED=-10 };
struct ns_run { uint32_t lba, vba, count; };
struct ns_update { uint32_t lba, vba, generation, key; };
struct ns_address { uint32_t ce, cau, block, page, sector; };
struct ns_sftl {
    uint32_t blocks, sectors, nruns, nupdates, run_capacity, update_capacity;
    uint32_t checkpoint_generation, checkpoint_key, ready;
    int error;
    const uint8_t *bank_mask; /* bit 2*ce+cau; zero or unknown is unusable */
    struct ns_run *runs;
    struct ns_update *updates;
    uint32_t read_only; /* recovery quarantine: reads valid, all mutation planning refused */
};
int ns_sftl_init(struct ns_sftl *, uint32_t blocks, uint32_t sectors,
                const uint8_t *bank_mask, struct ns_run *, uint32_t run_capacity,
                struct ns_update *, uint32_t update_capacity,
                uint32_t checkpoint_generation, uint32_t checkpoint_key);
int ns_vfl_address(const struct ns_sftl *, uint32_t vba, struct ns_address *);
int ns_sftl_node(struct ns_sftl *, const void *node4096);
int ns_sftl_replay(struct ns_sftl *, uint32_t vba, const uint32_t record[4]);
/* Verifies complete, gapless logical coverage and rejects ambiguous weaves.
 * On any error, lookup remains unavailable. */
int ns_sftl_finish(struct ns_sftl *);
/* Drop superseded RAM replay entries, preserving every newest lookup and
 * the global latest weave. Does not change flash/checkpoint or base runs. */
int ns_sftl_compact(struct ns_sftl *);
/* Materialize complete current mapping into coalesced runs using caller
 * work storage. Source map is never changed. The count is valid only on success;
 * partial output/work are scratch and must be discarded on failure. */
int ns_sftl_snapshot(const struct ns_sftl *,struct ns_run *,uint32_t capacity,
 struct ns_update *work,uint32_t work_capacity,uint32_t *count);
int ns_sftl_lookup(const struct ns_sftl *, uint32_t lba, uint32_t *vba);
/* Conservative write-planning guard: READY state, usable banks, no checkpoint
 * or replay references. This only permits consideration; it never erases. */
int ns_sftl_ready_block(const struct ns_sftl *, uint32_t block, uint32_t state);
#endif
