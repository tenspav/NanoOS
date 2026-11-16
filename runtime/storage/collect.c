#include "collect.h"
#include "sha256.h"
static int sync_writer(struct ns_writer *w){
 struct ns_sftl *m=w->map;
 if(!m->ready||m->read_only||m->error||m->checkpoint_generation!=w->checkpoint_gen||m->checkpoint_key!=w->checkpoint_key||m->bank_mask[w->block]!=w->mask)return NS_NOT_READY;
 if(w->next)return ns_write_resume_compacted(w);
 uint32_t gen=m->checkpoint_generation,key=m->checkpoint_key;
 if(m->nupdates){gen=m->updates[m->nupdates-1].generation;key=m->updates[m->nupdates-1].key;}
 if(gen!=w->gen||key!=w->key)return NS_CONFLICT;
 w->expected_updates=m->nupdates;return 0;
}
static int later(uint32_t g,uint32_t k,uint32_t og,uint32_t ok){return g>og||(g==og&&k>ok);}
static void digest(const void *data,unsigned char *out){struct sha256 h;sha_init(&h);sha_update(&h,data,16384);sha_final(&h,out);}
static int read_page(struct ns_writer *w,struct ns_collect *g,unsigned ce,uint32_t row,uint32_t *data,uint32_t *r){
 if(w->io.usec&&w->io.usec(w->io.opaque)-g->start>=g->budget)return NS_TIMEOUT;
 ++g->reads;return w->io.inspect(w->io.opaque,ce,row,data,r);
}
static int blank(const uint32_t *data,const uint32_t *r){
 for(unsigned i=0;i<4096;++i)if(data[i]!=0xffffffffu)return 0;
 for(unsigned i=0;i<16;++i)if(r[i]!=0xffffffffu)return 0;
 return 1;
}
static int survivor(struct ns_writer *w,struct ns_collect *g,struct ns_survivor *proof,uint32_t cap,const uint32_t *old){
 struct ns_sftl *m=w->map;uint32_t vba;int rc=ns_sftl_lookup(m,old[2],&vba);if(rc)return rc;
 if(vba/2048u==g->block)return NS_CONFLICT;
 const struct ns_update *u=0;
 for(unsigned i=m->nupdates;i;--i)if(m->updates[i-1].lba==old[2]){u=m->updates+i-1;break;}
 if(!u||u->vba!=vba||!later(u->generation,u->key,old[1],old[0]>>16))return NS_CONFLICT;
 uint32_t page=vba&~3u;unsigned index=0;
 while(index<g->survivors&&proof[index].vba!=page)++index;
 if(index==g->survivors){
  if(index==cap)return NS_SPACE;
  struct ns_address a;rc=ns_vfl_address(m,page,&a);if(rc)return rc;
  rc=read_page(w,g,a.ce,(a.cau<<19)|(a.block<<7)|a.page,w->scratch,w->scratch_record);if(rc)return rc==NS_TIMEOUT?rc:NS_VERIFY;
  proof[index].vba=page;
  for(unsigned k=0;k<16;++k)proof[index].record[k]=w->scratch_record[k];
  digest(w->scratch,proof[index].digest);++g->survivors;
 }
 const uint32_t *r=proof[index].record+(vba&3u)*4u;
 if((r[0]&65535u)!=1||r[1]!=u->generation||(r[0]>>16)!=u->key||r[2]!=old[2]||r[3]!=old[3])return NS_VERIFY;
 return 0;
}
static int scan(struct ns_writer *w,struct ns_collect *g,struct ns_survivor *proof,uint32_t cap,
 uint32_t *data,uint32_t *r,int check_source){
 int hole=0;
 for(unsigned page=0;page<128;++page)for(unsigned bank=0;bank<4;++bank)if(g->mask>>bank&1){
  int rc=read_page(w,g,bank/2,(bank%2)<<19|g->block<<7|page,data,r);
  if(rc&&rc!=-2)return rc==NS_TIMEOUT?rc:NS_IO;
  if(blank(data,r)){hole=1;continue;}
  if(!check_source)return NS_NOT_BLANK;
  if(page==127&&!(g->mask&~((1u<<(bank+1))-1u)))return NS_FORMAT;
  if(rc||hole)return NS_VERIFY;
  for(unsigned k=0;k<4;++k){const uint32_t *old=r+k*4;
   if((old[0]&65535u)!=1||old[1]==0xffffffffu||old[3]>old[1]||old[2]>=w->map->sectors||
      !later(old[1],old[0]>>16,w->checkpoint_gen,w->checkpoint_key))return NS_FORMAT;
   rc=survivor(w,g,proof,cap,old);if(rc)return rc;
  }
  ++g->programmed;
 }
 return 0;
}
int ns_append_collect(struct ns_append *a,struct ns_collect *g,uint32_t block,
 struct ns_survivor *proof,uint32_t cap,uint32_t *source,uint32_t *sr,uint32_t budget){
 if(!a||!a->active||!a->writer||!g||!proof||!cap||cap>2044||!source||!sr||!budget)return NS_ARGUMENT;
 struct ns_writer *w=a->writer;struct ns_sftl *m=w->map;
 if(a->poisoned||w->poisoned)return NS_WRITE_POISONED;
 if(!w->active||w->pending||!m->ready||m->read_only||m->error||m->nupdates!=w->expected_updates||
    m->checkpoint_generation!=w->checkpoint_gen||m->checkpoint_key!=w->checkpoint_key||
    m->bank_mask[w->block]!=w->mask)return NS_NOT_READY;
 if(!w->io.erase)return NS_UNSUPPORTED;
 if(block<4||block>=m->blocks||block>=2084||block==w->block||a->states[block]!=1||a->leases[block]!=10)return NS_ARGUMENT;
 uintptr_t p[8]={(uintptr_t)g,(uintptr_t)proof,(uintptr_t)source,(uintptr_t)sr,
  (uintptr_t)w->stage,(uintptr_t)w->stage_record,(uintptr_t)w->scratch,(uintptr_t)w->scratch_record};
 uintptr_t n[8]={sizeof *g,cap*sizeof *proof,16384,64,16384,64,16384,64};
 for(unsigned i=0;i<8;++i){if((p[i]&3u)||p[i]>UINTPTR_MAX-n[i])return NS_ARGUMENT;
  for(unsigned j=0;j<i;++j)if(p[i]<p[j]+n[j]&&p[j]<p[i]+n[i])return NS_ARGUMENT;
 }
 int rc=ns_sftl_compact(m);if(rc)return rc;
 rc=sync_writer(w);if(rc)return rc;
 rc=ns_sftl_ready_block(m,block,a->states[block]);if(rc)return rc;
 *g=(struct ns_collect){.block=block,.mask=m->bank_mask[block],.start=w->io.usec?w->io.usec(w->io.opaque):0,.budget=budget,.phase=1};
 rc=scan(w,g,proof,cap,source,sr,1);if(rc)return rc;
 if(!g->programmed||!g->survivors)return NS_NOT_READY;
 /* Recheck the still-current map after the read-only proof. */
 rc=sync_writer(w);if(rc)return rc;
 rc=ns_sftl_ready_block(m,block,a->states[block]);if(rc)return rc;
 g->phase=2;g->poisoned=1;a->poisoned=1;m->ready=0;
 for(unsigned bank=0;bank<4;++bank)if(g->mask>>bank&1){
  if(w->io.usec&&w->io.usec(w->io.opaque)-g->start>=budget)return NS_TIMEOUT;
  if(w->io.erase(w->io.opaque,bank/2,(bank%2)<<19|block<<7))return NS_IO;
  ++g->erases;
 }
 g->phase=3;rc=scan(w,g,proof,cap,source,sr,0);if(rc)return rc;
 g->phase=4;
 for(unsigned i=0;i<g->survivors;++i){
  struct ns_address ad;rc=ns_vfl_address(m,proof[i].vba,&ad);if(rc)return rc;
  rc=read_page(w,g,ad.ce,(ad.cau<<19)|(ad.block<<7)|ad.page,source,sr);if(rc)return rc==NS_TIMEOUT?rc:NS_VERIFY;
  for(unsigned k=0;k<16;++k)if(sr[k]!=proof[i].record[k])return NS_VERIFY;
  unsigned char d[32];digest(source,d);for(unsigned k=0;k<32;++k)if(d[k]!=proof[i].digest[k])return NS_VERIFY;
 }
 m->ready=1;a->poisoned=0;g->poisoned=0;g->phase=5;a->leases[block]=1;return 0;
}

