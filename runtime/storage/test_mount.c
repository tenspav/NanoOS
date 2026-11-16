#include "mount.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct source {unsigned masked,old_context,broken_tree,hole,conflict,bad_read,newer_outside,reserved_map,bad_boundary,bad_states,second_open,torn_block,torn_bank,torn_page,kind,prelude,after_tail,second_torn;uint32_t ticks;};
static const unsigned section[48]={1,1,1,1,2,2,2,2,3,3,3,3,3,3,3,3,
 5,5,5,5,5,5,5,5,5,5,5,5,6,6,6,6,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4};
static uint32_t clock_read(void *p){struct source *s=p;return s->ticks+=100;}
static int context(void *opaque,unsigned b,unsigned p,unsigned view,unsigned count,void *out,uint32_t *r){
 struct source *s=opaque;assert((b==2086||b==2087)&&p<=5&&view==1&&count==4);
 memset(out,255,16384);memset(r,255,64);
 r[0]=p==0||p==5?0x00ffff20:0x20|(p-1)<<8;r[1]=0xfffffff0u-(b==2086?0:6)-p;r[2]=r[3]=0;
 if(p==0||p==5)((uint32_t *)out)[3]=s->bad_boundary?0x08250825:0x08240824;
 if(s->old_context&&b==2087&&p==2)r[0]=0;
 /* Reserved tail deliberately looks non-bad in the bank arrays, as on
  * hardware: it must never be sent to the ordinary row callback. */
 if(p>=1&&p<=4){uint16_t *v=out;for(unsigned k=0;k<2088;++k)v[k]=((k>=8&&k<2084)||(s->masked&&k==3&&p==3))?0xbffe:0;}
 return 0;
}
static int inspect(void *opaque,uint32_t ce,uint32_t row,void *out,uint32_t *r){
 struct source *s=opaque;unsigned b=(row>>7)&4095,p=row&127,bank=ce*2+(row>>19);
 assert(b<8&&bank<4);uint32_t *d=out;
 memset(out,255,16384);memset(r,255,64);
 if(s->torn_block&&(b==s->torn_block||(s->second_torn&&b==5))&&bank==s->torn_bank&&p==s->torn_page){
  d[0]=0;
  if(s->kind==1)return -2;
  if(s->kind==2){r[0]=410u<<16|1;r[1]=9;r[2]=20;r[3]=9;return 0;}
  if(s->kind==3)return 0;
  if(s->kind==4){for(unsigned k=0;k<16;++k)r[k]=0;return 0;}
  return -1;
 }
 if(b==1&&((s->prelude&&p==0&&bank==0)||(s->after_tail&&p==1&&bank==0))){
  for(unsigned k=0;k<4;++k){unsigned lba=20+k;
   r[k*4]=(400+k)<<16|1;r[k*4+1]=9;r[k*4+2]=lba;r[k*4+3]=9;d[k*1024]=0xaa000000u|lba;
  }return 0;
 }
 if(s->bad_read&&b==1){memset(out,0,16384);return -2;}
 if(b==3){
  unsigned n=s->masked?3:4,slot=bank;if(s->masked){assert(bank!=2);if(bank==3)slot=2;}
  unsigned first=(p*n+slot)*4;if(first>=48)return -2;
  for(unsigned k=0;k<4;++k){unsigned index=first+k;uint32_t *x=d+k*1024;
   memset(x,0,4096);r[k*4]=(100+index)<<16|section[index]<<8|0x1f;r[k*4+1]=9;r[k*4+2]=0;r[k*4+3]=0xffffffff;
   if(index==0){x[0]=2;x[1]=3;x[2]=6;}
   if(index>=8&&index<16){for(unsigned j=0;j<512;++j){unsigned blk=(index-8)*512+j;
    x[j*2]=blk==0||blk==7?8:blk==1?1:blk==5?(s->second_open?4:1):blk==2?3:blk==3?(s->bad_states?1:7):blk==4?4:blk==6?(s->bad_states?9:7):9;
    x[j*2+1]=0x12340000u|blk;}}
   if(index>=32){memset(x,255,4096);if(index==32){x[0]=s->broken_tree?1:0;x[1]=0xfffffff0;x[2]=(s->reserved_map?2084:2)*2048;x[3]=128;}}
  }
  return 0;
 }
 if(b==2&&p<8){
  for(unsigned k=0;k<4;++k){unsigned lba=p*16+bank*4+k;
   r[k*4]=(lba+1)<<16|1;r[k*4+1]=9;r[k*4+2]=lba;r[k*4+3]=9;
   for(unsigned j=0;j<1024;++j)d[k*1024+j]=lba*1000+j;
  }return 0;
 }
 if((b==4&&bank==0&&(p==0||(s->hole&&p==2)))||(b==6&&s->newer_outside)||(b==5&&s->second_open&&bank==0&&p==0)){
  for(unsigned k=0;k<4;++k){unsigned lba=(b==5?10:3)+k;
   r[k*4]=(s->conflict?200:200+k+(b==5?4:0))<<16|1;r[k*4+1]=9;r[k*4+2]=lba;r[k*4+3]=9;
   for(unsigned j=0;j<1024;++j)d[k*1024+j]=0xab000000u|lba*1000+j;
  }return 0;
 }
 return -2;
}
static struct ns_mount m;static uint8_t mask[2088],state[2088];
static uint32_t fg[2088],fk[2088],data[4096],meta[16];
static struct ns_run runs[8192];static struct ns_update updates[1024];
static int mount(struct source *s,uint32_t budget){
 struct ns_mount_io io={s,inspect,context,clock_read};
 return ns_mount_n31(&m,&io,mask,state,fg,fk,data,meta,runs,8192,updates,1024,budget);
}
static int recover(struct source *s){
 struct ns_mount_io io={s,inspect,context,clock_read};
 return ns_mount_n31_recover(&m,&io,mask,state,fg,fk,data,meta,runs,8192,updates,1024,1000000);
}
int main(void){
 for(unsigned bits=0;bits<8;++bits){struct source s={.masked=bits&1,.old_context=bits&2,.second_open=bits&4};
  assert(!mount(&s,1000000));assert(m.map.ready&&m.root==3&&m.open_block==4&&m.map.nupdates==(s.second_open?8u:4u)&&m.map.sectors==128);
  assert(m.open_count==(s.second_open?2u:1u)&&m.base_head[2]==6&&m.hint_state_words[0]==7);
  assert(m.context_block==(s.old_context?2086:2087));assert(m.ready_block==1);
  struct ns_disk disk;uint32_t cache[4096],records[16],out[1024];
  assert(!ns_disk_init(&disk,&m.map,ns_mount_page_read,&m,cache,records));
  for(unsigned lba=0;lba<128;++lba){assert(!ns_disk_read(&disk,lba,out));
   for(unsigned j=0;j<1024;++j)assert(out[j]==(((lba>=3&&lba<=6)||(s.second_open&&lba>=10&&lba<=13))?(0xab000000u|lba*1000+j):lba*1000+j));}
 }
 struct source broken={.broken_tree=1};assert(mount(&broken,1000000)==NS_FORMAT&&!m.map.ready);
 struct source hole={.hole=1};assert(mount(&hole,1000000)==NS_FORMAT&&!m.map.ready);
 struct source conflict={.conflict=1};assert(mount(&conflict,1000000)==NS_CONFLICT&&!m.map.ready);
 struct source bad={.bad_read=1};assert(mount(&bad,1000000)==NS_IO&&!m.map.ready);
 struct source outside={.newer_outside=1};assert(mount(&outside,1000000)==NS_INCOMPLETE&&!m.map.ready);
 struct source reserved={.reserved_map=1};assert(mount(&reserved,1000000)==NS_FORMAT&&!m.map.ready);
 struct source boundary={.bad_boundary=1};assert(mount(&boundary,1000000)==NS_INCOMPLETE&&!m.map.ready);
 struct source sb={.bad_states=1};assert(mount(&sb,1000000)==NS_FORMAT&&!m.map.ready);
 assert(m.detail==0x10000109&&m.root==3&&m.map.sectors==128);
 assert(m.root_state_words[0]==1&&m.root_state_words[1]==0x12340003);
 assert(m.hint_state_words[0]==9&&m.hint_state_words[1]==0x12340006&&m.base_head[2]==6);
 struct source timeout={0};assert(mount(&timeout,100)==NS_TIMEOUT&&!m.map.ready);
 for(unsigned kind=1;kind<=3;++kind)for(unsigned prefix=0;prefix<=1;++prefix){
  struct source t={.torn_block=1,.torn_bank=prefix?1:0,.kind=kind,.prelude=prefix};assert(mount(&t,1000000)!=0&&!m.map.ready);
  t.ticks=0;assert(!recover(&t)&&m.map.ready&&m.recovered_pages==1&&m.quarantine_block==1&&state[1]==9&&m.ready_block!=1);
  assert(m.map.read_only&&ns_sftl_ready_block(&m.map,5,1)==NS_NOT_READY);
  assert(ns_sftl_replay(&m.map,2048,meta)==NS_NOT_READY&&m.map.ready);
  assert(m.bad_ce==0&&m.bad_row==((prefix?1u:0u)<<19|1u<<7));
  uint32_t v;assert(!ns_sftl_lookup(&m.map,20,&v));assert(v==(prefix?2048u:4096u+20u));
  if(prefix)assert(v==1u*2048u);
 }
 struct source transport={.torn_block=1,.kind=5};assert(recover(&transport)==NS_IO&&!m.map.ready);
 struct source referenced={.torn_block=2,.kind=1};assert(recover(&referenced)!=0&&!m.map.ready);
 struct source cpbad={.torn_block=3,.kind=1};assert(recover(&cpbad)!=0&&!m.map.ready);
 struct source openbad={.torn_block=4,.kind=1};assert(recover(&openbad)==NS_IO&&!m.map.ready);
 struct source later={.torn_block=1,.kind=1,.after_tail=1};assert(recover(&later)==NS_FORMAT&&!m.map.ready);
 struct source multiple={.torn_block=1,.kind=1,.second_torn=1};assert(recover(&multiple)==NS_IO&&!m.map.ready);
 struct source malformed={.torn_block=1,.kind=4};assert(recover(&malformed)==NS_FORMAT&&!m.map.ready);
 puts("PASS mount: redundant contexts, masked banks, complete checkpoint, replay, cached sector data; strict gaps/conflicts/errors/timeouts fail closed; opt-in single READY-tail quarantine, valid-prefix replay; referenced/open/root/transport/multiple/gap/unknown corruption rejected");
}
