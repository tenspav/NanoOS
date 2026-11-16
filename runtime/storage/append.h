#ifndef NS_APPEND_H
#define NS_APPEND_H
#include "write.h"
/* Full-page append session. Block reservations are RAM-only and last until
 * close/remount; no block erase, checkpoint creation or general GC here.
 * All committed data retains ordinary replayable sector records. */
struct ns_append {
 struct ns_writer *writer;
 uint8_t *leases;
 const uint8_t *states;
 uint32_t page_limit,blocks,pages,rotations,poisoned,active;
};
/* leases has at least map->blocks bytes, is separate from the source state
 * table/workspaces, and stays alive with the session. page_limit=0 uses the
 * natural usable-bank capacity minus the reserved final page. A smaller
 * limit permits bounded tests and deliberate early block rotation. */
int ns_append_open(struct ns_append *,struct ns_writer *,struct ns_sftl *,
 const uint8_t *states,uint8_t *leases,uint32_t lease_bytes,
 const struct ns_write_io *,uint32_t *stage,uint32_t *stage_record,
 uint32_t *scratch,uint32_t *scratch_record,uint32_t budget_us,uint32_t page_limit);
/* Deliberately rotate early; committed pages remain replayable. A failed
 * allocation leaves the current writer and committed map intact. */
int ns_append_rotate(struct ns_append *);
/* Four distinct LBAs and their preserved epochs; data is exactly16KiB.
 * Compacts superseded RAM replay entries before reserving four update slots.
 * Space/validation failures before a mutation leave committed reads usable.
 * Uncertain mutation invalidates the map and poisons the session. */
int ns_append_page(struct ns_append *,const uint32_t lba[4],const uint32_t epoch[4],const void *data);
#endif
