#ifndef NS_RECLAIM_H
#define NS_RECLAIM_H
#include "write.h"
/* A sealed single-page block created from a fully blank READY lease in this
 * session. Four distinct LBAs only. This is a caller-owned proof, not a
 * security boundary or permission to reclaim arbitrary populated blocks. */
struct ns_page_proof {uint32_t valid,block,mask,checkpoint_gen,checkpoint_key,record[16];unsigned char digest[32];};
struct ns_reclaim {uint32_t phase,reads,erases,programmed_seen,poisoned,ce,row,start,budget,detail;};
int ns_write_seal_page(const struct ns_writer *,struct ns_page_proof *);
/* Caller first compacts superseded RAM replay entries. Reclaims ONLY a
 * completely known single-page block, after a different known block supplies
 * newer verified replacements for all four LBAs. Validates every old-bank
 * page, erases, verifies full blankness and rechecks the surviving page.
 * Base references, live replay refs, unknown pages, changed checkpoint/masks
 * and replacement corruption refuse before erasing. Any uncertain mutation
 * invalidates the mount; a fresh mount is mandatory after failure. */
int ns_reclaim_page(struct ns_reclaim *,struct ns_sftl *,const uint8_t *states,
 const struct ns_write_io *,const struct ns_page_proof *old,const struct ns_page_proof *replacement,
 uint32_t *scratch,uint32_t *records,uint32_t budget_us);
#endif
