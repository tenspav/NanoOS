#include "mount.h"
#define BLOCKS 2088u
/* N31 context body identifies2084 as VFL-info;2086/2087 hold contexts.
 * The four-block tail belongs to VFL, outside SFTL data/checkpoint space.
 * Validate that profile in both context-body copies before using it. */
#define DATA_BLOCKS 2084u
static int later(uint32_t g,uint32_t k,uint32_t og,uint32_t ok){return g>og||(g==og&&k>ok);}
static int expired(struct ns_mount *m){return m->io.usec&&m->io.usec(m->io.opaque)-m->start>=m->budget;}
static int erased(const uint32_t *p,const uint32_t *r){
 for(unsigned i=0;i<16;++i)if(r[i]!=0xffffffffu)return 0;
 for(unsigned i=0;i<4096;++i)if(p[i]!=0xffffffffu)return 0;
 return 1;
}
/* Return1 ONLY for completely transferred all-FF bytes/records, never for a
 * timeout or an arbitrary device error. Non-erased errored data is rejected. */
static int read_page(struct ns_mount *m,uint32_t ce,uint32_t row){
 if(expired(m))return NS_TIMEOUT;
 ++m->reads;
 int rc=m->io.inspect(m->io.opaque,ce,row,m->page,m->record);m->array_error=rc==-2;
 if(rc==0||rc==-2){if(erased(m->page,m->record)){++m->erased;return 1;}if(!rc)return 0;}
 ++m->errors;return NS_IO;
}
static uint32_t popcount(uint32_t v){uint32_t n=0;for(unsigned k=0;k<4;++k)n+=(v>>k)&1u;return n;}
static uint32_t row_for(uint32_t block,uint32_t bank,uint32_t page){return (bank%2u)<<19|block<<7|page;}
static int context(struct ns_mount *m){
 uint32_t block[2]={0,0},generation[2]={0,0};unsigned count=0;
 for(unsigned b=2086;b<=2087;++b){
  if(expired(m))return NS_TIMEOUT;
  ++m->reads;int rc=m->io.context(m->io.opaque,b,5,1,4,m->page,m->record);
  if(rc||m->record[0]!=0x00ffff20u)continue;
  block[count]=b;generation[count++]=m->record[1];
 }
 if(count==2&&generation[1]<generation[0]){
  uint32_t t=block[0];block[0]=block[1];block[1]=t;
  t=generation[0];generation[0]=generation[1];generation[1]=t;
 }
 for(unsigned attempt=0;attempt<count;++attempt){
  uint32_t rootgen=generation[attempt]+5u;int good=1;
  for(unsigned b=0;b<BLOCKS;++b)m->mask[b]=15;
  for(unsigned p=0;p<6;++p){
   if(expired(m))return NS_TIMEOUT;
   ++m->reads;int rc=m->io.context(m->io.opaque,block[attempt],p,1,4,m->page,m->record);
   uint32_t tag=p==0||p==5?0x00ffff20u:0x20u|((p-1u)<<8);
   if(rc||m->record[0]!=tag||m->record[1]!=rootgen-p){good=0;break;}
   if((p==0||p==5)&&m->page[3]!=(DATA_BLOCKS|(DATA_BLOCKS<<16))){good=0;break;}
   if(p>=1&&p<=4)for(unsigned b=0;b<BLOCKS;++b){
    uint32_t s=(m->page[b/2]>>((b&1u)*16))&65535u;
    if(s==0xbffeu)m->mask[b]&=(uint8_t)~(1u<<(p-1));
   }
  }
  if(good){m->context_block=block[attempt];m->context_gen=generation[attempt];return 0;}
 }
 return NS_INCOMPLETE;
}
static void candidate(struct ns_mount *m,uint32_t block,const uint32_t *r){
 if((r[0]&65535u)!=0x011fu||r[1]==0xffffffffu)return;
 for(unsigned i=0;i<m->roots;++i)if(m->candidate[i].block==block)return;
 unsigned pos=0;
 while(pos<m->roots&&!later(r[1],r[0]>>16,m->candidate[pos].gen,m->candidate[pos].key))++pos;
 if(pos==8)return;
 if(m->roots<8)++m->roots;
 for(unsigned i=m->roots-1;i>pos;--i)m->candidate[i]=m->candidate[i-1];
 m->candidate[pos]=(struct ns_root){block,r[1],r[0]>>16};
}
static int survey(struct ns_mount *m){
 for(unsigned b=0;b<DATA_BLOCKS;++b){m->first_gen[b]=m->first_key[b]=0;
  for(unsigned bank=0;bank<4;++bank){if(!(m->mask[b]&(1u<<bank)))continue;
   int rc=read_page(m,bank/2,row_for(b,bank,0));
   if(rc==NS_TIMEOUT)return rc;
   /* A bad or unreadable usable-bank first page cannot be silently skipped. */
   if(rc<0){
    if(rc==NS_IO&&m->recovery_enabled&&m->array_error){m->deferred[b]=1;continue;}
    m->detail=b;return rc;
   }if(rc==1){if(!m->recovery_enabled)break;continue;}
   int partial=0;
   for(unsigned k=0;k<4;++k){const uint32_t *r=m->record+k*4;
    if(r[0]==0xffffffffu&&r[1]==0xffffffffu&&r[2]==0xffffffffu&&r[3]==0xffffffffu)partial=1;
   }
   if(partial){if(!m->recovery_enabled){m->detail=b;return NS_FORMAT;}m->deferred[b]=1;}
   for(unsigned k=0;k<4;++k){const uint32_t *r=m->record+k*4;unsigned type=r[0]&255u;
    if(type!=1&&type!=2&&type!=0x1f)m->deferred[b]|=2;
   }
   candidate(m,b,m->record);
   for(unsigned k=0;k<4;++k){const uint32_t *r=m->record+k*4;unsigned type=r[0]&255u;
    if(type==1||type==2){if(r[1]==0xffffffffu||r[3]>r[1])return NS_FORMAT;
     if(later(r[1],r[0]>>16,m->first_gen[b],m->first_key[b])){m->first_gen[b]=r[1];m->first_key[b]=r[0]>>16;}}
   }
   if(!m->recovery_enabled)break;
  }
 }
 return m->roots?0:NS_INCOMPLETE;
}
static int cp_record(const uint32_t *r,uint32_t gen,uint32_t key,unsigned section){
 return (r[0]&65535u)==(0x1fu|(section<<8))&&r[1]==gen&&(r[0]>>16)==key;
}
static int load_root(struct ns_mount *m,struct ns_root root,struct ns_run *runs,uint32_t nr,
                     struct ns_update *updates,uint32_t nu){
 static const uint8_t section[48]={1,1,1,1,2,2,2,2,3,3,3,3,3,3,3,3,
  5,5,5,5,5,5,5,5,5,5,5,5,6,6,6,6,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4};
 uint32_t gen=root.gen,key=root.key,index=0;int empty_tree=0;
 int rc=ns_sftl_init(&m->map,DATA_BLOCKS,1,m->mask,runs,nr,updates,nu,gen,key);if(rc)return rc;
 for(unsigned b=0;b<BLOCKS;++b)m->states[b]=255;
 m->tree_nodes=0;
 m->root=root.block;
 m->open_count=0;m->open_block=0xffffffffu;
 for(unsigned p=0;index<48&&p<128;++p)for(unsigned bank=0;bank<4&&index<48;++bank){
  if(!(m->mask[root.block]&(1u<<bank)))continue;
  rc=read_page(m,bank/2,row_for(root.block,bank,p));if(rc)return rc<0?rc:NS_FORMAT;
  for(unsigned k=0;k<4&&index<48;++k,++index){const uint32_t *r=m->record+k*4,*data=m->page+k*1024;
   if(!cp_record(r,gen,key,section[index])){m->detail=index;return NS_FORMAT;}
   if(index==0){
    for(unsigned j=0;j<16;++j)m->base_head[j]=data[j];
    if(data[0]!=2||data[1]!=root.block||data[2]>=DATA_BLOCKS)return NS_FORMAT;
   }
   if(index>=8&&index<16){unsigned first=(index-8)*512;
    for(unsigned j=0;j<512&&first+j<DATA_BLOCKS;++j){
     uint32_t state=data[j*2]&255u;if(state>9)return NS_FORMAT;m->states[first+j]=(uint8_t)state;
     if(first+j==root.block)for(unsigned q=0;q<2;++q)m->root_state_words[q]=data[j*2+q];
     if(first+j==m->base_head[2])for(unsigned q=0;q<2;++q)m->hint_state_words[q]=data[j*2+q];
    }
   }
   if(index>=32){
    if(data[0]==0xffffffffu&&data[1]==0xffffffffu)empty_tree=1;
    else {if(empty_tree)return NS_FORMAT;rc=ns_sftl_node(&m->map,data);if(rc)return rc;++m->tree_nodes;}
   }
   if(key==65535u){key=0;++gen;}else ++key;
  }
 }
 if(index!=48||!m->tree_nodes)return NS_FORMAT;
 m->map.checkpoint_generation=gen;m->map.checkpoint_key=key;
 /* Threshold is the final checkpoint record, not the next write position. */
 if(!key){--m->map.checkpoint_generation;m->map.checkpoint_key=65535;}else --m->map.checkpoint_key;
 uint32_t end=0;for(unsigned i=0;i<m->map.nruns;++i){uint32_t e=m->map.runs[i].lba+m->map.runs[i].count;if(e>end)end=e;}
 if(!end||end>0x400000u)return NS_FORMAT;
 m->map.sectors=end;
 rc=ns_sftl_finish(&m->map);
 if(rc){m->detail=0x20000000u;return rc;}
 /* Survey array errors may only belong to a truly unreferenced READY.
  * Do not turn an error in an existing checkpoint or asset block into a hole. */
 for(unsigned b=0;b<DATA_BLOCKS;++b)if(m->deferred[b]){
  if((m->deferred[b]&1u)&&ns_sftl_ready_block(&m->map,b,m->states[b]))return NS_IO;
  if((m->deferred[b]&2u)&&m->states[b]==1){if(!m->recovery_enabled)return NS_FORMAT;}
  else m->deferred[b]&=1u;
 }
 /* run44: base word2 names another state7 block, not open data. There
  * are two state4 blocks; replay must scan all of them. The serialized
  * header hint is diagnostic only and must never narrow the replay set. */
 m->open_block=0xffffffffu;m->open_count=0;
 for(unsigned b=0;b<DATA_BLOCKS;++b)if(m->states[b]==4){
  if(!m->open_count)m->open_block=b;
  ++m->open_count;
 }
 if(m->states[root.block]!=7){
  m->detail=0x10000000u|((uint32_t)m->states[root.block]<<8)|m->states[m->base_head[2]];
  m->map.ready=0;return NS_FORMAT;
 }
 return 0;
}
static int drop_tail(struct ns_mount *m,unsigned b,unsigned bank,unsigned page){
 if(!m->recovery_enabled||m->states[b]!=1||m->recovered_pages)return NS_IO;
 /* This check runs before this block's replay entries are inserted. A later
  * tail is allowed to have a valid replay prefix, so inspect base runs only. */
 for(unsigned i=0;i<m->map.nruns;++i)if(m->map.runs[i].vba!=0x7fffffu&&m->map.runs[i].vba/2048u==b)return NS_IO;
 m->recovered_pages=1;m->quarantine_block=b;m->bad_ce=bank/2;m->bad_row=row_for(b,bank,page);m->deferred[b]=1;return 0;
}
static int page_records(struct ns_mount *m,unsigned p,int *partial){
 int bad=0;*partial=0;
 for(unsigned k=0;k<4;++k){const uint32_t *r=m->record+k*4;unsigned type=r[0]&255u;
  if(r[0]==0xffffffffu&&r[1]==0xffffffffu&&r[2]==0xffffffffu&&r[3]==0xffffffffu)*partial=1;
  if(type==0x1c){if(p!=127)bad=1;continue;}
  if((type!=1&&type!=2)||(r[0]&0x200u)||r[1]==0xffffffffu||r[3]>r[1]||r[2]>=m->map.sectors)bad=1;
 }
 return bad?NS_FORMAT:0;
}
static int replay(struct ns_mount *m){
 uint32_t cpgen=m->map.checkpoint_generation,cpkey=m->map.checkpoint_key;
 for(unsigned b=0;b<DATA_BLOCKS;++b){
  int newer=later(m->first_gen[b],m->first_key[b],cpgen,cpkey);
  if(m->states[b]!=4&&!newer&&!m->deferred[b])continue;
  if(m->states[b]!=4&&m->states[b]!=1){m->detail=b;return NS_INCOMPLETE;}
  int hole=0,have=0;
  for(unsigned p=0;p<128;++p)for(unsigned bank=0;bank<4;++bank){if(!(m->mask[b]&(1u<<bank)))continue;
   m->detail=0x30000000u|(b<<9)|(p<<2)|bank;
   int rc=read_page(m,bank/2,row_for(b,bank,p));
   if(rc<0){if(rc==NS_IO&&m->array_error&&!hole&&!drop_tail(m,b,bank,p)){hole=1;continue;}return rc;}
   ++m->replay_pages;
   if(rc==1){hole=1;continue;}
   if(hole){m->detail=b;return NS_FORMAT;}
   int partial=0;rc=page_records(m,p,&partial);
   if(rc){if(partial&&!drop_tail(m,b,bank,p)){hole=1;continue;}return rc;}
   for(unsigned k=0;k<4;++k){const uint32_t *r=m->record+k*4;unsigned type=r[0]&255u;
    m->detail=0x40000000u|(b<<11)|(p<<4)|(bank<<2)|k;
    if(type==0x1c){if(p!=127)return NS_FORMAT;continue;}
    if(type!=1&&type!=2)return NS_FORMAT;
    if(later(r[1],r[0]>>16,cpgen,cpkey))have=1;
    unsigned n=popcount(m->mask[b]),slot=0;for(unsigned q=0;q<bank;++q)slot+=(m->mask[b]>>q)&1u;
    uint32_t vba=b*2048u+p*n*4u+slot*4u+k;
    rc=ns_sftl_replay(&m->map,vba,r);if(rc)return rc;
   }
  }
  if(newer&&!have&&m->quarantine_block!=b)return NS_INCOMPLETE;
 }
 int rc=ns_sftl_finish(&m->map);if(!rc&&m->recovered_pages){m->states[m->quarantine_block]=9;m->map.read_only=1;}
 return rc;
}
static int mount_n31(struct ns_mount *m,const struct ns_mount_io *io,uint8_t *mask,
 uint8_t *states,uint32_t *fg,uint32_t *fk,uint32_t *data,uint32_t *record,
 struct ns_run *runs,uint32_t nr,struct ns_update *updates,uint32_t nu,uint32_t budget,int recover){
 if(!m)return NS_ARGUMENT;
 *m=(struct ns_mount){.ready_block=0xffffffffu,.quarantine_block=0xffffffffu,.recovery_enabled=(uint32_t)recover};
 if(!io||!io->inspect||!io->context||!mask||!states||!fg||!fk||!data||!record||!runs||!nr||!budget)return NS_ARGUMENT;
 m->io=*io;m->mask=mask;m->states=states;m->first_gen=fg;m->first_key=fk;m->page=data;m->record=record;m->blocks=BLOCKS;m->budget=budget;
 m->start=io->usec?io->usec(io->opaque):0;m->stage=1;
 int rc=context(m);if(rc)goto bad;
 m->stage=2;rc=survey(m);if(rc)goto bad;
 m->stage=3;rc=NS_INCOMPLETE;
 for(unsigned i=0;i<m->roots;++i){rc=load_root(m,m->candidate[i],runs,nr,updates,nu);if(!rc)break;if(rc==NS_TIMEOUT||rc==NS_IO)goto bad;}
 if(rc)goto bad;
 m->stage=4;rc=replay(m);if(rc)goto bad;
 for(unsigned b=1;b<DATA_BLOCKS;++b)if(!ns_sftl_ready_block(&m->map,b,m->states[b])){m->ready_block=b;break;}
 m->stage=5;m->detail=0;return 0;
 bad:m->map.ready=0;return rc;
}
int ns_mount_n31(struct ns_mount *m,const struct ns_mount_io *io,uint8_t *mask,uint8_t *states,uint32_t *fg,uint32_t *fk,uint32_t *data,uint32_t *record,struct ns_run *runs,uint32_t nr,struct ns_update *updates,uint32_t nu,uint32_t budget){
 return mount_n31(m,io,mask,states,fg,fk,data,record,runs,nr,updates,nu,budget,0);
}
int ns_mount_n31_recover(struct ns_mount *m,const struct ns_mount_io *io,uint8_t *mask,uint8_t *states,uint32_t *fg,uint32_t *fk,uint32_t *data,uint32_t *record,struct ns_run *runs,uint32_t nr,struct ns_update *updates,uint32_t nu,uint32_t budget){
 return mount_n31(m,io,mask,states,fg,fk,data,record,runs,nr,updates,nu,budget,1);
}
int ns_mount_page_read(void *opaque,uint32_t ce,uint32_t row,void *out,uint32_t *record){
 struct ns_mount *m=opaque;int rc=m->io.inspect(m->io.opaque,ce,row,out,record);
 return rc?NS_IO:0;
}
