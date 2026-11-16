#include "write.h"
static unsigned banks(uint32_t m){unsigned n=0;for(unsigned i=0;i<4;++i)n+=(m>>i)&1u;return n;}
static uint32_t row(uint32_t b,unsigned bank,unsigned page){return (bank&1u)<<19|b<<7|page;}
static int expired(struct ns_writer *w){return w->io.usec&&w->io.usec(w->io.opaque)-w->start>=w->budget;}
static int blank_page(struct ns_writer *w,unsigned bank,unsigned page){
 if(expired(w))return NS_TIMEOUT;
 w->last_ce=bank/2;w->last_row=row(w->block,bank,page);++w->reads;
 int rc=w->io.inspect(w->io.opaque,w->last_ce,w->last_row,w->scratch,w->scratch_record);
 if(rc&&rc!=-2)return NS_IO;
 for(unsigned i=0;i<4096;++i)if(w->scratch[i]!=0xffffffffu)return NS_NOT_BLANK;
 for(unsigned i=0;i<16;++i)if(w->scratch_record[i]!=0xffffffffu)return NS_NOT_BLANK;
 return 0;
}
static int scan_blank(struct ns_writer *w){
 for(unsigned p=0;p<128;++p)for(unsigned b=0;b<4;++b)if(w->mask&(1u<<b)){
  int rc=blank_page(w,b,p);if(rc)return rc;
 }
 return 0;
}
static int live(struct ns_writer *w){
 if(!w||!w->active||w->closed)return NS_NOT_READY;
 if(w->poisoned)return NS_WRITE_POISONED;
 if(!w->map->ready||w->map->read_only||w->map->error||w->map->nupdates!=w->expected_updates||
    w->map->checkpoint_generation!=w->checkpoint_gen||w->map->checkpoint_key!=w->checkpoint_key||
    w->map->bank_mask[w->block]!=w->mask)return NS_NOT_READY;
 return 0;
}
int ns_write_prepare(struct ns_writer *w,struct ns_sftl *m,const uint8_t *states,
 const struct ns_write_io *io,uint32_t *stage,uint32_t *sr,uint32_t *scratch,uint32_t *rr,uint32_t budget){
 if(!w)return NS_ARGUMENT;
 *w=(struct ns_writer){.block=0xffffffffu};
 if(m&&m->read_only)return NS_NOT_READY;
 if(!m||!m->ready||m->read_only||m->error||!states||!io||!io->inspect||!stage||!sr||!scratch||!rr||
    stage==scratch||sr==rr||!budget)return NS_ARGUMENT;
 uintptr_t address[4]={(uintptr_t)stage,(uintptr_t)sr,(uintptr_t)scratch,(uintptr_t)rr};
 const uintptr_t length[4]={16384,64,16384,64};
 for(unsigned i=0;i<4;++i){
  if((address[i]&3u)||address[i]>UINTPTR_MAX-length[i])return NS_ARGUMENT;
  for(unsigned j=0;j<i;++j)if(address[i]<address[j]+length[j]&&address[j]<address[i]+length[i])return NS_ARGUMENT;
 }
 w->map=m;w->io=*io;w->stage=stage;w->stage_record=sr;w->scratch=scratch;w->scratch_record=rr;
 w->budget=budget;w->start=io->usec?io->usec(io->opaque):0;
 for(unsigned b=4;b<m->blocks&&b<2084;++b)if(!ns_sftl_ready_block(m,b,states[b])){w->block=b;break;}
 if(w->block==0xffffffffu)return NS_SPACE;
 w->mask=m->bank_mask[w->block];int rc=scan_blank(w);if(rc)return rc;
 w->gen=m->checkpoint_generation;w->key=m->checkpoint_key;
 if(m->nupdates){struct ns_update u=m->updates[m->nupdates-1];w->gen=u.generation;w->key=u.key;}
 if(w->gen>=0xfffffffeu&&w->key==65535u)return NS_SPACE;
 w->checkpoint_gen=m->checkpoint_generation;w->checkpoint_key=m->checkpoint_key;
 w->expected_updates=m->nupdates;w->active=1;return 0;
}
static void advance(uint32_t *g,uint32_t *k){if(*k==65535u){*k=0;++*g;}else ++*k;}
int ns_write_put(struct ns_writer *w,uint32_t lba,uint32_t epoch,const void *data){
 int rc=live(w);if(rc)return rc;
 if(!data||lba>=w->map->sectors)return NS_ARGUMENT;
 if(!w->io.program)return NS_UNSUPPORTED;
 uint32_t g=w->gen,k=w->key;for(unsigned i=0;i<=w->pending;++i)advance(&g,&k);
 if(g==0xffffffffu||epoch>g)return NS_ARGUMENT;
 if(w->pending==4)return NS_SPACE;
 const unsigned char *s=data;unsigned char *d=(unsigned char *)(w->stage+w->pending*1024u);
 for(unsigned i=0;i<4096;++i)d[i]=s[i];
 uint32_t *r=w->stage_record+w->pending*4u;r[0]=k<<16|1;r[1]=g;r[2]=lba;r[3]=epoch;++w->pending;
 return 0;
}
int ns_write_flush(struct ns_writer *w){
 int rc=live(w);if(rc)return rc;
 if(!w->pending)return 0;
 if(!w->io.program)return NS_UNSUPPORTED;
 if(expired(w))return NS_TIMEOUT;
 unsigned n=banks(w->mask),slots=n*512u-4u;
 if(w->next>=slots||slots-w->next<4||w->map->update_capacity-w->map->nupdates<4)return NS_SPACE;
 unsigned count=w->pending;uint32_t g=w->gen,k=w->key;
 for(unsigned i=0;i<4;++i){advance(&g,&k);if(g==0xffffffffu)return NS_SPACE;
  if(i>=count){for(unsigned j=0;j<1024;++j)w->stage[i*1024+j]=w->stage[(count-1)*1024+j];
   w->stage_record[i*4+2]=w->stage_record[(count-1)*4+2];w->stage_record[i*4+3]=w->stage_record[(count-1)*4+3];}
  w->stage_record[i*4]=k<<16|1;w->stage_record[i*4+1]=g;
 }
 struct ns_address a;uint32_t vba=w->block*2048u+w->next;
 rc=ns_vfl_address(w->map,vba,&a);if(rc)return rc;
 /* Recheck immediately before the irreversible callback. */
 rc=blank_page(w,a.ce*2+a.cau,a.page);if(rc)return rc;
 w->poisoned=1;
 w->map->ready=0;
 rc=w->io.program(w->io.opaque,a.ce,row(w->block,a.ce*2+a.cau,a.page),w->stage,w->stage_record);if(rc)return NS_IO;
 ++w->reads;rc=w->io.inspect(w->io.opaque,a.ce,row(w->block,a.ce*2+a.cau,a.page),w->scratch,w->scratch_record);
 if(rc)return NS_VERIFY;
 for(unsigned i=0;i<4096;++i)if(w->stage[i]!=w->scratch[i])return NS_VERIFY;
 for(unsigned i=0;i<16;++i)if(w->stage_record[i]!=w->scratch_record[i])return NS_VERIFY;
 if(expired(w))return NS_TIMEOUT;
 for(unsigned i=0;i<4;++i){rc=ns_sftl_replay(w->map,vba+i,w->stage_record+i*4);if(rc)return rc;}
 rc=ns_sftl_finish(w->map);if(rc)return rc;
 w->expected_updates=w->map->nupdates;w->next+=4;w->pending=0;w->gen=g;w->key=k;w->poisoned=0;return 0;
}
int ns_write_erase_candidate(struct ns_writer *w){
 int rc=live(w);if(rc)return rc;
 if(!w->io.erase)return NS_UNSUPPORTED;
 if(w->next||w->pending)return NS_NOT_READY;
 rc=scan_blank(w);if(rc)return rc;
 w->poisoned=1;
 w->map->ready=0;
 for(unsigned b=0;b<4;++b)if(w->mask&(1u<<b)){
  if(expired(w))return NS_TIMEOUT;
  if(w->io.erase(w->io.opaque,b/2,row(w->block,b,0)))return NS_IO;
 }
 rc=scan_blank(w);if(rc)return rc;
 w->map->ready=1;w->poisoned=0;return 0;
}
int ns_write_resume_compacted(struct ns_writer *w){
 if(!w||!w->active||w->poisoned||w->pending||w->next<4)return NS_NOT_READY;
 struct ns_sftl *m=w->map;
 if(!m->ready||m->read_only||m->error||!m->nupdates||m->checkpoint_generation!=w->checkpoint_gen||
    m->checkpoint_key!=w->checkpoint_key||m->bank_mask[w->block]!=w->mask)return NS_NOT_READY;
 struct ns_update last=m->updates[m->nupdates-1];
 if(last.generation!=w->gen||last.key!=w->key)return NS_CONFLICT;
 for(unsigned k=0;k<4;++k){uint32_t vba;
  if(ns_sftl_lookup(m,w->stage_record[k*4+2],&vba)||vba!=w->block*2048u+w->next-4+k)return NS_CONFLICT;
 }
 w->expected_updates=m->nupdates;return 0;
}
