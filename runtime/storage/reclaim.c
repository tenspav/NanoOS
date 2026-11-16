#include "reclaim.h"
#include "sha256.h"
#define PROOF 0x53504331u
static int later(const uint32_t *a,const uint32_t *b){return a[1]>b[1]||(a[1]==b[1]&&(a[0]>>16)>(b[0]>>16));}
static int match(const struct ns_page_proof *p,const uint32_t *data,const uint32_t *r,uint32_t *detail){
 *detail=0;
 for(unsigned i=0;i<16;++i)if(p->record[i]!=r[i]){*detail=1+i;return 0;}
 struct sha256 h;unsigned char d[32];sha_init(&h);sha_update(&h,data,16384);sha_final(&h,d);
 for(unsigned i=0;i<32;++i)if(d[i]!=p->digest[i]){*detail=0x100u+i;return 0;}
 return 1;
}
int ns_write_seal_page(const struct ns_writer *w,struct ns_page_proof *p){
 if(!p)return NS_ARGUMENT;
 p->valid=0;
 if(!w||!w->active||w->poisoned||w->pending||w->next!=4||!w->map->ready||
    w->expected_updates!=w->map->nupdates)return NS_NOT_READY;
 for(unsigned k=0;k<4;++k){uint32_t vba;const uint32_t *r=w->stage_record+k*4;
  if((r[0]&65535u)!=1||ns_sftl_lookup(w->map,r[2],&vba)||vba!=w->block*2048u+k)return NS_CONFLICT;
  for(unsigned j=0;j<k;++j)if(w->stage_record[j*4+2]==r[2])return NS_CONFLICT;
 }
 p->block=w->block;p->mask=w->mask;p->checkpoint_gen=w->checkpoint_gen;p->checkpoint_key=w->checkpoint_key;
 for(unsigned i=0;i<16;++i)p->record[i]=w->stage_record[i];
 struct sha256 h;sha_init(&h);sha_update(&h,w->stage,16384);sha_final(&h,p->digest);p->valid=PROOF;return 0;
}
static int read_page(struct ns_reclaim *g,const struct ns_write_io *io,unsigned ce,uint32_t row,uint32_t *data,uint32_t *r){
 if(io->usec&&io->usec(io->opaque)-g->start>=g->budget)return NS_TIMEOUT;
 g->ce=ce;g->row=row;++g->reads;return io->inspect(io->opaque,ce,row,data,r);
}
static int verify(struct ns_reclaim *g,const struct ns_sftl *m,const struct ns_write_io *io,
                  const struct ns_page_proof *p,uint32_t *data,uint32_t *r){
 struct ns_address a;int rc=ns_vfl_address(m,p->block*2048u,&a);if(rc)return rc;
 rc=read_page(g,io,a.ce,(a.cau<<19)|(a.block<<7)|a.page,data,r);
 if(rc)return rc==NS_TIMEOUT?rc:NS_VERIFY;
 return match(p,data,r,&g->detail)?0:NS_VERIFY;
}
static int scan(struct ns_reclaim *g,const struct ns_sftl *m,const struct ns_write_io *io,
                const struct ns_page_proof *p,int programmed,uint32_t *data,uint32_t *r){
 struct ns_address a;int rc=ns_vfl_address(m,p->block*2048u,&a);if(rc)return rc;
 unsigned first=a.ce*2+a.cau;
 for(unsigned page=0;page<128;++page)for(unsigned bank=0;bank<4;++bank)if(p->mask&(1u<<bank)){
  rc=read_page(g,io,bank/2,(bank%2)<<19|p->block<<7|page,data,r);
  if(programmed&&page==0&&bank==first){if(rc)return rc==NS_TIMEOUT?rc:NS_VERIFY;if(!match(p,data,r,&g->detail))return NS_VERIFY;++g->programmed_seen;continue;}
  if(rc&&rc!=-2)return rc==NS_TIMEOUT?rc:NS_IO;
  for(unsigned i=0;i<4096;++i)if(data[i]!=0xffffffffu)return NS_NOT_BLANK;
  for(unsigned i=0;i<16;++i)if(r[i]!=0xffffffffu)return NS_NOT_BLANK;
 }
 return 0;
}
int ns_reclaim_page(struct ns_reclaim *g,struct ns_sftl *m,const uint8_t *states,const struct ns_write_io *io,
 const struct ns_page_proof *old,const struct ns_page_proof *newer,uint32_t *data,uint32_t *r,uint32_t budget){
 if(!g)return NS_ARGUMENT;
 if(!m||!m->ready||m->read_only||m->error||!states||!io||!io->inspect||!io->erase||!old||!newer||
    !data||!r||!budget||old->valid!=PROOF||newer->valid!=PROOF||old->block==newer->block||
    old->block<4||old->block>=m->blocks||old->block>=2084||newer->block<4||newer->block>=m->blocks||newer->block>=2084||
    !old->mask||!newer->mask||(old->mask&~15u)||(newer->mask&~15u)||
    m->bank_mask[old->block]!=old->mask||m->bank_mask[newer->block]!=newer->mask||
    m->checkpoint_generation!=old->checkpoint_gen||m->checkpoint_key!=old->checkpoint_key||
    newer->checkpoint_gen!=old->checkpoint_gen||newer->checkpoint_key!=old->checkpoint_key)return NS_ARGUMENT;
 uintptr_t ptr[5]={(uintptr_t)old,(uintptr_t)newer,(uintptr_t)data,(uintptr_t)r,(uintptr_t)g};
 const uintptr_t len[5]={sizeof *old,sizeof *newer,16384,64,sizeof *g};
 for(unsigned i=0;i<5;++i){if((ptr[i]&3u)||ptr[i]>UINTPTR_MAX-len[i])return NS_ARGUMENT;
  for(unsigned j=0;j<i;++j)if(ptr[i]<ptr[j]+len[j]&&ptr[j]<ptr[i]+len[i])return NS_ARGUMENT;
 }
 *g=(struct ns_reclaim){0};
 int rc=ns_sftl_ready_block(m,old->block,states[old->block]);if(rc)return rc;
 for(unsigned k=0;k<4;++k){const uint32_t *a=old->record+k*4,*b=newer->record+k*4;uint32_t vba;
  if(a[2]!=b[2]||a[3]!=b[3]||!later(b,a)||ns_sftl_lookup(m,b[2],&vba)||vba!=newer->block*2048u+k)return NS_CONFLICT;
 }
 g->start=io->usec?io->usec(io->opaque):0;g->budget=budget;g->phase=1;
 rc=verify(g,m,io,newer,data,r);if(rc)return rc;
 g->phase=2;rc=scan(g,m,io,old,1,data,r);if(rc)return rc;
 g->phase=3;g->poisoned=1;m->ready=0;
 for(unsigned bank=0;bank<4;++bank)if(old->mask&(1u<<bank)){
  if(io->usec&&io->usec(io->opaque)-g->start>=budget)return NS_TIMEOUT;
  g->ce=bank/2;g->row=(bank%2)<<19|old->block<<7;
  if(io->erase(io->opaque,g->ce,g->row))return NS_IO;
  ++g->erases;
 }
 g->phase=4;rc=scan(g,m,io,old,0,data,r);if(rc)return rc;
 rc=verify(g,m,io,newer,data,r);if(rc)return rc;
 m->ready=1;g->poisoned=0;g->phase=5;return 0;
}
