#include "reclaim.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t media[2][4][128][4112];
struct source {unsigned programs,erases,fail_erase,no_erase,damage_survivor;uint32_t ticks;};
static uint32_t usec(void *p){return ((struct source *)p)->ticks+=10;}
static uint32_t *slot(unsigned ce,uint32_t row){unsigned b=(row>>7)&4095,bank=ce*2+(row>>19);assert((b==4||b==5)&&bank<4);return media[b-4][bank][row&127];}
static int inspect(void *p,uint32_t ce,uint32_t row,void *out,uint32_t *r){
 (void)p;uint32_t *s=slot(ce,row);memcpy(out,s,16384);memcpy(r,s+4096,64);return s[4096]==0xffffffffu?-2:0;
}
static int program(void *p,uint32_t ce,uint32_t row,const void *data,const uint32_t *r){
 struct source *s=p;uint32_t *dst=slot(ce,row);for(unsigned i=0;i<4112;++i)assert(dst[i]==0xffffffffu);
 ++s->programs;memcpy(dst,data,16384);memcpy(dst+4096,r,64);return 0;
}
static int erase(void *p,uint32_t ce,uint32_t row){
 struct source *s=p;++s->erases;assert(((row>>7)&4095)==4&&!(row&127));
 if(s->fail_erase)return -1;
 if(!s->no_erase)memset(slot(ce,row),255,sizeof media[0][0]);
 if(s->damage_survivor&&s->erases==1)media[1][0][0][0]^=1;
 return 0;
}
static struct ns_sftl map;static struct ns_run runs[8];static struct ns_update updates[64];
static uint8_t masks[8],states[8];static struct ns_writer w;static struct ns_page_proof a,b;
static struct ns_reclaim gc;static uint32_t stage[4096],sr[16],scratch[4096],rr[16],payload[1024];
static struct ns_write_io io;
static void init_map(struct ns_sftl *m,struct ns_run *r,struct ns_update *u){
 assert(!ns_sftl_init(m,8,128,masks,r,8,u,64,9,100));uint32_t node[1024];memset(node,255,sizeof node);
 node[0]=0;node[1]=0xfffffff0;node[2]=0x7fffff;node[3]=128;assert(!ns_sftl_node(m,node)&&!ns_sftl_finish(m));
}
static void write_page(uint32_t salt){
 for(unsigned k=0;k<4;++k){for(unsigned j=0;j<1024;++j)payload[j]=salt+k*1024+j;assert(!ns_write_put(&w,10+k,9,payload));}
 assert(!ns_write_flush(&w));
}
static void setup(struct source *s,unsigned mask){
 memset(media,255,sizeof media);memset(masks,15,sizeof masks);memset(states,8,sizeof states);states[4]=states[5]=1;masks[4]=mask;
 init_map(&map,runs,updates);io=(struct ns_write_io){s,inspect,program,erase,usec};
 assert(!ns_write_prepare(&w,&map,states,&io,stage,sr,scratch,rr,1000000));assert(w.block==4);
 write_page(0xa1000000);assert(!ns_write_seal_page(&w,&a));
 assert(!ns_write_prepare(&w,&map,states,&io,stage,sr,scratch,rr,1000000)&&w.block==5);
 write_page(0xb2000000);assert(!ns_write_seal_page(&w,&b));
}
static int reclaim(void){return ns_reclaim_page(&gc,&map,states,&io,&a,&b,scratch,rr,1000000);}
int main(void){
 for(unsigned mask=11;mask<=15;mask+=4){struct source s={0};setup(&s,mask);
  assert(reclaim()==NS_CONFLICT&&!s.erases&&map.ready);
  assert(!ns_sftl_compact(&map)&&map.nupdates==4);
  assert(!reclaim()&&gc.phase==5&&gc.programmed_seen==1&&!gc.poisoned&&map.ready);
  assert(s.erases==(mask==15?4u:3u));
  for(unsigned bank=0;bank<4;++bank)if(mask>>bank&1)for(unsigned pg=0;pg<128;++pg)for(unsigned k=0;k<4112;++k)assert(media[0][bank][pg][k]==0xffffffffu);
  assert(!ns_write_resume_compacted(&w));write_page(0xc3000000);assert(s.programs==3&&w.next==8);
  struct ns_sftl fresh;struct ns_run fr[8];struct ns_update fu[64];init_map(&fresh,fr,fu);
  for(unsigned bank=0;bank<2;++bank)for(unsigned k=0;k<4;++k)assert(!ns_sftl_replay(&fresh,5*2048+bank*4+k,media[1][bank][0]+4096+k*4));
  assert(!ns_sftl_finish(&fresh));for(unsigned k=0;k<4;++k){uint32_t vba;assert(!ns_sftl_lookup(&fresh,10+k,&vba)&&vba==5*2048+4+k);}
  assert(ns_write_seal_page(&w,&b)==NS_NOT_READY);
 }
 for(unsigned fault=0;fault<9;++fault){struct source s={0};setup(&s,15);assert(!ns_sftl_compact(&map));
  if(fault==0)media[1][0][0][17]^=1;
  if(fault==1)media[0][3][127][10]=0;
  if(fault==2)a.digest[0]^=1;
  if(fault==3)++map.checkpoint_key;
  if(fault==4)states[4]=4;
  if(fault==5)s.fail_erase=1;
  if(fault==6)s.no_erase=1;
  if(fault==7)s.damage_survivor=1;
  if(fault==8){runs[0].vba=4*2048;assert(!ns_sftl_finish(&map));}
  assert(reclaim()<0);
  if(fault==5||fault==6||fault==7)assert(!map.ready&&gc.poisoned&&s.erases);
  else assert(map.ready&&!s.erases);
 }
 struct source s={0};setup(&s,15);assert(!ns_sftl_compact(&map));
 assert(ns_reclaim_page(&gc,&map,states,&io,&a,&b,(uint32_t *)&a,rr,1000000)==NS_ARGUMENT&&!s.erases);
 assert(ns_reclaim_page(&gc,&map,states,&io,&a,&b,scratch,rr,1)==NS_TIMEOUT&&!s.erases&&map.ready);
 puts("PASS reclaim MODEL: newer verified replacement, RAM compaction, full owned-page/blank scan, programmed-block erase and surviving-copy recheck; fresh replay without old block; live/base/unknown/corrupt/no-op/error/alias/time guards");
}
