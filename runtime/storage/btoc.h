#ifndef NS_BTOC_H
#define NS_BTOC_H
#include "write.h"
struct ns_btoc_entry {uint32_t delta,epoch,lba,count;};
struct ns_btoc {struct ns_btoc_entry *entry;uint32_t capacity,n,sectors,role,first_gen,first_key,last_gen,last_key,span;};
struct ns_close {uint32_t phase,reads,programs,entries,data_sectors,tail_sectors,span,poisoned,statistics[4];};
int ns_btoc_init(struct ns_btoc *,struct ns_btoc_entry *,uint32_t capacity);
/* One physical data record, in append order. Weave gaps are preserved;
 * runs merge only with contiguous weave, epoch and logical sector. */
int ns_btoc_add(struct ns_btoc *,const uint32_t record[4]);
/* tail_sectors is4or8, always complete16KiB page(s). span=0 derives last+1;
 * a larger explicit span can preserve a stock trailing weave gap. */
int ns_btoc_encode(const struct ns_btoc *,uint32_t tail_sectors,uint32_t span,
 uint32_t *data,uint32_t *records);
/* Compare meaningful entries/terminator and EVERY summary record. Padding
 * after the terminator is not interpreted. */
int ns_btoc_match(const struct ns_btoc *,uint32_t tail_sectors,uint32_t span,
 const uint32_t *data,const uint32_t *records);
void ns_btoc_statistics(const struct ns_btoc *,uint32_t out[4]);
/* Close only a full caller-owned ns_writer prefix. Caller chooses4or8tail
 * sectors BEFORE filling it. Source must occupy exactly all preceding slots;
 * no gaps/padding data are invented. Separate32KiB data/128byte record and
 *2048entry workspace; writer staging remains intact. No logical weave/map
 * changes. Any uncertain tail mutation poisons writer/map. */
int ns_write_close(struct ns_writer *,struct ns_close *,uint32_t tail_sectors,
 struct ns_btoc_entry *,uint32_t entry_capacity,uint32_t *summary_data,
 uint32_t *summary_records,uint32_t budget_us);
#endif
