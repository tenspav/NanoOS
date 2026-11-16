#include "fmc_program.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct model {uint32_t r[1024],ticks,kicks,writes,cleans,phase_fault,verdict_fault,timeout,frozen,wrong_row,bad_bus;const void *ptr[16];unsigned count;};
static uint32_t bus(void *p,const void *v,uint32_t bytes){
 struct model *m=p;(void)bytes;if(m->bad_bus)return 0;
 for(unsigned i=0;i<m->count;++i)if(m->ptr[i]==v)return 0x9000000u+i*0x10000u;
 assert(m->count<16);m->ptr[m->count++]=v;return 0x9000000u+(m->count-1)*0x10000u;
}
static uint32_t rd(void *p,uint32_t off){struct model *m=p;return m->r[off/4];}
static void wr(void *p,uint32_t off,uint32_t v){
 struct model *m=p;++m->writes;
 if(off==0xc0c){m->r[off/4]&=~v;return;}m->r[off/4]=v;
 if(off!=0xc00)return;
 assert(v==0xfff5);++m->kicks;
 m->r[0xc0c/4]=m->timeout?0:m->phase_fault==m->kicks?4:1;
 m->r[0xc38/4]=m->kicks==1?0x1d8:0x8f0;
 if(m->kicks==1){assert(m->r[0xd08/4]==0x10000&&m->r[0xd10/4]==1);}
 else{
  assert(m->r[0xd04/4]==0x10000&&m->r[0xd08/4]==4096&&m->r[0xd10/4]==4&&m->r[0xd1c/4]==3);
  const uint32_t *ring=m->ptr[(m->r[0xd0c/4]-0x9000000u)/0x10000u];
  assert((ring[0]&0x80000000u)&&!ring[4]);m->r[0x0c/4]=m->wrong_row?0:ring[1];
  m->r[0xd00/4]=m->verdict_fault;
 }
}
static uint32_t clock_us(void *p){struct model *m=p;return m->frozen?7:(m->ticks+=100000);}
static void clean(void *p){++((struct model *)p)->cleans;}
static struct ns_fmc_program work;static uint32_t data[4096],record[16];
static int run(struct model *m,unsigned ce,uint32_t row){struct ns_fmc_io io={m,rd,wr,clock_us,clean,bus};return ns_fmc_program_page(&io,&work,ce,row,data,record);}
int main(void){
 for(unsigned ce=0;ce<2;++ce)for(unsigned cau=0;cau<2;++cau){struct model m={0};memset(&work,0,sizeof work);
  assert(!run(&m,ce,cau<<19|7<<7|1)&&work.phase==3&&!work.poisoned&&m.kicks==2&&m.cleans==4);
  assert(work.ring[0]==(0x80000000u|1u<<ce)&&work.rows[0]==(cau<<19|7<<7|1));
 }
 for(unsigned fault=0;fault<6;++fault){struct model m={0};memset(&work,0,sizeof work);
  if(fault==0)m.phase_fault=1;if(fault==1)m.phase_fault=2;if(fault==2)m.verdict_fault=5;
  if(fault>=3&&fault<=4){m.timeout=1;m.frozen=fault==4;}if(fault==5)m.wrong_row=1;
  int rc=run(&m,0,7<<7);assert(rc<0&&work.poisoned);
  unsigned writes=m.writes;assert(run(&m,0,7<<7)==-14&&m.writes==writes);
  if(fault==0||fault==3||fault==4)assert(m.kicks==1);
 }
 for(unsigned mode=0;mode<4;++mode){struct model m={0};memset(&work,0,sizeof work);
  if(mode==3)m.bad_bus=1;
  assert(run(&m,mode==0?2:0,mode==1?0:mode==2?2084<<7:7<<7)==NS_ARGUMENT&&!m.writes);
 }
 puts("PASS FMC controller MODEL: CE/CAU guards, exact stock mailboxes/two kicks, DMA publication, IRQ/verdict/address errors, timeout/frozen clock, poisoned retries; actual CS data covered by full-system tests");
}
