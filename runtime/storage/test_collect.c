#include "collect.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t media[3][4][128][4112];
struct source {unsigned programs,fail_at,erases,no_erase,fail_erase,corrupt_survivor;uint32_t ticks;};
static uint32_t usec(void *p){struct source *s=p;return s->ticks+=10;}
static int inspect(void *p,uint32_t ce,uint32_t row,void *data,uint32_t *r){
 (void)p;unsigned block=((row>>7)&4095)-7,bank=ce*2+(row>>19),page=row&127;
 assert(block<3&&bank<4);memcpy(data,media[block][bank][page],16384);memcpy(r,media[block][bank][page]+4096,64);
 return r[0]==0xffffffffu?-2:0;
}
static int program(void *p,uint32_t ce,uint32_t row,const void *data,const uint32_t *r){
 struct source *s=p;unsigned block=((row>>7)&4095)-7,bank=ce*2+(row>>19),page=row&127;
 assert(block<3&&bank<4);for(unsigned i=0;i<4112;++i)assert(media[block][bank][page][i]==0xffffffffu);
 ++s->programs;memcpy(media[block][bank][page],data,16384);memcpy(media[block][bank][page]+4096,r,64);
 return s->fail_at==s->programs?-1:0;
}
static int erase(void *p,uint32_t ce,uint32_t row){
 struct source *s=p;unsigned block=((row>>7)&4095)-7,bank=ce*2+(row>>19);assert(!(row&127));++s->erases;
 if(s->fail_erase)return -1;
 if(!s->no_erase)memset(media[block][bank],255,sizeof media[block][bank]);
 if(s->corrupt_survivor&&s->erases==1)media[1][3][0][1]^=1;
 return 0;
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
 init_map(&m,runs,updates,12);struct ns_write_io io={s,inspect,program,erase,usec};
 return ns_append_open(&a,&w,&m,states,leases,sizeof leases,&io,stage,sr,scratch,rr,1000000,limit);
}
static int put(unsigned seq){for(unsigned i=0;i<4096;++i)data[i]=seq*65537u+i;return ns_append_page(&a,lb,ep,data);}
static void verify(unsigned seq){
 for(unsigned k=0;k<4;++k){uint32_t v;struct ns_address ad;assert(!ns_sftl_lookup(&m,k,&v)&&!ns_vfl_address(&m,v,&ad));
  assert(media[ad.block-7][ad.ce*2+ad.cau][ad.page][ad.sector*1024]==seq*65537u+k*1024);
 }
}
static void remount(unsigned seq){
 init_map(&fresh,fruns,fupdates,8192);
 for(unsigned b=7;b<10;++b){unsigned n=0;for(unsigned bank=0;bank<4;++bank)n+=(masks[b]>>bank)&1u;
  for(unsigned page=0;page<128;++page){unsigned slot=0;for(unsigned bank=0;bank<4;++bank)if(masks[b]>>bank&1){
   const uint32_t *r=media[b-7][bank][page]+4096;
   if(r[0]!=0xffffffffu)for(unsigned k=0;k<4;++k)assert(!ns_sftl_replay(&fresh,b*2048+page*n*4+slot*4+k,r+k*4));
   ++slot;
  }}
 }
 assert(!ns_sftl_finish(&fresh));
 for(unsigned k=0;k<4;++k){uint32_t v,a;assert(!ns_sftl_lookup(&fresh,k,&v)&&!ns_sftl_lookup(&m,k,&a)&&v==a);}
 verify(seq);
}
static struct ns_collect gc;static struct ns_survivor proof[2044];
static uint32_t src[4096],src_record[16],saved[8][1024];
static struct ns_evacuate evacuation;
static void snapshot(unsigned save){
 struct ns_disk disk;assert(!ns_disk_init(&disk,&m,inspect,w.io.opaque,scratch,rr));
 for(unsigned i=0;i<8;++i){if(save)assert(!ns_disk_read(&disk,i,saved[i]));
  else{assert(!ns_disk_read(&disk,i,src));assert(!memcmp(src,saved[i],4096));}}
}
static int collect(unsigned block,unsigned cap){return ns_append_collect(&a,&gc,block,proof,cap,src,src_record,1000000);}
int main(void){
 struct source s={0};assert(!setup(&s,2));
 assert(!put(1)&&!put(2));
 assert(collect(7,2044)==NS_ARGUMENT&&!s.erases); /* Still current. */
 assert(!put(3)&&!put(4)&&w.block==8);
 assert(!ns_sftl_ready_block(&m,7,states[7])&&leases[7]==10); /* Reservation survives superseded RAM updates. */
 assert(!collect(7,2044)&&gc.phase==5&&gc.programmed==2&&gc.survivors==1&&gc.erases==4);
 assert(!a.poisoned&&m.ready&&leases[7]==1);verify(4);
 assert(!ns_append_rotate(&a)&&w.block==7&&w.next==0);
 assert(!put(5));remount(5);
 /* Whole natural-capacity victim and repeated collection/reuse. */
 s=(struct source){0};assert(!setup(&s,0));
 for(unsigned seq=1;seq<=512;++seq)assert(!put(seq));
 assert(w.block==8&&!collect(7,2044)&&gc.programmed==511&&gc.survivors==1&&s.erases==4);
 assert(!ns_append_rotate(&a)&&w.block==7&&!put(513));remount(513);
 assert(!collect(8,2044)&&s.erases==5&&gc.programmed==1);verify(513);
 /* Collection refuses a live old LBA, not just the newest four overwrites. */
 s=(struct source){0};assert(!setup(&s,1));assert(!put(1));
 for(unsigned i=0;i<4;++i)lb[i]=i+4;
 assert(!put(2));assert(collect(7,2044)==NS_CONFLICT&&!s.erases&&m.ready);
 for(unsigned i=0;i<4;++i)lb[i]=i;
 /* Unknown data after a hole, wrong epoch, replacement record mismatch,
  * missing workspace and changed checkpoint all refuse before erasing. */
 for(unsigned fault=0;fault<7;++fault){
  s=(struct source){0};assert(!setup(&s,1));assert(!put(1)&&!put(2));
  if(fault==0)media[0][0][127][0]=0;
  if(fault==1)media[0][0][0][4096+3]^=1;
  if(fault==2)media[1][3][0][4096]^=65536;
  if(fault==3)++m.checkpoint_key;
  if(fault==6){runs[0].vba=7*2048;assert(!ns_sftl_finish(&m));}
  int rc=fault==4?collect(7,0):fault==5?ns_append_collect(&a,&gc,7,proof,2044,stage,src_record,1000000):collect(7,2044);
  assert(rc&&s.erases==0&&m.ready);
 }
 /* Survivor capacity is checked before any mutation, including distinct
  * replacement pages rather than just repeated four-LBA test versions. */
 s=(struct source){0};assert(!setup(&s,2));assert(!put(1));
 for(unsigned i=0;i<4;++i)lb[i]=i+4;
 assert(!put(2)&&!put(3));for(unsigned i=0;i<4;++i)lb[i]=i;
 assert(!put(4)&&collect(7,1)==NS_SPACE&&!s.erases&&m.ready);
 assert(!collect(7,2044)&&gc.survivors==2);verify(4);
 for(unsigned fault=0;fault<3;++fault){
  s=(struct source){0};assert(!setup(&s,1));assert(!put(1)&&!put(2));
  s.no_erase=fault==0;s.fail_erase=fault==1;s.corrupt_survivor=fault==2;
  assert(collect(7,2044)&&s.erases&&gc.poisoned&&a.poisoned&&!m.ready);
  assert(put(3)==NS_WRITE_POISONED&&ns_append_rotate(&a)==NS_WRITE_POISONED);
 }
 s=(struct source){0};assert(!setup(&s,1));assert(!put(1)&&!put(2));
 assert(ns_append_collect(&a,&gc,7,proof,2044,src,src_record,1)==NS_TIMEOUT&&!s.erases&&m.ready);
 /* Partially live groups: stale neighbors MUST retain their current data,
  * not be resurrected from the victim's older version. */
 s=(struct source){0};assert(!setup(&s,0));assert(!put(1));
 for(unsigned i=0;i<4;++i)lb[i]=i+4;
 assert(!put(2)&&!ns_append_rotate(&a));
 lb[0]=0;lb[1]=1;lb[2]=4;lb[3]=5;assert(!put(3));snapshot(1);
 assert(saved[0][0]==3*65537u&&saved[1][0]==3*65537u+1024);
 assert(saved[2][0]==65537u+2048&&saved[3][0]==65537u+3072);
 assert(saved[4][0]==3*65537u+2048&&saved[5][0]==3*65537u+3072);
 assert(saved[6][0]==2*65537u+2048&&saved[7][0]==2*65537u+3072);
 assert(!ns_append_evacuate(&a,&evacuation,7,src,src_record,1000000));
 assert(evacuation.phase==2&&evacuation.copied==2&&evacuation.sectors==4&&!s.erases);snapshot(0);
 assert(!collect(7,2044)&&gc.survivors==2);snapshot(0);
 /* A full natural victim whose last page is still entirely live. Destination
  * begins empty, then all current sectors move before the erase proof. */
 for(unsigned i=0;i<4;++i)lb[i]=i;
 s=(struct source){0};assert(!setup(&s,0));
 for(unsigned seq=1;seq<=511;++seq)assert(!put(seq));
 assert(!ns_append_rotate(&a)&&w.block==8&&!w.next);
 assert(!ns_append_evacuate(&a,&evacuation,7,src,src_record,1000000));
 assert(evacuation.copied==1&&evacuation.sectors==4&&evacuation.scanned==512&&s.programs==512&&!s.erases);
 remount(511);assert(!collect(7,2044)&&gc.programmed==511&&s.erases==4);
 assert(!ns_append_rotate(&a)&&w.block==7&&!put(512));remount(512);
 /* Safe partial progress on no space/validation failure; uncertain copies
  * poison the map and never enter erase. */
 s=(struct source){.fail_at=2};assert(!setup(&s,0));assert(!put(1)&&!ns_append_rotate(&a));
 assert(ns_append_evacuate(&a,&evacuation,7,src,src_record,1000000)==NS_IO&&evacuation.poisoned&&a.poisoned&&!m.ready&&!s.erases);
 s=(struct source){0};assert(!setup(&s,1));assert(!put(1)&&!ns_append_rotate(&a));
 lb[0]=0;lb[1]=1;lb[2]=4;lb[3]=5;assert(!put(2));leases[9]=9;
 assert(ns_append_evacuate(&a,&evacuation,7,src,src_record,1000000)==NS_SPACE&&m.ready&&!a.poisoned&&!s.erases&&s.programs==2);
 for(unsigned i=0;i<4;++i)lb[i]=i;
 s=(struct source){0};assert(!setup(&s,0));assert(!put(1)&&!ns_append_rotate(&a));
 assert(ns_append_evacuate(&a,&evacuation,7,stage,src_record,1000000)==NS_ARGUMENT&&s.programs==1);
 assert(ns_append_evacuate(&a,&evacuation,7,src,src_record,1)==NS_TIMEOUT&&s.programs==1&&!s.erases&&m.ready);
 media[0][0][127][0]=0;
 assert(ns_append_evacuate(&a,&evacuation,7,src,src_record,1000000)==NS_FORMAT&&s.programs==2&&!s.erases&&m.ready);verify(1);
 puts("PASS collect MODEL: full511page superseded victim, partial-live RMW evacuation without stale resurrection, multiple verified survivors, repeated erase/reuse and independent replay; live/base/unknown/epoch/record/capacity/alias/time/no-op/error/corrupt-survivor guards");
}
