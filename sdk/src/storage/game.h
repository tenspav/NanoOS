#ifndef NS_GAME_H
#define NS_GAME_H
#include "save.h"
#include "partition.h"
#define NS_GAME_RUNS 8192u
#define NS_GAME_UPDATES 65536u
/* One exclusively owned mounted volume per instance. Put in static/heap DRAM,
 * not the small kernel stack. All operations are synchronous and single-threaded.
 * Assets are ASCII HFS+ paths; saves must be explicitly preallocated32KiB files.
 * No implicit format, filesystem metadata writes, checkpoint publication or
 * pre-existing-block erase. Session-only garbage collection is opt-in via
 * reclaim_enabled; leave it zero for ordinary games until interrupted-erase
 * recovery is implemented. Recovered mounts always refuse writes.
 * The current N31 mount profile uses a0xc040logical-sector exported-disk prefix. */
struct ns_game {
 struct ns_mount mount;struct ns_disk disk;struct ns_hfs fs;
 struct ns_writer writer;struct ns_append append;struct ns_save save;
 struct ns_write_io io;struct ns_run runs[NS_GAME_RUNS];struct ns_update updates[NS_GAME_UPDATES];
 uint8_t mask[2088],states[2088],leases[2088];uint32_t fg[2088],fk[2088];
 uint32_t page[4096],record[16],cache[16*4096],cache_record[16*16];
 uint32_t stage[4096],stage_record[16],scratch[4096],scratch_record[16],slots[8192];
 unsigned char cat[32768],ext[32768];
 uint32_t mounted,budget_us,reclaimed,reclaim_enabled;int strict_mount_rc;
};
int ns_game_mount(struct ns_game *,const struct ns_mount_io *,const struct ns_write_io *,uint32_t budget_us);
/* Files remain valid until this volume is remounted; pread has ordinary short
 * EOF semantics, with done authoritative on an error. */
int ns_game_open(struct ns_game *,const char *absolute_path,struct ns_file *);
int ns_game_read(struct ns_game *,const struct ns_file *,uint64_t offset,void *,uint32_t length,uint32_t *done);
/* One save file per instance, explicitly selected by game ID and schema.
 * NS_SAVE_EMPTY means a blank prepared file with no save yet. NS_NOT_FOUND
 * means the file does not exist. No new allocation is performed by save_open. */
int ns_game_save_open(struct ns_game *,const char *path,uint32_t game_id,uint32_t schema);
int ns_game_save_load(struct ns_game *,void *,uint32_t capacity,uint32_t *length);
int ns_game_save_store(struct ns_game *,const void *,uint32_t length);
#endif
