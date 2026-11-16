#include "nano_game.h"
#include "gameabi.h"
#include "fmc_program.h"
#include "fmc_erase.h"
static uint32_t clock_us(void *p){return ((const struct nanoos_api *)p)->usec();}
static int inspect(void *p,uint32_t ce,uint32_t row,void *data,uint32_t *r){return ((const struct nanoos_api *)p)->nand_inspect_ppn_page(ce,row,data,r);}
static int context(void *p,unsigned b,unsigned pg,unsigned view,unsigned n,void *data,uint32_t *r){return ((const struct nanoos_api *)p)->nand_read_ppn_context(b,pg,view,n,data,r);}
static uint32_t reg_read(void *p,uint32_t off){(void)p;return *(volatile uint32_t *)(uintptr_t)(0x38a00000u+off);}
static void reg_write(void *p,uint32_t off,uint32_t v){(void)p;*(volatile uint32_t *)(uintptr_t)(0x38a00000u+off)=v;}
static void clean(void *p){((const struct nanoos_api *)p)->dcache_clean();}
static uint32_t bus(void *p,const void *v,uint32_t n){(void)p;uintptr_t a=(uintptr_t)v;return a>=0x08000000u&&a<0x0c000000u&&n<=0x0c000000u-a?(uint32_t)a:0;}
static int program(void *p,uint32_t ce,uint32_t row,const void *data,const uint32_t *r){
 struct ns_fmc_io io={p,reg_read,reg_write,clock_us,clean,bus};struct ns_fmc_program result={0};return ns_fmc_program_page(&io,&result,ce,row,data,r);
}
static int bringup(void *p){return ((const struct nanoos_api *)p)->nand_probe()?1:0;}
static int erase(void *p,uint32_t ce,uint32_t row){
 struct ns_fmc_io io={p,reg_read,reg_write,clock_us,clean,bus};struct ns_fmc_erase result={0};return ns_fmc_erase_block(&io,&result,ce,row,bringup);
}
int ns_nano_game_mount(struct ns_game *g,const struct nanoos_api *api){
 if(!g||!api||api->abi!=NGAME_ABI||NGAME_API_TABLE(api)<19||!api->nand_probe||!api->nand_info||!api->nand_inspect_ppn_page||!api->nand_read_ppn_context||!api->usec||!api->dcache_clean)return NS_UNSUPPORTED;
 if(!api->nand_probe())return NS_IO;
 if(api->nand_info->caus!=2||api->nand_info->blocks!=2088||api->nand_info->pages!=128)return NS_UNSUPPORTED;
 struct ns_mount_io read={(void *)api,inspect,context,clock_us};struct ns_write_io write={(void *)api,inspect,program,erase,clock_us};
 return ns_game_mount(g,&read,&write,150000000u);
}
