#ifndef NS_WRITE_H
#define NS_WRITE_H
#include "mount.h"
enum { NS_NOT_BLANK=-13,NS_WRITE_POISONED=-14,NS_VERIFY=-15 };
/* Program/erase callbacks must provide bounded controller completion. They
 * are optional for read-only preflight. This file never accesses MMIO. */
struct ns_write_io {
 void *opaque;ns_page_read inspect;
 int (*program)(void *,uint32_t ce,uint32_t row,const void *,const uint32_t *);
 int (*erase)(void *,uint32_t ce,uint32_t block_row);
 uint32_t (*usec)(void *);
};
struct ns_writer {
 struct ns_sftl *map;struct ns_write_io io;
 uint32_t *stage,*stage_record,*scratch,*scratch_record;
 uint32_t block,mask,next,pending,gen,key,expected_updates,checkpoint_gen,checkpoint_key;
 uint32_t active,poisoned,reads,start,budget,last_ce,last_row,closed;
};
/* Select a current READY block >=4, reject every live tree/replay reference,
 * and inspect ALL128pages of each usable bank. Errored bytes qualify as
 * blank only under the transport-complete inspection contract and all-FF
 * data+records. No program/erase is issued by prepare. Buffers are separate
 *16KiB/64byte stage and16KiB/64byte inspection workspaces. */
int ns_write_prepare(struct ns_writer *,struct ns_sftl *,const uint8_t *states,
 const struct ns_write_io *,uint32_t *stage,uint32_t *stage_record,
 uint32_t *scratch,uint32_t *scratch_record,uint32_t budget_us);
/* Put/flush buffer up to four4096byte logical sectors. Caller supplies the
 * preserved record epoch (gen2). Short flush pads by repeating the last
 * sector with later keys; native stock acceptance remains unverified. */
int ns_write_put(struct ns_writer *,uint32_t lba,uint32_t epoch,const void *);
int ns_write_flush(struct ns_writer *);
/* Rebind after a RAM-only replay compaction when no other writer advanced
 * the weave or changed this writer's last full distinct-LBA page. */
int ns_write_resume_compacted(struct ns_writer *);
/* Optional erase of the already blank-verified, unreferenced READY target;
 * only before any pending/programmed page. Rechecks all banks afterward.
 * Any uncertain physical mutation poisons the session until fresh mount. */
int ns_write_erase_candidate(struct ns_writer *);
#endif
