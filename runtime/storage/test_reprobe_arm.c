/* MODEL ONLY: RESET invalidates power-state2; repeated probe must restore it. */
#include "gameabi.h"
static const struct nanoos_api *api;
static uint32_t data[4096],meta[16];
struct result {uint32_t status,cycles,lost_reads,restored_reads,probe_rc;};
volatile struct result ns_reprobe_stats;
#define FMC(o) (*(volatile uint32_t *)(0x38a00000u+(o)))
static int read_page(unsigned bank,int configured){
 for(unsigned i=0;i<4096;++i)data[i]=0xa5a5a5a5u;
 for(unsigned i=0;i<16;++i)meta[i]=0x5a5a5a5au;
 int rc=api->nand_inspect_ppn_page(bank/2,(bank%2)<<19|7<<7,data,meta);
 if(configured){if(rc)return 0;
  for(unsigned i=0;i<4096;++i)if(data[i]!=(0x12340000u|bank))return 0;
  for(unsigned i=0;i<16;++i)if(meta[i]!=0x11223344u)return 0;
 }else{if(rc!=-1)return 0;
  for(unsigned i=0;i<4096;++i)if(data[i]!=0xa5a5a5a5u)return 0;
  for(unsigned i=0;i<16;++i)if(meta[i]!=0x5a5a5a5au)return 0;
 }return 1;
}
int nano_example_main(const struct nanoos_api *a){
 api=a;ns_reprobe_stats.status=1;if(!a||NGAME_API_TABLE(a)<19||!a->nand_probe())return 1;
 for(unsigned bank=0;bank<4;++bank)if(!read_page(bank,1))return 1;
 for(unsigned cycle=0;cycle<3;++cycle){
  for(unsigned ce=0;ce<2;++ce){FMC(0)=0xff001u|(2u<<ce);FMC(8)=0xff;
   unsigned polls=0;while(!(FMC(0x48)&2u))if(++polls>1000000u)return 1;FMC(0x48)=2;FMC(0)=1;}
  for(unsigned bank=0;bank<4;++bank){if(!read_page(bank,0))return 1;++ns_reprobe_stats.lost_reads;}
  ns_reprobe_stats.probe_rc=a->nand_probe()?1:0;if(!ns_reprobe_stats.probe_rc)return 1;
  for(unsigned bank=0;bank<4;++bank){if(!read_page(bank,1))return 1;++ns_reprobe_stats.restored_reads;}
  ++ns_reprobe_stats.cycles;
 }
 ns_reprobe_stats.status=0;return 0;
}
