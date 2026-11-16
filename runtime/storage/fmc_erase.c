#include "fmc_erase.h"
static int event(const struct ns_fmc_io *io,struct ns_fmc_erase *w,uint32_t bit,uint32_t budget){
 uint32_t start=io->usec(io->opaque);unsigned polls=0;w->event=bit;
 while(!(io->read(io->opaque,0x48)&bit))
  if(io->usec(io->opaque)-start>=budget||++polls>=1000000u)return -9;
 io->write(io->opaque,0x48,bit);return 0;
}
static int command(const struct ns_fmc_io *io,struct ns_fmc_erase *w,uint32_t cmd){
 io->write(io->opaque,8,cmd);return event(io,w,2,20000);
}
int ns_fmc_erase_block(const struct ns_fmc_io *io,struct ns_fmc_erase *w,uint32_t ce,
 uint32_t row,int (*bringup)(void *)){
 if(!io||!w||!io->read||!io->write||!io->usec||!bringup||ce>1||(row>>19)>1||
    (row&127u)||((row>>7)&4095)<4||((row>>7)&4095)>=2084)return NS_ARGUMENT;
 if(w->poisoned)return -14;
 uint32_t start=io->usec(io->opaque);w->poisoned=1;w->phase=1;w->status=0;
 w->last_ce=ce;w->last_row=row;
 /* Exact single-block stock CPU sequence084cd960..9ae. Timing global+28
  * is10000, CE field is reconstructed,6A/address4/D7 then77/7D/status. */
 io->write(io->opaque,0,0x10001u|(2u<<ce));
 int rc=command(io,w,0x6a);
 if(!rc){w->phase=2;io->write(io->opaque,0x0c,row);io->write(io->opaque,0x2c,3);io->write(io->opaque,4,1);rc=event(io,w,4,20000);}
 if(!rc){w->phase=3;rc=command(io,w,0xd7);}
 if(!rc)rc=command(io,w,0x77);
 if(!rc)rc=command(io,w,0x7d);
 if(!rc){
  w->phase=4;unsigned polls=0;
  do{
   io->write(io->opaque,0x48,0x800000);io->write(io->opaque,0x14,1);io->write(io->opaque,4,0xea);
   rc=event(io,w,0x800000,2000000);
   w->status=io->read(io->opaque,0x4c);io->write(io->opaque,4,0x20);io->write(io->opaque,0x48,0x1000000);
   if(rc)break;
   /* run48 returned50 on the erase view after a successful bank. Stock
    * erase084cd99e passes NULL to the status helper and never tests bit4.
    * Do not import the READ uECC bit here; retain NAND FAIL bit0. */
   if(w->status&1u){rc=-7;break;}
   /*77/7D erase status is the bit6-ready view. The bit1/bit5 checks
    * belong to the79 program view; stock erase does not request it. */
   if(w->status&0x40u)break;
   if(io->usec(io->opaque)-start>=2000000u||++polls>=64u){rc=-9;break;}
  }while(1);
 }
 if(!rc)rc=command(io,w,0x77);
 io->write(io->opaque,4,0x20);io->write(io->opaque,0,1);
 w->elapsed=io->usec(io->opaque)-start;if(rc)return rc;
 w->phase=5;if(!bringup(io->opaque))return -7;
 w->phase=6;w->poisoned=0;++w->completed;return 0;
}
