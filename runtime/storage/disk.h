#ifndef NS_DISK_H
#define NS_DISK_H
#include "sftl.h"
typedef int (*ns_page_read)(void *opaque,uint32_t ce,uint32_t row,
                            void *data16384,uint32_t record16[16]);
#define NS_DISK_CACHE_MAX 16u
struct ns_disk_slot {
    uint32_t *data, *record;
    uint32_t ce, row, valid, stamp;
};
struct ns_disk {
    const struct ns_sftl *map;
    ns_page_read read_page;
    void *opaque;
    uint32_t *data, *record;
    uint32_t ce, row, valid, reads, hits;
    uint32_t cap, clock;
    struct ns_disk_slot slots[NS_DISK_CACHE_MAX];
};
/* Workspace is caller-owned: data is 16 KiB, records are 64 bytes. */
int ns_disk_init(struct ns_disk *,const struct ns_sftl *,ns_page_read,void *,
                 uint32_t *data16384,uint32_t *record16);
int ns_disk_set_cache(struct ns_disk *,uint32_t *data_pages,uint32_t *record_pages,uint32_t pages);
void ns_disk_invalidate(struct ns_disk *);
/* One 4096-byte sector: caller output is unchanged on failure. */
int ns_disk_read(struct ns_disk *,uint32_t lba,void *out4096);
/* Byte reads may return a valid prefix. done is authoritative on all errors;
 * EOF is a short successful read, and bytes outside the prefix are untouched. */
int ns_disk_pread(struct ns_disk *,uint64_t offset,void *,uint32_t length,
                  uint32_t *done);
#endif
