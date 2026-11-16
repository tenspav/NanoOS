#ifndef NS_SAVE_H
#define NS_SAVE_H
#include "hfs.h"
#include "disk.h"
#include "collect.h"
#define NS_SAVE_EMPTY 1 /* open succeeded; prepared file has no save yet */
#define NS_SAVE_FILE_BYTES 32768u
#define NS_SAVE_MAX_BYTES 16320u
/* Preallocated, fixed-size two-slot save. No HFS metadata or checkpoint writes.
 * A game owns one32KiB regular file; both slots are complete logical16KiB pages.
 * Retain this object/workspace and the mounted filesystem/session exclusively.
 * load returns NS_NOT_FOUND for a wholly blank new file. Bad newest envelopes
 * fall back to the other valid slot. One slot I/O error also permits a
 * read-only fallback; two unreadable/invalid slots fail without payload output.
 * This envelope cannot repair a NAND mount that fails before file access. */
struct ns_save {
 struct ns_hfs *fs;struct ns_disk *disk;struct ns_append *append;
 struct ns_file file;uint32_t lba[8],epoch[8],game,schema;
 uint32_t *slots;uint32_t active,length,recovered,opened,faulted,epoch_missing;
 uint64_t sequence;
};
/* slots is a separate aligned32KiB workspace, not any disk/writer buffer.
 * The caller explicitly authorizes mutation of this one existing file.
 * The append session need not be opened until the first store. */
int ns_save_open(struct ns_save *,struct ns_hfs *,struct ns_disk *,struct ns_append *,
 const char *path,uint32_t game,uint32_t schema,uint32_t *slots);
int ns_save_load(struct ns_save *,void *out,uint32_t capacity,uint32_t *length);
/* Complete replacement,0..NS_SAVE_MAX_BYTES; sequence advances only after
 * verified append and fresh logical readback. Failure after a program requires
 * fresh mount and leaves the prior slot physically untouched. */
int ns_save_store(struct ns_save *,const void *payload,uint32_t length);
/* Optional maintenance: collect fully superseded blocks owned by this session.
 * Requires an erase callback. Never adopts/erases a pre-existing block.
 * At most eight surviving pages for this one two-slot save file; sessions
 * shared with other writers may return NS_SPACE and retain the victim. */
int ns_save_reclaim(struct ns_save *,uint32_t budget_us,uint32_t *collected);
#endif
