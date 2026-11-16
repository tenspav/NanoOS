#include "sftl.h"
#define NS_HOLE 0x7fffffu
static uint32_t le32(const unsigned char *p) {
    return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static uint32_t banks(uint32_t mask) {
    uint32_t n=0;for(unsigned k=0;k<4;++k)n+=(mask>>k)&1u;return n;
}
static int later(uint32_t g,uint32_t k,uint32_t oldg,uint32_t oldk) {
    return g>oldg||(g==oldg&&k>oldk);
}
int ns_sftl_init(struct ns_sftl *m,uint32_t blocks,uint32_t sectors,
                const uint8_t *mask,struct ns_run *r,uint32_t nr,
                struct ns_update *u,uint32_t nu,uint32_t gen,uint32_t key) {
    if(!m)return NS_ARGUMENT;
    m->ready=0;
    if(!mask||!r||!nr||(!u&&nu)||!blocks||blocks>4096||
       !sectors||sectors>0x400000u||gen==0xffffffffu||key>65535)return NS_ARGUMENT;
    *m=(struct ns_sftl){.blocks=blocks,.sectors=sectors,.run_capacity=nr,
        .update_capacity=nu,.checkpoint_generation=gen,.checkpoint_key=key,
        .bank_mask=mask,.runs=r,.updates=u};
    return NS_OK;
}
int ns_vfl_address(const struct ns_sftl *m,uint32_t vba,struct ns_address *out) {
    if(!m||!out)return NS_ARGUMENT;
    uint32_t block=vba>>11,off=vba&2047u;
    if(block>=m->blocks)return NS_FORMAT;
    uint32_t mask=m->bank_mask[block],n=banks(mask);
    if(!n||mask&~15u||off>=n*512u-4u)return NS_FORMAT;
    uint32_t rem=off>>2,slot=rem%n,bank=0;
    for(;bank<4;++bank)if(mask&(1u<<bank)){if(!slot)break;--slot;}
    if(bank==4)return NS_FORMAT;
    *out=(struct ns_address){bank>>1,bank&1u,block,rem/n,off&3u};
    return NS_OK;
}
static int valid_run(const struct ns_sftl *m,uint32_t vba,uint32_t n) {
    struct ns_address a;
    if(!n||ns_vfl_address(m,vba,&a))return 0;
    return n<=banks(m->bank_mask[a.block])*512u-4u-(vba&2047u);
}
static int fail(struct ns_sftl *m,int error) {
    m->ready=0;m->error=error;return error;
}
int ns_sftl_node(struct ns_sftl *m,const void *node) {
    if(!m)return NS_ARGUMENT;
    if(!node)return fail(m,NS_ARGUMENT);
    if(m->error)return m->error;
    m->ready=0;
    const unsigned char *b=node;
    if(le32(b+4)!=0xfffffff0u)return fail(m,NS_FORMAT);
    uint32_t lba=le32(b),n=0;int padding=0;
    /* Validate the complete node before adding any run. A corrupt late pair
     * must not leave a partially accepted node in the checkpoint. */
    for(unsigned j=8;j<4096;j+=8) {
        uint32_t v=le32(b+j),count=le32(b+j+4);
        if(v==0xffffffffu&&count==0xffffffffu){padding=1;continue;}
        if(padding||!count||lba>0x400000u||count>0x400000u-lba||
           (v!=NS_HOLE&&!valid_run(m,v,count)))return fail(m,NS_FORMAT);
        lba+=count;++n;
    }
    if(!n||n>m->run_capacity-m->nruns)return fail(m,n?NS_SPACE:NS_FORMAT);
    lba=le32(b);
    for(unsigned j=8,k=0;k<n;j+=8,++k) {
        uint32_t v=le32(b+j),count=le32(b+j+4);
        m->runs[m->nruns++]=(struct ns_run){lba,v,count};lba+=count;
    }
    return NS_OK;
}
int ns_sftl_replay(struct ns_sftl *m,uint32_t vba,const uint32_t r[4]) {
    if(!m)return NS_ARGUMENT;
    if(m->read_only)return NS_NOT_READY;
    if(!r)return fail(m,NS_ARGUMENT);
    if(m->error)return m->error;
    m->ready=0;
    uint32_t type=r[0]&255u,key=r[0]>>16;
    if((type!=1&&type!=2)||(r[0]&0x200u)||r[1]==0xffffffffu||r[3]>r[1]||
       r[2]>=m->sectors||!valid_run(m,vba,1))return fail(m,NS_FORMAT);
    if(!later(r[1],key,m->checkpoint_generation,m->checkpoint_key))return NS_OK;
    if(m->nupdates==m->update_capacity)return fail(m,NS_SPACE);
    m->updates[m->nupdates++]=(struct ns_update){r[2],vba,r[1],key};
    return NS_OK;
}
/* In-place heapsort: bounded stack, no libc, and no quadratic replay sort. */
static int run_less(struct ns_run a,struct ns_run b){return a.lba<b.lba;}
static int update_less(struct ns_update a,struct ns_update b) {
    return a.generation<b.generation||
           (a.generation==b.generation&&a.key<b.key);
}
static int update_lba_less(struct ns_update a,struct ns_update b){
 return a.lba<b.lba||(a.lba==b.lba&&update_less(a,b));
}
#define SORT(name,type,less) \
static void name(type *a,uint32_t n){ \
 for(uint32_t i=1;i<n;++i){uint32_t c=i;while(c){uint32_t p=(c-1)/2; \
  if(!less(a[p],a[c])){break;}type t=a[p];a[p]=a[c];a[c]=t;c=p;}} \
 for(uint32_t end=n;end>1;){type t=a[0];a[0]=a[--end];a[end]=t; \
  uint32_t p=0;while(p<end/2){uint32_t c=p*2+1; \
   if(c+1<end&&less(a[c],a[c+1])){++c;}if(!less(a[p],a[c])){break;} \
   t=a[p];a[p]=a[c];a[c]=t;p=c;}}}
SORT(sort_runs,struct ns_run,run_less)
SORT(sort_updates,struct ns_update,update_less)
SORT(sort_updates_lba,struct ns_update,update_lba_less)
int ns_sftl_finish(struct ns_sftl *m) {
    if(!m)return NS_ARGUMENT;
    if(m->error)return m->error;
    m->ready=0;
    sort_runs(m->runs,m->nruns);uint32_t next=0;
    for(uint32_t i=0;i<m->nruns;++i) {
        struct ns_run r=m->runs[i];
        if(r.lba!=next)return fail(m,NS_FORMAT);
        next+=r.count;
    }
    if(next<m->sectors)return fail(m,NS_FORMAT);
    sort_updates(m->updates,m->nupdates);
    for(uint32_t i=1;i<m->nupdates;++i) {
        struct ns_update a=m->updates[i-1],b=m->updates[i];
        if(a.generation==b.generation&&a.key==b.key&&
           (a.lba!=b.lba||a.vba!=b.vba))return fail(m,NS_CONFLICT);
    }
    m->ready=1;return NS_OK;
}
int ns_sftl_lookup(const struct ns_sftl *m,uint32_t lba,uint32_t *vba) {
    if(!m||!vba||lba>=m->sectors)return NS_ARGUMENT;
    if(!m->ready)return NS_NOT_READY;
    /* Replay remains weave-ordered; scan newest first. A mount can compact
     * this bounded overlay into a hash index without changing its semantics. */
    for(uint32_t i=m->nupdates;i;--i)if(m->updates[i-1].lba==lba) {
        *vba=m->updates[i-1].vba;return NS_OK;
    }
    uint32_t lo=0,hi=m->nruns;
    while(lo<hi){uint32_t mid=lo+(hi-lo)/2;
        if(m->runs[mid].lba<=lba)lo=mid+1;else hi=mid;}
    if(!lo)return NS_FORMAT;
    struct ns_run r=m->runs[lo-1];
    if(lba-r.lba>=r.count)return NS_FORMAT;
    if(r.vba==NS_HOLE)return NS_UNMAPPED;
    *vba=r.vba+lba-r.lba;return NS_OK;
}
int ns_sftl_compact(struct ns_sftl *m){
 if(!m)return NS_ARGUMENT;
 if(!m->ready||m->error)return NS_NOT_READY;
 m->ready=0;sort_updates_lba(m->updates,m->nupdates);uint32_t kept=0;
 for(uint32_t i=0;i<m->nupdates;){uint32_t end=i+1;
  while(end<m->nupdates&&m->updates[end].lba==m->updates[i].lba)++end;
  m->updates[kept++]=m->updates[end-1];i=end;
 }
 m->nupdates=kept;return ns_sftl_finish(m);
}
int ns_sftl_ready_block(const struct ns_sftl *m,uint32_t block,uint32_t state) {
    if(!m||!m->ready||m->read_only)return NS_NOT_READY;
    if(block>=m->blocks||!m->bank_mask[block]||
       (m->bank_mask[block]&~15u)||state!=1)return NS_ARGUMENT;
    for(uint32_t i=0;i<m->nruns;++i)
        if(m->runs[i].vba!=NS_HOLE&&(m->runs[i].vba>>11)==block)return NS_CONFLICT;
    for(uint32_t i=0;i<m->nupdates;++i)
        if((m->updates[i].vba>>11)==block)return NS_CONFLICT;
    return NS_OK;
}

static int snapshot_emit(struct ns_run *out,uint32_t cap,uint32_t *n,uint32_t lba,uint32_t vba,uint32_t count){
 if(!count)return 0;
 if(*n){struct ns_run *last=out+*n-1;
  if(last->lba+last->count==lba&&((vba==NS_HOLE&&last->vba==NS_HOLE)||(last->vba!=NS_HOLE&&vba==last->vba+last->count))){last->count+=count;return 0;}
 }
 if(*n==cap)return NS_SPACE;
 out[(*n)++]=(struct ns_run){lba,vba,count};return 0;
}
int ns_sftl_snapshot(const struct ns_sftl *m,struct ns_run *out,uint32_t cap,struct ns_update *work,uint32_t nw,uint32_t *count){
 if(!m||!out||!cap||!count)return NS_ARGUMENT;
 if(!m->ready||m->error)return NS_NOT_READY;
 if(m->nupdates&&(!work||nw<m->nupdates))return NS_SPACE;
 if(cap>0x400000u||nw>0x400000u)return NS_ARGUMENT;
 uintptr_t p[6]={(uintptr_t)m,(uintptr_t)m->runs,(uintptr_t)m->updates,(uintptr_t)out,(uintptr_t)work,(uintptr_t)count};
 uintptr_t size[6]={sizeof *m,m->nruns*sizeof *m->runs,m->nupdates*sizeof *m->updates,cap*sizeof *out,m->nupdates*sizeof *work,sizeof *count};
 for(unsigned i=0;i<6;++i){if(p[i]>UINTPTR_MAX-size[i])return NS_ARGUMENT;
  for(unsigned j=0;j<i;++j)if((i>=3||j>=3)&&size[i]&&size[j]&&p[i]<p[j]+size[j]&&p[j]<p[i]+size[i])return NS_ARGUMENT;
 }
 *count=0;
 for(unsigned i=0;i<m->nupdates;++i)work[i]=m->updates[i];
 sort_updates_lba(work,m->nupdates);uint32_t nu=0;
 for(unsigned i=0;i<m->nupdates;){unsigned j=i+1;while(j<m->nupdates&&work[j].lba==work[i].lba)++j;work[nu++]=work[j-1];i=j;}
 uint32_t u=0,n=0;
 for(unsigned i=0;i<m->nruns;++i){struct ns_run r=m->runs[i];uint32_t at=r.lba,end=r.lba+r.count;
  while(u<nu&&work[u].lba<end){struct ns_update a=work[u++];
   if(a.lba<at)return NS_CONFLICT;
   int rc=snapshot_emit(out,cap,&n,at,r.vba==NS_HOLE?NS_HOLE:r.vba+at-r.lba,a.lba-at);if(rc)return rc;
   rc=snapshot_emit(out,cap,&n,a.lba,a.vba,1);if(rc)return rc;at=a.lba+1;
  }
  int rc=snapshot_emit(out,cap,&n,at,r.vba==NS_HOLE?NS_HOLE:r.vba+at-r.lba,end-at);if(rc)return rc;
 }
 if(u!=nu)return NS_CONFLICT;
 *count=n;return 0;
}
