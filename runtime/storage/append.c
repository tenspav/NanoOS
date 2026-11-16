#include "append.h"
static int overlaps(uintptr_t a,uintptr_t an,uintptr_t b,uintptr_t bn){
 return a>UINTPTR_MAX-an||b>UINTPTR_MAX-bn||(a<b+bn&&b<a+an);
}
static uint32_t capacity(const struct ns_append *a){
 unsigned banks=0;for(unsigned b=0;b<4;++b)banks+=(a->writer->mask>>b)&1u;
 uint32_t n=banks*128u-1u;return a->page_limit&&a->page_limit<n?a->page_limit:n;
}
int ns_append_open(struct ns_append *a,struct ns_writer *w,struct ns_sftl *m,
 const uint8_t *states,uint8_t *leases,uint32_t bytes,const struct ns_write_io *io,
 uint32_t *stage,uint32_t *sr,uint32_t *scratch,uint32_t *rr,uint32_t budget,uint32_t limit){
 if(!a||!w||!m||!states||!leases||bytes<m->blocks||!io||!io->program||limit>511)return NS_ARGUMENT;
 uintptr_t p[8]={(uintptr_t)a,(uintptr_t)w,(uintptr_t)states,(uintptr_t)leases,
  (uintptr_t)stage,(uintptr_t)sr,(uintptr_t)scratch,(uintptr_t)rr};
 uintptr_t n[8]={sizeof *a,sizeof *w,m->blocks,m->blocks,16384,64,16384,64};
 for(unsigned i=0;i<8;++i)for(unsigned j=0;j<i;++j)if(overlaps(p[i],n[i],p[j],n[j]))return NS_ARGUMENT;
 *a=(struct ns_append){0};
 for(unsigned b=0;b<m->blocks;++b)leases[b]=states[b];
 int rc=ns_write_prepare(w,m,leases,io,stage,sr,scratch,rr,budget);if(rc)return rc;
 leases[w->block]=10; /* Never rediscover a used block after RAM compaction. */
 a->writer=w;a->leases=leases;a->states=states;a->page_limit=limit;a->blocks=1;a->active=1;return 0;
}
int ns_append_page(struct ns_append *a,const uint32_t lba[4],const uint32_t epoch[4],const void *data){
 if(!a||!a->active||!a->writer)return NS_NOT_READY;
 struct ns_writer *w=a->writer;struct ns_sftl *m=w->map;
 if(a->poisoned||w->poisoned)return NS_WRITE_POISONED;
 if(!w->active||w->closed||w->pending||!m->ready||m->read_only||m->error||m->nupdates!=w->expected_updates||
    m->checkpoint_generation!=w->checkpoint_gen||m->checkpoint_key!=w->checkpoint_key||
    m->bank_mask[w->block]!=w->mask)return NS_NOT_READY;
 if(!lba||!epoch||!data)return NS_ARGUMENT;
 uintptr_t p=(uintptr_t)data;
 if(overlaps(p,16384,(uintptr_t)w->stage,16384)||overlaps(p,16384,(uintptr_t)w->stage_record,64)||
    overlaps(p,16384,(uintptr_t)w->scratch,16384)||overlaps(p,16384,(uintptr_t)w->scratch_record,64))return NS_ARGUMENT;
 uint32_t lb[4],ep[4],g=w->gen,k=w->key;
 for(unsigned i=0;i<4;++i){
  if(k==65535u){k=0;++g;}else ++k;
  lb[i]=lba[i];ep[i]=epoch[i];
  if(lb[i]>=m->sectors||g==0xffffffffu||ep[i]>g)return NS_ARGUMENT;
  for(unsigned j=0;j<i;++j)if(lb[j]==lb[i])return NS_ARGUMENT;
 }
 int rc=ns_sftl_compact(m);if(rc){a->poisoned=1;return rc;}
 if(w->next){rc=ns_write_resume_compacted(w);if(rc)return rc;}
 else w->expected_updates=m->nupdates;
 if(m->update_capacity-m->nupdates<4)return NS_SPACE;
 if(w->next/4>=capacity(a)){
  struct ns_writer next;
  rc=ns_write_prepare(&next,m,a->leases,&w->io,w->stage,w->stage_record,w->scratch,w->scratch_record,w->budget);
  if(rc)return rc;
  a->leases[next.block]=10;*w=next;++a->blocks;++a->rotations;
 }
 /* Each call has its own bounded transfer deadline, independent of how long
  * the caller spends between pages. Full blank allocation has its own bound. */
 w->start=w->io.usec?w->io.usec(w->io.opaque):0;
 for(unsigned i=0;i<4;++i){rc=ns_write_put(w,lb[i],ep[i],(const unsigned char *)data+i*4096);if(rc)return rc;}
 rc=ns_write_flush(w);
 if(w->poisoned||!m->ready)a->poisoned=1;
 else if(rc)w->pending=0; /* No callback could have changed the array. */
 if(!rc)++a->pages;
 return rc;
}

int ns_append_rotate(struct ns_append *a){
 if(!a||!a->active||!a->writer)return NS_NOT_READY;
 struct ns_writer *w=a->writer;struct ns_sftl *m=w->map;
 if(a->poisoned||w->poisoned)return NS_WRITE_POISONED;
 if(!w->active||w->pending||!w->next||!m->ready||m->read_only||m->error||m->nupdates!=w->expected_updates||
    m->checkpoint_generation!=w->checkpoint_gen||m->checkpoint_key!=w->checkpoint_key||
    m->bank_mask[w->block]!=w->mask)return NS_NOT_READY;
 int rc=ns_sftl_compact(m);if(rc){a->poisoned=1;return rc;}
 rc=ns_write_resume_compacted(w);if(rc)return rc;
 struct ns_writer next;
 rc=ns_write_prepare(&next,m,a->leases,&w->io,w->stage,w->stage_record,w->scratch,w->scratch_record,w->budget);
 if(rc)return rc;
 a->leases[next.block]=10;*w=next;++a->blocks;++a->rotations;return 0;
}
