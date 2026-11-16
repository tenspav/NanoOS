#include "checkpoint_plan.h"
#include "write.h"
#define HOLE 0x7fffffu
static uint32_t get(const unsigned char *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int next(uint32_t *g,uint32_t *k){if(*k==65535){*k=0;++*g;}else ++*k;return *g==0xffffffffu?NS_SPACE:0;}
int ns_checkpoint_plan(struct ns_cp_plan *p,const struct ns_sftl *m,const void *input,const uint32_t *ir,
 const struct ns_cp_closed *closed,uint32_t nc,uint32_t *out,uint32_t *records,struct ns_run *runs,uint32_t nr,struct ns_update *updates,uint32_t nu){
 if(!p||!m||!input||!ir||!out||!records||!runs||!nr||nr>0x400000u||nc>2084||(!closed&&nc))return NS_ARGUMENT;
 if(!m->ready||m->error||m->blocks!=2084)return NS_NOT_READY;
 uintptr_t ptr[10]={(uintptr_t)p,(uintptr_t)input,(uintptr_t)ir,(uintptr_t)out,(uintptr_t)records,(uintptr_t)runs,(uintptr_t)updates,(uintptr_t)m->runs,(uintptr_t)m->updates,(uintptr_t)closed};
 uintptr_t len[10]={sizeof *p,196608,768,196608,768,nr*sizeof *runs,m->nupdates*sizeof *updates,m->nruns*sizeof *m->runs,m->nupdates*sizeof *m->updates,nc*sizeof *closed};
 for(unsigned i=0;i<10;++i){if(ptr[i]>UINTPTR_MAX-len[i])return NS_ARGUMENT;
  for(unsigned j=0;j<i;++j)if(len[i]&&len[j]&&ptr[i]<ptr[j]+len[j]&&ptr[j]<ptr[i]+len[i])return NS_ARGUMENT;
 }
 for(unsigned i=0;i<nc;++i){if(closed[i].block<4||closed[i].block>=2084||(closed[i].tail!=4&&closed[i].tail!=8)||closed[i].role>1||!closed[i].statistics[3]||closed[i].statistics[0]>closed[i].statistics[2]||closed[i].statistics[2]>closed[i].statistics[1])return NS_ARGUMENT;
  for(unsigned j=0;j<i;++j)if(closed[i].block==closed[j].block)return NS_ARGUMENT;
 }
 *p=(struct ns_cp_plan){.phase=1};const unsigned char *b=input;
 uint32_t roots=get(b);if(!roots||roots>32||get(b+16380)!=2084)return NS_FORMAT;
 for(unsigned i=0;i<roots;++i){uint32_t block=get(b+4+i*4);if(block>=2084||(get(b+32768+block*8)&255u)!=7)return NS_FORMAT;
  for(unsigned j=0;j<i;++j)if(get(b+4+j*4)==block)return NS_FORMAT;
 }
 static const unsigned char sections[48]={1,1,1,1,2,2,2,2,3,3,3,3,3,3,3,3,5,5,5,5,5,5,5,5,5,5,5,5,6,6,6,6,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4};
 uint32_t gen=ir[1],key=ir[0]>>16;
 for(unsigned i=0;i<48;++i){
  uint32_t offset=0,capacity=0;
  if(i>=8&&i<16){offset=(i-8)*512;capacity=512;}
  else if(i>=16&&i<28){offset=(i-16)*256;capacity=256;}
  else if(i>=28&&i<32){offset=(i-28)*1024;capacity=1024;}
  uint32_t remain=offset<2084?2084-offset:0;if(capacity>remain)capacity=remain;
  if(ir[i*4+2]!=((capacity<<16)|offset)||ir[i*4+3]!=0xffffffffu)return NS_FORMAT;
  if((ir[i*4]&65535u)!=(0x1fu|sections[i]<<8)||ir[i*4+1]!=gen||(ir[i*4]>>16)!=key)return NS_FORMAT;
  if(i<47&&next(&gen,&key))return NS_SPACE;
 }
 if(gen!=m->checkpoint_generation||key!=m->checkpoint_key)return NS_CONFLICT;
 uint32_t stats=get(b+16384),pos=4;if(stats>2047)return NS_FORMAT;
 for(unsigned i=0;i<stats;++i){if(pos>16384-8)return NS_FORMAT;uint32_t n=get(b+16384+pos+4);pos+=8;if(n>(16384-pos)/4)return NS_FORMAT;pos+=n*4;}
 uint32_t idx=0;int empty=0;
 for(unsigned node=0;node<16;++node){const unsigned char *d=b+(32+node)*4096;uint32_t lba=get(d);
  if(lba==0xffffffffu&&get(d+4)==0xffffffffu){empty=1;continue;}
  if(empty||get(d+4)!=0xfffffff0u)return NS_FORMAT;
  int padding=0;
  for(unsigned k=8;k<4096;k+=8){uint32_t v=get(d+k),n=get(d+k+4);
   if(v==0xffffffffu&&n==0xffffffffu){padding=1;continue;}
   if(padding||idx==m->nruns||m->runs[idx].lba!=lba||m->runs[idx].vba!=v||m->runs[idx].count!=n)return NS_CONFLICT;
   lba+=n;++idx;
   if(v!=HOLE){if(v/2048u>=2084)return NS_FORMAT;p->refs[v/2048u]+=n;}
  }
 }
 if(idx!=m->nruns)return NS_CONFLICT;
 for(unsigned block=0;block<2084;++block){uint32_t state=get(b+32768+block*8),aux=get(b+32768+block*8+4);
  if((state&255u)>9||(aux&65535u)||(state>>16)!=p->refs[block])return NS_CONFLICT;
  p->refs[block]=0;
 }
 int rc=ns_sftl_snapshot(m,runs,nr,updates,nu,&p->runs);if(rc)return rc;
 p->nodes=(p->runs+510)/511;if(p->nodes>16)return NS_SPACE;
 for(unsigned i=0;i<p->runs;++i)if(runs[i].vba!=HOLE)p->refs[runs[i].vba/2048u]+=runs[i].count;
 for(unsigned i=0;i<196608/4;++i)out[i]=get(b+i*4);
 for(unsigned block=0;block<2084;++block){uint32_t old=out[8192+block*2],state=old&255u;
  for(unsigned i=0;i<nc;++i)if(closed[i].block==block){
   if(state!=1||(closed[i].tail!=4&&closed[i].tail!=8)||closed[i].role>1)return NS_ARGUMENT;
   state=3;out[8192+block*2]=3|((closed[i].tail*2+closed[i].role)<<8);
   for(unsigned j=0;j<4;++j)out[16384+block*4+j]=closed[i].statistics[j];
  }
  if(p->refs[block]&&state!=3&&state!=4)return NS_CONFLICT;
  if(p->refs[block]>65535u)return NS_FORMAT;
  out[8192+block*2]=(out[8192+block*2]&65535u)|(p->refs[block]<<16);
  if(out[8192+block*2]!=old)++p->changed_blocks;
 }
 for(unsigned i=32768;i<49152;++i)out[i]=0xffffffffu;
 for(unsigned i=0;i<p->runs;++i){unsigned node=i/511,slot=i%511;uint32_t *d=out+(32+node)*1024;
  if(!slot){d[0]=runs[i].lba;d[1]=0xfffffff0u;}
  d[2+slot*2]=runs[i].vba;d[3+slot*2]=runs[i].count;
 }
 gen=m->checkpoint_generation;key=m->checkpoint_key;if(m->nupdates){gen=m->updates[m->nupdates-1].generation;key=m->updates[m->nupdates-1].key;}
 if(next(&gen,&key))return NS_SPACE;
 p->first_gen=gen;p->first_key=key;
 for(unsigned i=0;i<48;++i){records[i*4]=(key<<16)|sections[i]<<8|0x1f;records[i*4+1]=gen;records[i*4+2]=ir[i*4+2];records[i*4+3]=ir[i*4+3];if(i<47&&next(&gen,&key))return NS_SPACE;}
 p->last_gen=gen;p->last_key=key;
 struct ns_sftl shadow;rc=ns_sftl_init(&shadow,m->blocks,m->sectors,m->bank_mask,runs,nr,0,0,gen,key);if(rc)return rc;
 shadow.nruns=p->runs;rc=ns_sftl_finish(&shadow);if(rc)return rc;
 /* Independently exercise lookup boundaries, each replay LBA and10k
  * deterministic addresses against the live source lookup implementation. */
 uint32_t random=0x4e435031u;
 for(unsigned i=0;i<m->nruns*2u+m->nupdates+10000u;++i){uint32_t lba;
  if(i<m->nruns*2u){struct ns_run r=m->runs[i/2];lba=r.lba+(i&1u?r.count-1:0);}
  else if(i<m->nruns*2u+m->nupdates)lba=m->updates[i-m->nruns*2u].lba;
  else{random^=random<<13;random^=random>>17;random^=random<<5;lba=random%m->sectors;}
  uint32_t a=0,c=0;int ra=ns_sftl_lookup(m,lba,&a),rb=ns_sftl_lookup(&shadow,lba,&c);if(ra!=rb||(!ra&&a!=c))return NS_VERIFY;++p->checks;
 }
 p->phase=2;return 0;
}