int ns_append_evacuate(struct ns_append *a,struct ns_evacuate *e,uint32_t block,
 uint32_t *source,uint32_t *sr,uint32_t budget){
 if(!a||!a->active||!a->writer||!e||!source||!sr||!budget)return NS_ARGUMENT;
 struct ns_writer *w=a->writer;struct ns_sftl *m=w->map;
 if(a->poisoned||w->poisoned)return NS_WRITE_POISONED;
 if(!w->active||w->pending||!m->ready||m->read_only||m->error||m->nupdates!=w->expected_updates||
    m->checkpoint_generation!=w->checkpoint_gen||m->checkpoint_key!=w->checkpoint_key||m->bank_mask[w->block]!=w->mask)return NS_NOT_READY;
 if(block<4||block>=m->blocks||block>=2084||block==w->block||a->states[block]!=1||a->leases[block]!=10)return NS_ARGUMENT;
 uintptr_t p[7]={(uintptr_t)e,(uintptr_t)source,(uintptr_t)sr,(uintptr_t)w->stage,(uintptr_t)w->stage_record,(uintptr_t)w->scratch,(uintptr_t)w->scratch_record};
 uintptr_t size[7]={sizeof *e,16384,64,16384,64,16384,64};
 for(unsigned i=0;i<7;++i){if((p[i]&3u)||p[i]>UINTPTR_MAX-size[i])return NS_ARGUMENT;
  for(unsigned j=0;j<i;++j)if(p[i]<p[j]+size[j]&&p[j]<p[i]+size[i])return NS_ARGUMENT;
 }
 for(unsigned i=0;i<m->nruns;++i){const struct ns_run *r=m->runs+i;
  if(r->vba!=0x7fffffu&&r->vba<block*2048u+2048u&&block*2048u<r->vba+r->count)return NS_CONFLICT;
 }
 struct ns_disk d;int rc=ns_disk_init(&d,m,w->io.inspect,w->io.opaque,w->scratch,w->scratch_record);if(rc)return rc;
 *e=(struct ns_evacuate){.block=block,.start=w->io.usec?w->io.usec(w->io.opaque):0,.budget=budget,.phase=1};
 unsigned mask=m->bank_mask[block],banks=0;for(unsigned i=0;i<4;++i)banks+=(mask>>i)&1u;
 int hole=0;
 for(unsigned page=0;page<128;++page){unsigned slot=0;for(unsigned bank=0;bank<4;++bank)if(mask>>bank&1){
  if(w->io.usec&&w->io.usec(w->io.opaque)-e->start>=budget)return NS_TIMEOUT;
  ++e->reads;++e->scanned;
  rc=w->io.inspect(w->io.opaque,bank/2,(bank%2)<<19|block<<7|page,source,sr);
  if(rc&&rc!=-2)return NS_IO;
  uint32_t vba=block*2048u+page*banks*4u+slot*4u;++slot;
  if(blank(source,sr)){hole=1;continue;}
  if(rc||hole||vba%2048u>=banks*512u-4u)return NS_FORMAT;
  uint32_t lba[4],epoch[4],current[4];unsigned live=0;
  for(unsigned k=0;k<4;++k){const uint32_t *r=sr+k*4;
   if((r[0]&65535u)!=1||r[1]==0xffffffffu||r[3]>r[1]||r[2]>=m->sectors||!later(r[1],r[0]>>16,w->checkpoint_gen,w->checkpoint_key))return NS_FORMAT;
   lba[k]=r[2];for(unsigned j=0;j<k;++j)if(lba[j]==lba[k])return NS_FORMAT;
   rc=ns_sftl_lookup(m,lba[k],current+k);if(rc)return rc;
   live+=current[k]==vba+k;
  }
  if(!live)continue;
  /* Even stale neighbors are loaded through the CURRENT logical map. */
  ns_disk_invalidate(&d);
  for(unsigned k=0;k<4;++k){
   rc=ns_disk_read(&d,lba[k],source+k*1024);if(rc)return rc;
   struct ns_address ad;rc=ns_vfl_address(m,current[k],&ad);if(rc)return rc;
   epoch[k]=d.record[ad.sector*4+3];
  }
  e->reads+=d.reads;d.reads=0;
  if(w->io.usec&&w->io.usec(w->io.opaque)-e->start>=budget)return NS_TIMEOUT;
  rc=ns_append_page(a,lba,epoch,source);if(rc){e->poisoned=a->poisoned;return rc;}
  ++e->copied;e->sectors+=live;ns_disk_invalidate(&d);
 }}
 rc=ns_sftl_compact(m);if(rc)return rc;rc=sync_writer(w);if(rc)return rc;
 rc=ns_sftl_ready_block(m,block,a->states[block]);if(rc)return rc;
 e->phase=2;return 0;
}
