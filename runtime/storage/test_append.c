#include "append.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t media[3][4][128][4112];
struct source {unsigned programs,fail_at;uint32_t ticks;};
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
int main(void){
 struct source s={0};assert(!setup(&s,2));
 for(unsigned seq=1;seq<=6;++seq){assert(!put(seq));verify(seq);assert(m.nupdates<=8&&m.ready&&!a.poisoned);}
 assert(a.blocks==3&&a.rotations==2&&a.pages==6&&s.programs==6&&w.block==9&&w.next==8);
 assert(leases[7]==10&&leases[8]==10&&leases[9]==10&&states[7]==1);
 assert(put(7)==NS_SPACE&&s.programs==6&&m.ready);remount(6);
 /* Real capacity, not an artificial boundary: all masks and all page rows.
  * Last virtual page is reserved on each superblock and remains all FF. */
 s=(struct source){0};assert(!setup(&s,0));
 unsigned total=511+127+255;
 for(unsigned seq=1;seq<=total;++seq)assert(!put(seq));
 assert(a.pages==total&&a.rotations==2&&put(total+1)==NS_SPACE&&s.programs==total);remount(total);
 for(unsigned b=7;b<10;++b){unsigned last=3;while(!(masks[b]>>last&1))--last;
  for(unsigned i=0;i<4112;++i)assert(media[b-7][last][127][i]==0xffffffffu);
 }
 /* Never reserve/mutate another block for invalid input or stale mount. */
 s=(struct source){0};assert(!setup(&s,1));assert(!put(1));
 ep[3]=100;assert(put(2)==NS_ARGUMENT&&s.programs==1&&a.blocks==1&&!w.pending);ep[3]=9;
 lb[3]=lb[2];assert(put(2)==NS_ARGUMENT&&s.programs==1);lb[3]=3;
 assert(ns_append_page(&a,lb,ep,stage)==NS_ARGUMENT&&s.programs==1);
 ++m.nupdates;assert(put(2)==NS_NOT_READY&&s.programs==1);
 s=(struct source){.fail_at=2};assert(!setup(&s,1));assert(!put(1));
 assert(put(2)==NS_IO&&a.poisoned&&!m.ready&&s.programs==2);
 assert(put(3)==NS_WRITE_POISONED&&s.programs==2);
 s=(struct source){0};assert(!setup(&s,1));assert(!put(1));media[1][3][127][0]=0;
 assert(put(2)==NS_NOT_BLANK&&s.programs==1&&m.ready&&!a.poisoned&&a.blocks==1);verify(1);
 s=(struct source){0};assert(!setup(&s,1));assert(!put(1));m.update_capacity=4;
 assert(put(2)==NS_SPACE&&s.programs==1&&m.ready&&a.blocks==1);verify(1);
 s=(struct source){0};assert(!setup(&s,2));w.budget=1;
 assert(put(1)==NS_TIMEOUT&&!w.pending&&!s.programs&&m.ready&&!a.poisoned);
 w.budget=1000000;assert(!put(1));verify(1);
 s=(struct source){0};assert(!setup(&s,0));assert(ns_append_rotate(&a)==NS_NOT_READY);
 assert(!put(1)&&!ns_append_rotate(&a)&&w.block==8&&w.next==0);
 assert(!put(2));remount(2);
 struct ns_write_io io={&s,inspect,program,0,usec};
 assert(ns_append_open(&a,&w,&m,states,states,sizeof states,&io,stage,sr,scratch,rr,100000,1)==NS_ARGUMENT);
 puts("PASS append MODEL: full natural bank-masked capacities, reserved-tail preservation, three-block rotation, bounded RAM compaction, independent full replay; no-space/invalid/stale/dirty/uncertain-mutation guards");
}
