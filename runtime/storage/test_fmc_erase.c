#include "fmc_erase.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct model {uint32_t r[1024],commands[16],count,writes,ticks,stuck,frozen,status,init_count,init_fail;};
static uint32_t read_reg(void *p,uint32_t o){struct model *m=p;if(o==0x48)return m->stuck?0:0xffffffffu;if(o==0x4c)return m->status;return m->r[o/4];}
static void write_reg(void *p,uint32_t o,uint32_t v){struct model *m=p;m->r[o/4]=v;++m->writes;if(o==8){assert(m->count<16);m->commands[m->count++]=v;}}
static uint32_t usec(void *p){struct model *m=p;return m->frozen?0:(m->ticks+=1000);}
static int init(void *p){struct model *m=p;++m->init_count;return !m->init_fail;}
static int run(struct model *m,struct ns_fmc_erase *w,unsigned ce,uint32_t row){struct ns_fmc_io io={m,read_reg,write_reg,usec,0,0};return ns_fmc_erase_block(&io,w,ce,row,init);}
int main(void){
 const uint32_t ready[]={0x40,0x50,0x60,0x62};
 for(unsigned variant=0;variant<4;++variant)for(unsigned ce=0;ce<2;++ce)for(unsigned cau=0;cau<2;++cau){struct model m={.status=ready[variant]};struct ns_fmc_erase w={0};
  assert(!run(&m,&w,ce,cau<<19|7<<7)&&w.phase==6&&w.completed==1&&!w.poisoned&&m.init_count==1);
  const uint32_t commands[]={0x6a,0xd7,0x77,0x7d,0x77};assert(m.count==5&&!memcmp(commands,m.commands,sizeof commands));
  assert(m.r[0x0c/4]==(cau<<19|7<<7)&&m.r[0x2c/4]==3&&m.r[0]==1);
  assert(w.status==ready[variant]&&w.last_ce==ce&&w.last_row==(cau<<19|7<<7));
 }
 for(unsigned fault=0;fault<6;++fault){struct model m={.status=0x62};struct ns_fmc_erase w={0};
  if(fault<2){m.stuck=1;m.frozen=fault==1;}if(fault==2)m.status=0x63;if(fault==3)m.status=0;if(fault==4)m.init_fail=1;if(fault==5)m.status=0x51;
  assert(run(&m,&w,0,7<<7)<0&&w.poisoned);unsigned n=m.writes;assert(run(&m,&w,0,7<<7)==-14&&m.writes==n);
 }
 for(unsigned mode=0;mode<4;++mode){struct model m={0};struct ns_fmc_erase w={0};
  assert(run(&m,&w,mode==0?2:0,mode==1?0:mode==2?2084<<7:(7<<7)|1)==NS_ARGUMENT&&!m.writes);
 }
 puts("PASS erase controller MODEL: exact6A/address4/D7/77/7D/status/77 sequence; CE/CAU/boot/reserved/page guards, passive bring-up, status/event/frozen-clock errors and poisoned retry");
}
