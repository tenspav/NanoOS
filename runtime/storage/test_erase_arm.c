/* SYNTHETIC MEDIA ONLY: deliberately erases seeded programmed test blocks.
 * Built only by test_erase_effect.py; never a deployable fixture choice. */
#include "gameabi.h"
#include "fmc_erase.h"
static const struct nanoos_api *api;
static uint32_t data[4096],meta[16];
static struct ns_fmc_erase state;
struct result {uint32_t status,erases,pages,neighbors,rc;};
volatile struct result ns_erase_effect_stats;
static uint32_t rd(void *p,uint32_t o){(void)p;return *(volatile uint32_t *)(uintptr_t)(0x38a00000+o);}
static void wr(void *p,uint32_t o,uint32_t v){(void)p;*(volatile uint32_t *)(uintptr_t)(0x38a00000+o)=v;}
static uint32_t us(void *p){(void)p;return api->usec();}
static int init(void *p){(void)p;return api->nand_probe()?1:0;}
static int page(unsigned bank,unsigned block,unsigned pg,uint32_t value,int erased){
 int rc=api->nand_inspect_ppn_page(bank/2,(bank%2)<<19|block<<7|pg,data,meta);
 if(rc&&(!erased||rc!=-2))return 0;
 for(unsigned i=0;i<4096;++i)if(data[i]!=value)return 0;
 for(unsigned i=0;i<16;++i)if(meta[i]!=(erased?0xffffffffu:0x11223344u))return 0;
 return 1;
}
int nano_example_main(const struct nanoos_api *a){
 api=a;ns_erase_effect_stats.status=1;
 if(!a||NGAME_API_TABLE(a)<19||!a->nand_probe())return 1;
 struct ns_fmc_io io={0,rd,wr,us,0,0};
 for(unsigned bank=0;bank<4;++bank){unsigned block=7+bank;
  if(!page(bank,block,0,0xab000000u|block<<8|bank,0)||!page(bank,block,127,0xab000000u|block<<8|bank,0))return 1;
  int rc=ns_fmc_erase_block(&io,&state,bank/2,(bank%2)<<19|block<<7,init);
  ns_erase_effect_stats.rc=(uint32_t)rc;if(rc)return 1;++ns_erase_effect_stats.erases;
  for(unsigned pg=0;pg<128;++pg){if(!page(bank,block,pg,0xffffffffu,1))return 1;++ns_erase_effect_stats.pages;}
  for(unsigned neighbor=0;neighbor<4;++neighbor)if(neighbor!=bank){
   if(!page(neighbor,block,0,0xab000000u|block<<8|neighbor,0)||!page(neighbor,block,127,0xab000000u|block<<8|neighbor,0))return 1;
   ++ns_erase_effect_stats.neighbors;
  }
 }
 ns_erase_effect_stats.status=0;return 0;
}
