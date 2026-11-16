#include "btoc.h"
static uint64_t weave(uint32_t g,uint32_t k){return ((uint64_t)g<<16)|k;}
int ns_btoc_init(struct ns_btoc *b,struct ns_btoc_entry *e,uint32_t cap){
 if(!b||!e||!cap||cap>2048)return NS_ARGUMENT;
 *b=(struct ns_btoc){.entry=e,.capacity=cap};return 0;
}
int ns_btoc_add(struct ns_btoc *b,const uint32_t r[4]){
 if(!b||!b->entry||!r||!b->capacity||b->n>b->capacity)return NS_ARGUMENT;
 uint32_t type=r[0]&255u,role=(r[0]>>8)&1u,key=r[0]>>16;
 if((type!=1&&type!=2)||(r[0]&0xfe00u)||r[1]==0xffffffffu||r[3]>r[1]||r[2]>=0x400000u)return NS_FORMAT;
 uint64_t seq=weave(r[1],key),first=b->sectors?weave(b->first_gen,b->first_key):seq;
 if(b->sectors&&(role!=b->role||seq<=weave(b->last_gen,b->last_key)))return NS_CONFLICT;
 if(seq-first>=0xffffffffu||b->sectors>=2044)return NS_SPACE;
 uint32_t delta=(uint32_t)(seq-first);struct ns_btoc_entry *last=b->n?b->entry+b->n-1:0;
 int merge=last&&last->delta+last->count==delta&&last->epoch==r[3]&&last->lba+last->count==r[2];
 if(!merge&&b->n==b->capacity)return NS_SPACE;
 if(!b->sectors){b->first_gen=r[1];b->first_key=key;b->role=role;}
 if(merge)++last->count;else b->entry[b->n++]=(struct ns_btoc_entry){delta,r[3],r[2],1};
 ++b->sectors;b->last_gen=r[1];b->last_key=key;b->span=delta+1;return 0;
}
static int valid(const struct ns_btoc *b,uint32_t tail,uint32_t span){
 if(!b||!b->entry||!b->sectors||!b->n||b->n>b->capacity||b->capacity>2048||b->role>1||b->first_gen==0xffffffffu||b->first_key>65535||
    b->sectors>2044||!b->span||(tail!=4&&tail!=8)||b->n+1>tail*256u||span<b->span)return NS_ARGUMENT;
 uint32_t count=0;uint64_t end=0;
 for(unsigned i=0;i<b->n;++i){const struct ns_btoc_entry *e=b->entry+i;
  if(!e->count||e->count>2044-count||e->delta<end||e->lba>=0x400000u||e->count>0x400000u-e->lba||e->epoch>(weave(b->first_gen,b->first_key)+e->delta)>>16)return NS_FORMAT;
  count+=e->count;end=(uint64_t)e->delta+e->count;
 }
 if(count!=b->sectors||end!=b->span||weave(b->last_gen,b->last_key)!=weave(b->first_gen,b->first_key)+b->span-1)return NS_FORMAT;
 if(weave(b->first_gen,b->first_key)+span>(((uint64_t)0xffffffffu)<<16))return NS_SPACE;
 return 0;
}
static uint32_t tag(const struct ns_btoc *b,uint32_t tail){return b->first_key<<16|tail<<9|b->role<<8|0x1c;}
int ns_btoc_encode(const struct ns_btoc *b,uint32_t tail,uint32_t span,uint32_t *data,uint32_t *r){
 if(!span&&b)span=b->span;
 int rc=valid(b,tail,span);if(rc||!data||!r)return rc?rc:NS_ARGUMENT;
 uintptr_t p[4]={(uintptr_t)b,(uintptr_t)b->entry,(uintptr_t)data,(uintptr_t)r};
 uintptr_t n[4]={sizeof *b,b->capacity*sizeof *b->entry,tail*4096u,tail*16u};
 for(unsigned i=0;i<4;++i){if((p[i]&3u)||p[i]>UINTPTR_MAX-n[i])return NS_ARGUMENT;
  for(unsigned j=0;j<i;++j)if(p[i]<p[j]+n[j]&&p[j]<p[i]+n[i])return NS_ARGUMENT;
 }
 for(unsigned i=0;i<tail*1024u;++i)data[i]=0;
 for(unsigned i=0;i<b->n;++i){data[i*4]=b->entry[i].delta;data[i*4+1]=b->entry[i].epoch;data[i*4+2]=b->entry[i].lba;data[i*4+3]=b->entry[i].count;}
 for(unsigned i=0;i<tail;++i){r[i*4]=tag(b,tail);r[i*4+1]=b->first_gen;r[i*4+2]=span;r[i*4+3]=0xffffffffu;}
 return 0;
}
int ns_btoc_match(const struct ns_btoc *b,uint32_t tail,uint32_t span,const uint32_t *data,const uint32_t *r){
 if(!span&&b)span=b->span;
 int rc=valid(b,tail,span);if(rc||!data||!r)return rc?rc:NS_ARGUMENT;
 for(unsigned i=0;i<b->n;++i)if(data[i*4]!=b->entry[i].delta||data[i*4+1]!=b->entry[i].epoch||data[i*4+2]!=b->entry[i].lba||data[i*4+3]!=b->entry[i].count)return NS_VERIFY;
 if(data[b->n*4+3])return NS_VERIFY;
 for(unsigned i=0;i<tail;++i)if(r[i*4]!=tag(b,tail)||r[i*4+1]!=b->first_gen||r[i*4+2]!=span||r[i*4+3]!=0xffffffffu)return NS_VERIFY;
 return 0;
}
void ns_btoc_statistics(const struct ns_btoc *b,uint32_t out[4]){
 uint32_t min=0xffffffffu,max=0,n=0;uint64_t total=0;
 for(unsigned i=0;i<b->n;++i){uint32_t e=b->entry[i].epoch;if(e==0xffffffffu)continue;
  if(e<min)min=e;
  if(e>max)max=e;
  total+=e;++n;
 }
 if(!n)n=1;
 out[0]=min==0xffffffffu?max:min;out[1]=max;out[2]=(uint32_t)(total/n);out[3]=n;
}
static int expired(struct ns_writer *w,uint32_t start,uint32_t budget){return w->io.usec&&w->io.usec(w->io.opaque)-start>=budget;}
int ns_write_close(struct ns_writer *w,struct ns_close *c,uint32_t tail,struct ns_btoc_entry *entries,uint32_t cap,uint32_t *data,uint32_t *r,uint32_t budget){
 if(!w||!c||!entries||!data||!r||!budget||(tail!=4&&tail!=8))return NS_ARGUMENT;
 if(w->poisoned)return NS_WRITE_POISONED;
 struct ns_sftl *m=w->map;unsigned banks=0;for(unsigned i=0;i<4;++i)banks+=(w->mask>>i)&1u;
 if(!w->active||w->closed||w->pending||!m||!m->ready||m->read_only||m->error||m->nupdates!=w->expected_updates||
    m->checkpoint_generation!=w->checkpoint_gen||m->checkpoint_key!=w->checkpoint_key||m->bank_mask[w->block]!=w->mask||
    w->next!=banks*512u-tail)return NS_NOT_READY;
 if(tail==8&&banks<2)return NS_UNSUPPORTED;
 if(!m->nupdates||m->updates[m->nupdates-1].generation!=w->gen||m->updates[m->nupdates-1].key!=w->key)return NS_CONFLICT;
 if(!w->io.program)return NS_UNSUPPORTED;
 uintptr_t p[9]={(uintptr_t)c,(uintptr_t)entries,(uintptr_t)data,(uintptr_t)r,(uintptr_t)w->stage,(uintptr_t)w->scratch,(uintptr_t)w->scratch_record,(uintptr_t)w->stage_record,(uintptr_t)w};
 uintptr_t n[9]={sizeof *c,cap*sizeof *entries,tail*4096u,tail*16u,16384,16384,64,64,sizeof *w};
 if(!cap||cap>2048)return NS_ARGUMENT;
 for(unsigned i=0;i<9;++i){if((p[i]&3u)||p[i]>UINTPTR_MAX-n[i])return NS_ARGUMENT;
  for(unsigned j=0;j<i;++j)if(p[i]<p[j]+n[j]&&p[j]<p[i]+n[i])return NS_ARGUMENT;
 }
 *c=(struct ns_close){.tail_sectors=tail,.phase=1};uint32_t start=w->io.usec?w->io.usec(w->io.opaque):0;
 struct ns_btoc b;int rc=ns_btoc_init(&b,entries,cap);if(rc)return rc;
 for(unsigned off=0;off<w->next;off+=4){
  if(expired(w,start,budget))return NS_TIMEOUT;
  struct ns_address a;rc=ns_vfl_address(m,w->block*2048u+off,&a);if(rc)return rc;
  ++c->reads;rc=w->io.inspect(w->io.opaque,a.ce,(a.cau<<19)|(a.block<<7)|a.page,w->scratch,w->scratch_record);if(rc)return NS_IO;
  for(unsigned k=0;k<4;++k){rc=ns_btoc_add(&b,w->scratch_record+k*4);if(rc)return rc;}
 }
 if(b.last_gen!=w->gen||b.last_key!=w->key||weave(b.first_gen,b.first_key)<=weave(w->checkpoint_gen,w->checkpoint_key))return NS_CONFLICT;
 rc=ns_btoc_encode(&b,tail,0,data,r);if(rc)return rc;
 c->entries=b.n;c->data_sectors=b.sectors;c->span=b.span;ns_btoc_statistics(&b,c->statistics);c->phase=2;
 /* The final metadata pages cannot be addressed by ns_vfl_address's data
  * guard. Translate the bounded tail explicitly, in usable-bank stripe order. */
 for(unsigned off=w->next;off<banks*512u;off+=4){
  unsigned ordinal=off/4,slot=ordinal%banks,bank=0;
  for(;bank<4;++bank)if(w->mask>>bank&1u){if(!slot)break;--slot;}
  unsigned ce=bank/2,row=(bank%2)<<19|w->block<<7|(ordinal/banks);
  if(expired(w,start,budget))return NS_TIMEOUT;
  ++c->reads;rc=w->io.inspect(w->io.opaque,ce,row,w->scratch,w->scratch_record);
  if(rc&&rc!=-2)return NS_IO;
  for(unsigned k=0;k<4096;++k)if(w->scratch[k]!=0xffffffffu)return NS_NOT_BLANK;
  for(unsigned k=0;k<16;++k)if(w->scratch_record[k]!=0xffffffffu)return NS_NOT_BLANK;
 }
 c->phase=3;c->poisoned=1;w->poisoned=1;m->ready=0;
 for(unsigned i=0;i<tail/4;++i){
  unsigned ordinal=w->next/4+i,slot=ordinal%banks,bank=0;
  for(;bank<4;++bank)if(w->mask>>bank&1u){if(!slot)break;--slot;}
  unsigned ce=bank/2,row=(bank%2)<<19|w->block<<7|(ordinal/banks);
  if(expired(w,start,budget))return NS_TIMEOUT;
  if(w->io.program(w->io.opaque,ce,row,data+i*4096,r+i*16))return NS_IO;
  ++c->programs;++c->reads;
  if(w->io.inspect(w->io.opaque,ce,row,w->scratch,w->scratch_record))return NS_VERIFY;
  for(unsigned k=0;k<4096;++k)if(w->scratch[k]!=data[i*4096+k])return NS_VERIFY;
  for(unsigned k=0;k<16;++k)if(w->scratch_record[k]!=r[i*16+k])return NS_VERIFY;
 }
 if(expired(w,start,budget))return NS_TIMEOUT;
 c->phase=4;c->poisoned=0;w->poisoned=0;w->closed=1;m->ready=1;return 0;
}
