#ifndef NS_HFS_H
#define NS_HFS_H
#include <stdint.h>
#include "sftl.h"
enum { NS_NOT_FOUND=-11, NS_NOT_FILE=-12 };
typedef int (*ns_bytes_read)(void *,uint64_t,void *,uint32_t);
struct ns_extent {uint32_t start,count;};
struct ns_fork {uint64_t size;uint32_t blocks,id;uint8_t type;struct ns_extent extent[8];};
struct ns_tree {uint32_t first,last,nodes,size,root;};
struct ns_hfs {
 ns_bytes_read read;void *opaque;uint64_t offset,bytes;
 uint32_t block_size,blocks,shift,mounted,attributes;
 struct ns_fork catalog,overflow;
 struct ns_tree cat_tree,ext_tree;
 unsigned char *cat_node,*ext_node;
};
struct ns_file {uint32_t id,kind;struct ns_fork fork;};
/* Workspace: two separate32768byte node buffers. Read-only HFS+ parser.
 * Initial path API accepts ASCII names and ASCII case folding. Unicode path
 * normalization/hard links/symlinks and filesystem writes are unsupported. */
int ns_hfs_mount(struct ns_hfs *,ns_bytes_read,void *,uint64_t volume_offset,
                 uint64_t volume_bytes,unsigned char *catalog_node,unsigned char *extent_node);
int ns_hfs_open(struct ns_hfs *,const char *absolute_path,struct ns_file *);
int ns_hfs_pread(struct ns_hfs *,const struct ns_file *,uint64_t offset,
                 void *,uint32_t length,uint32_t *done);
/* Resolve an aligned full logical sector wholly owned by this regular file.
 * Checks every allocation-block segment, including overflow extents. Output
 * is a whole-device4096byte LBA, suitable for a fixed-size data-only update.
 * Does not authorize mutation, allocate blocks or update filesystem metadata. */
int ns_hfs_sector(struct ns_hfs *,const struct ns_file *,uint64_t file_offset,uint32_t *lba);
#endif
