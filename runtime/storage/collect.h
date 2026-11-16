#ifndef NS_COLLECT_H
#define NS_COLLECT_H
#include "append.h"
struct ns_survivor {uint32_t vba,record[16];unsigned char digest[32];};
struct ns_evacuate {uint32_t phase,reads,scanned,copied,sectors,poisoned,block,start,budget;};
struct ns_collect {uint32_t phase,reads,erases,programmed,survivors,poisoned,block,mask,start,budget;};
/* Collect only a block allocated by this append session, originally READY,
 * now entirely superseded. Never relocates live data or erases a base/live
 * referenced block. Every populated old sector must have a strictly newer
 * current replacement with matching epoch and exact replay weave. All
 * distinct replacement pages are read/hash-sealed before erase and checked
 * again afterward; every victim page is then verified blank. Success releases
 * its RAM reservation. No checkpoint/state-table flash write is issued.
 * Caller workspaces: survivor capacity up to2044 distinct replacement pages;
 * separate16KiB+64byte source buffers. Writer inspection buffers are reused,
 * but its staged last page/records are preserved for append continuation. */
/* Copy still-current victim data into append space before collection. For
 * partially live pages, read CURRENT versions of every LBA in that old group;
 * this prevents stale neighbors from being resurrected and avoids partial-page
 * padding. Does not erase. On space/time/validation failure, completed copies
 * remain valid and the victim is retained; uncertain program poisons session.
 * Source buffers must be separate from all writer workspaces. */
int ns_append_evacuate(struct ns_append *,struct ns_evacuate *,uint32_t block,
 uint32_t *source,uint32_t *source_record,uint32_t budget_us);
int ns_append_collect(struct ns_append *,struct ns_collect *,uint32_t block,
 struct ns_survivor *,uint32_t survivor_capacity,uint32_t *source,uint32_t *source_record,uint32_t budget_us);
#endif
