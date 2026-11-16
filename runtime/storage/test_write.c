#include "write.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t media[4][128][4112];
struct source {unsigned programs,erases,fail_read,fail_program,fail_erase,tamper;uint32_t ticks;};
static uint32_t clock_us(void *p){struct source *s=p;return s->ticks+=10;}
static unsigned bank_row(uint32_t ce,uint32_t row){assert(((row>>7)&4095)==7);return ce*2+(row>>19);}
static int inspect(void *p,uint32_t ce,uint32_t row,void *out,uint32_t *r){
 struct source *s=p;if(s->fail_read)return -1;
 unsigned bank=bank_row(ce,row),pg=row&127;
 memcpy(out,media[bank][pg],16384);memcpy(r,media[bank][pg]+4096,64);
 if(s->programs&&s->tamper)((uint32_t *)out)[100]^=1;
 return media[bank][pg][4096]==0xffffffffu?-2:0;
}
static int program(void *p,uint32_t ce,uint32_t row,const void *data,const uint32_t *r){
 struct source *s=p;unsigned bank=bank_row(ce,row),pg=row&127;++s->programs;
 for(unsigned i=0;i<4112;++i)assert(media[bank][pg][i]==0xffffffffu);
 memcpy(media[bank][pg],data,s->fail_program?8192:16384);memcpy(media[bank][pg]+4096,r,64);
 return s->fail_program?-1:0;
}
static int erase(void *p,uint32_t ce,uint32_t row){
 struct source *s=p;unsigned bank=bank_row(ce,row);assert(!(row&127));++s->erases;
 if(s->fail_erase)return -1;
 memset(media[bank],255,sizeof media[bank]);return 0;
}
static struct ns_sftl map;static struct ns_run runs[8];static struct ns_update updates[64];
static uint8_t masks[8],states[8];static struct ns_writer writer;
static uint32_t stage[4096],sr[16],scratch[4096],rr[16],data[1024];
static void setup(unsigned mask){
 memset(media,255,sizeof media);memset(masks,15,sizeof masks);memset(states,9,sizeof states);
 masks[7]=mask;states[1]=states[7]=1;states[4]=8;states[5]=4;states[6]=7;
 assert(!ns_sftl_init(&map,8,128,masks,runs,8,updates,64,9,65534));
 uint32_t node[1024];memset(node,255,sizeof node);node[0]=0;node[1]=0xfffffff0;node[2]=0x7fffff;node[3]=128;
 assert(!ns_sftl_node(&map,node)&&!ns_sftl_finish(&map));
 for(unsigned i=0;i<1024;++i)data[i]=0xaa000000u+i;
}
static int prepare(struct source *s,unsigned disabled){
 struct ns_write_io io={s,inspect,disabled?0:program,disabled?0:erase,clock_us};
 return ns_write_prepare(&writer,&map,states,&io,stage,sr,scratch,rr,1000000);
}
int main(void){
 setup(15);struct source recovered={0};map.read_only=1;assert(prepare(&recovered,0)==NS_NOT_READY&&!writer.active&&!recovered.programs&&!recovered.erases&&!writer.reads);
 for(unsigned mask=11;mask<=15;mask+=4){
  setup(mask);struct source s={0};assert(!prepare(&s,0));
  assert(writer.block==7&&writer.reads==(mask==15?512u:384u)&&!s.programs&&!s.erases);
  assert(!ns_write_erase_candidate(&writer)&&s.erases==(mask==15?4u:3u));
  assert(!ns_write_put(&writer,5,9,data));data[0]^=1;assert(!ns_write_put(&writer,6,9,data));
  assert(!ns_write_flush(&writer)&&s.programs==1&&map.ready&&map.nupdates==4);
  assert(writer.gen==10&&writer.key==2&&writer.next==4&&!writer.pending&&!writer.poisoned);
  assert(media[0][0][4096]==0xffff0001u&&media[0][0][4100]==1&&media[0][0][4101]==10);
  assert(media[0][0][4096+6]==6&&media[0][0][4096+10]==6&&media[0][0][4096+14]==6);
  uint32_t vba=0;assert(!ns_sftl_lookup(&map,5,&vba)&&vba==7*2048);
  assert(!ns_sftl_lookup(&map,6,&vba)&&vba==7*2048+3);
  assert(ns_write_erase_candidate(&writer)==NS_NOT_READY);
  for(unsigned i=0;i<4;++i)assert(!ns_write_put(&writer,20+i,10,data));
  assert(ns_write_put(&writer,25,10,data)==NS_SPACE);
  assert(!ns_write_flush(&writer)&&s.programs==2&&writer.next==8);
  /* Mask11 drops bank2: second page still targets bank1, with row bit19. */
  assert(media[1][0][4096+2]==20);
  for(unsigned i=0;i<4;++i)assert(!ns_write_put(&writer,30+i,10,data));
  assert(!ns_write_flush(&writer)&&s.programs==3);
  assert(media[mask==15?2:3][0][4096+2]==30);
 }
 setup(15);struct source ro={0};assert(!prepare(&ro,1));
 assert(ns_write_put(&writer,0,9,data)==NS_UNSUPPORTED&&ns_write_erase_candidate(&writer)==NS_UNSUPPORTED&&!ro.programs&&!ro.erases);
 struct ns_write_io alias_io={&ro,inspect,program,erase,clock_us};
 assert(ns_write_prepare(&writer,&map,states,&alias_io,stage,sr,stage+1,rr,1000)==NS_ARGUMENT&&!writer.reads);
 setup(15);struct source dirty={0};media[3][127][4096]=0;assert(prepare(&dirty,0)==NS_NOT_BLANK&&!writer.active&&!dirty.programs);
 setup(15);struct source bad={.fail_read=1};assert(prepare(&bad,0)==NS_IO&&!writer.active);
 setup(15);struct source full={0};assert(!prepare(&full,0));map.update_capacity=2;
 assert(!ns_write_put(&writer,1,9,data)&&ns_write_flush(&writer)==NS_SPACE&&!full.programs&&map.ready);
 for(unsigned fault=0;fault<2;++fault){
  setup(15);struct source s={.fail_program=fault==0,.tamper=fault==1};assert(!prepare(&s,0));
  assert(!ns_write_put(&writer,1,9,data));assert(ns_write_flush(&writer)==(fault?NS_VERIFY:NS_IO));
  assert(!map.ready&&writer.poisoned&&map.nupdates==0&&ns_write_flush(&writer)==NS_WRITE_POISONED);
 }
 setup(15);struct source er={.fail_erase=1};assert(!prepare(&er,0));
 assert(ns_write_erase_candidate(&writer)==NS_IO&&writer.poisoned&&!map.ready);
 setup(15);struct source live={0};runs[0].vba=7*2048;assert(!ns_sftl_finish(&map));
 assert(prepare(&live,0)==NS_SPACE&&!writer.active&&!writer.reads);
 setup(15);struct source changed={0};assert(!prepare(&changed,0));++map.nupdates;
 assert(ns_write_put(&writer,1,9,data)==NS_NOT_READY&&!changed.programs);
 puts("PASS write MODEL: guarded READY blank proof, all banks, masked stripes, erase verification, buffered/full/partial pages, epoch/key wrap, live-reference/capacity/read-only guards; uncertain program/erase poisons mount");
}
