#include "btoc.h"
#include "append.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t media[3][4][128][4112];
struct source {unsigned programs,fail_at,no_summary,corrupt_summary,late_timeout;uint32_t ticks;};
static uint32_t usec(void *p){struct source *s=p;return s->ticks+=10;}
static int inspect(void *p,uint32_t ce,uint32_t row,void *data,uint32_t *r){
 (void)p;unsigned block=((row>>7)&4095)-7,bank=ce*2+(row>>19),page=row&127;
 assert(block<3&&bank<4);memcpy(data,media[block][bank][page],16384);memcpy(r,media[block][bank][page]+4096,64);
 return r[0]==0xffffffffu?-2:0;
}
static int program(void *p,uint32_t ce,uint32_t row,const void *data,const uint32_t *r){
 struct source *s=p;unsigned block=((row>>7)&4095)-7,bank=ce*2+(row>>19),page=row&127;
 assert(block<3&&bank<4);for(unsigned i=0;i<4112;++i)assert(media[block][bank][page][i]==0xffffffffu);
 ++s->programs;
 if((r[0]&255u)==0x1c&&s->no_summary)return 0;
 memcpy(media[block][bank][page],data,16384);memcpy(media[block][bank][page]+4096,r,64);
 if((r[0]&255u)==0x1c){if(s->corrupt_summary)media[block][bank][page][1]^=1;if(s->late_timeout)s->ticks+=1000000;}
 return s->fail_at==s->programs?-1:0;
}
static struct ns_sftl m,fresh;static struct ns_run runs[4],fruns[4];
static struct ns_update updates[12],fupdates[8192];
static uint8_t masks[10],states[10],leases[10];static struct ns_writer w;static struct ns_append a;
static uint32_t stage[4096],sr[16],scratch[4096],rr[16],data[4096],lb[4]={0,1,2,3},ep[4]={9,9,9,9};
static void init_map(struct ns_sftl *map,struct ns_run *r,struct ns_update *u,unsigned cap){
 assert(!ns_sftl_init(map,10,8,masks,r,4,u,cap,9,65530));
 uint32_t node[1024];memset(node,255,sizeof node);node[0]=0;node[1]=0xfffffff0;node[2]=0x7fffff;node[3]=8;
 assert(!ns_sftl_node(map,node)&&!ns_sftl_finish(map));
}
static int setup(struct source *s,unsigned limit){
 memset(media,255,sizeof media);memset(states,9,sizeof states);memset(masks,15,sizeof masks);
 states[7]=states[8]=states[9]=1;masks[8]=8;masks[9]=6;
 init_map(&m,runs,updates,12);struct ns_write_io io={s,inspect,program,0,usec};
 return ns_append_open(&a,&w,&m,states,leases,sizeof leases,&io,stage,sr,scratch,rr,1000000,limit);
}
static int put(unsigned seq){for(unsigned i=0;i<4096;++i)data[i]=seq*65537u+i;return ns_append_page(&a,lb,ep,data);}
static struct ns_btoc_entry entries[2048];static uint32_t summary[8192],summary_record[32];static struct ns_close closed;
static void recover(unsigned seq){
 init_map(&fresh,fruns,fupdates,8192);
 for(unsigned b=7;b<10;++b){unsigned n=__builtin_popcount(masks[b]);
  for(unsigned pg=0;pg<128;++pg){unsigned slot=0;for(unsigned bank=0;bank<4;++bank)if(masks[b]>>bank&1){
   const uint32_t *r=media[b-7][bank][pg]+4096;
   if(r[0]!=0xffffffffu&&(r[0]&255u)!=0x1c)for(unsigned k=0;k<4;++k)assert(!ns_sftl_replay(&fresh,b*2048+pg*n*4+slot*4+k,r+k*4));
   ++slot;
  }}
 }
 assert(!ns_sftl_finish(&fresh));
 for(unsigned k=0;k<4;++k){uint32_t v;struct ns_address ad;unsigned slot=0;while(lb[slot]!=k)++slot;
  assert(!ns_sftl_lookup(&fresh,k,&v)&&!ns_vfl_address(&fresh,v,&ad));
  assert(media[ad.block-7][ad.ce*2+ad.cau][ad.page][ad.sector*1024]==seq*65537u+slot*1024);
 }
}
static unsigned fill(struct source *s,unsigned mask,unsigned tail,int fragmented){
 assert(!setup(s,0));masks[7]=mask;w.active=0;
 struct ns_write_io io={s,inspect,program,0,usec};assert(!ns_append_open(&a,&w,&m,states,leases,sizeof leases,&io,stage,sr,scratch,rr,1000000,0));
 unsigned pages=__builtin_popcount(mask)*128-tail/4;
 if(fragmented){lb[0]=0;lb[1]=2;lb[2]=1;lb[3]=3;}else for(unsigned i=0;i<4;++i)lb[i]=i;
 for(unsigned seq=1;seq<=pages;++seq)assert(!put(seq));
 return pages;
}
static int close_block(unsigned tail,unsigned cap,unsigned budget){return ns_write_close(&w,&closed,tail,entries,cap,summary,summary_record,budget);}
int main(void){
 for(unsigned mask=1;mask<=15;++mask){
  for(unsigned tail=4;tail<=8;tail+=4){if(tail==8&&__builtin_popcount(mask)<2)continue;
   struct source s={0};unsigned pages=fill(&s,mask,tail,tail==8);
   uint32_t before_gen=w.gen,before_key=w.key;assert(!close_block(tail,2048,1000000));
   assert(closed.phase==4&&closed.programs==tail/4&&closed.data_sectors==pages*4&&w.closed&&!w.poisoned&&m.ready);
   assert(w.gen==before_gen&&w.key==before_key&&closed.statistics[3]==closed.entries);
   assert(ns_write_put(&w,0,9,data)==NS_NOT_READY&&ns_append_page(&a,lb,ep,data)==NS_NOT_READY);
   recover(pages);assert(!ns_append_rotate(&a)&&!w.closed&&w.block==8);assert(!put(pages+1));recover(pages+1);
  }
 }
 for(unsigned fault=0;fault<8;++fault){
  struct source s={0};unsigned pages=fill(&s,15,8,1);unsigned before=s.programs;
  if(fault==0)media[0][0][0][4096]=3;
  if(fault==1)media[0][3][127][0]=0;
  if(fault==2)s.fail_at=before+1;
  if(fault==3)s.fail_at=before+2;
  if(fault==4)s.no_summary=1;
  if(fault==5)s.corrupt_summary=1;
  if(fault==6)s.late_timeout=1;
  int rc=close_block(8,2048,fault==7?1:1000000);assert(rc);
  if(fault<2||fault==7){assert(m.ready&&!w.poisoned&&s.programs==before);}
  else{assert(!m.ready&&w.poisoned&&closed.poisoned&&ns_append_rotate(&a)==NS_WRITE_POISONED);}
  if(fault>=2)recover(pages); /* Complete data survive each metadata cut point. */
 }
 struct source s={0};fill(&s,15,8,1);assert(close_block(8,1,1000000)==NS_SPACE&&m.ready&&!w.poisoned);
 assert(ns_write_close(&w,&closed,8,entries,2048,stage,summary_record,1000000)==NS_ARGUMENT);
 assert(close_block(4,2048,1000000)==NS_NOT_READY); /* Never leave a gap. */
 puts("PASS block close MODEL: all15bank masks,4/8sector full tails, fragmented and sequential records, source weave unchanged, closed writer guards/rotation; malformed/source/tail/space/alias/time/no-op/first-and-second-tail-failure controls and fresh data replay");
}
