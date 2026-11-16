#ifndef NS_PARTITION_H
#define NS_PARTITION_H
#include "hfs.h"
/* Discover the unique HFS partition from an Apple partition map or MBR.
 * Offsets are bytes; read callback is relative to its whole backing device. */
int ns_partition_hfs(ns_bytes_read,void *,uint64_t disk_offset,uint64_t disk_bytes,
                      uint64_t *volume_offset,uint64_t *volume_bytes);
#endif
