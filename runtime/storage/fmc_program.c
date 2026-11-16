#include "fmc_program.h"
#include "fmc_images.h"
static int execute(const struct ns_fmc_io *io,struct ns_fmc_program *w,
                   const uint32_t *code,uint32_t bytes,int verdict){
 void *p=io->opaque;uint32_t start=io->usec(p);
 io->write(p,0xc10,0);io->write(p,0xc0c,255);io->write(p,0xc4c,bytes);
 io->write(p,0xc04,io->bus(p,code,bytes));
 io->write(p,0xc6c,0x10); /* Stock idle-kick path at080d0cd0. */
 io->clean(p);io->write(p,0xc00,0xfff5);
 unsigned polls=0;uint32_t irq;
 do{
  irq=io->read(p,0xc0c);
  if(irq&13u)break;
  if(io->usec(p)-start>=2000000u||++polls>=1000000u){
   w->irq=irq;w->pc=io->read(p,0xc38);w->elapsed=io->usec(p)-start;io->write(p,0xc10,0);return -9;
  }
 }while(1);
 io->write(p,0xc10,0);io->clean(p);
 w->irq=irq;w->pc=io->read(p,0xc38);w->verdict=io->read(p,0xd00);w->elapsed=io->usec(p)-start;
 io->write(p,0xc0c,irq&13u);
 if((irq&13u)!=1u||(verdict&&w->verdict))return -7;
 return 0;
}
int ns_fmc_program_page(const struct ns_fmc_io *io,struct ns_fmc_program *w,
 uint32_t ce,uint32_t row,const void *data,const uint32_t records[16]){
 if(!io||!w||!io->read||!io->write||!io->usec||!io->clean||!io->bus||!data||!records||
    ((uintptr_t)data&3u)||((uintptr_t)records&3u)||ce>1||(row>>19)>1||
    ((row>>7)&4095)<4||((row>>7)&4095)>=2084)return NS_ARGUMENT;
 if(w->poisoned)return -14;
 uint32_t data_bus=io->bus(io->opaque,data,16384),record_bus=io->bus(io->opaque,records,64);
 uint32_t rows_bus=io->bus(io->opaque,w->rows,64),counts_bus=io->bus(io->opaque,w->counts,64);
 uint32_t ring_bus=io->bus(io->opaque,w->ring,64),fail_bus=io->bus(io->opaque,w->failures,64);
 if(!data_bus||!record_bus||!rows_bus||!counts_bus||!ring_bus||!fail_bus||
    !io->bus(io->opaque,ns_cs_prepare,sizeof ns_cs_prepare)||!io->bus(io->opaque,ns_cs_program,sizeof ns_cs_program))return NS_ARGUMENT;
 for(unsigned i=0;i<16;++i){w->rows[i]=w->counts[i]=w->ring[i]=w->failures[i]=0;}
 w->rows[0]=row;w->counts[0]=1u<<ce;w->counts[1]=1;
 w->ring[0]=0x80000000u|(1u<<ce);w->ring[1]=row;w->ring[2]=data_bus;w->ring[3]=record_bus;
 w->poisoned=1;w->phase=1;
 /* Saved stock caller084cdb52..86: row list, timing, CE/count list, count. */
 io->write(io->opaque,0xd04,rows_bus);io->write(io->opaque,0xd08,0x10000);
 io->write(io->opaque,0xd0c,counts_bus);io->write(io->opaque,0xd10,1);
 int rc=execute(io,w,ns_cs_prepare,sizeof ns_cs_prepare,0);if(rc)return rc;
 w->phase=2;
 /* Saved stock caller084cdc6e..b8: timing+28=10000, sector size1000,
  * four sectors/page, four address cycles and a real failure-log buffer. */
 io->write(io->opaque,0xd00,0xffffffffu);io->write(io->opaque,0xd04,0x10000);
 io->write(io->opaque,0xd08,4096);io->write(io->opaque,0xd0c,ring_bus);
 io->write(io->opaque,0xd10,4);io->write(io->opaque,0xd1c,3);io->write(io->opaque,0xd2c,fail_bus);
 rc=execute(io,w,ns_cs_program,sizeof ns_cs_program,1);if(rc)return rc;
 if(io->read(io->opaque,0x0c)!=row)return -15;
 w->phase=3;w->poisoned=0;return 0;
}
