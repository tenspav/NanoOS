#include "../../NanoOS/src/checkpoint_report.h"
#include "../../NanoOS/src/btoc_report.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t packets[RPT_MIRRORS][RPT_DATA+CP_PAYLOAD_BYTES] __attribute__((aligned(64)));
static struct cp_packet_header want;
static uint8_t bt_packets[RPT_MIRRORS][RPT_DATA+BT_PAYLOAD_BYTES] __attribute__((aligned(64)));
static struct bt_packet_header bt_want;
static int is_btoc;
static unsigned writes,fail_at;
static uintptr_t test_base(unsigned i){return (uintptr_t)packets[i];}
static void test_clear(const struct cp_packet_header *h){
 for(unsigned i=0;i<RPT_MIRRORS;++i)cp_packet_clear_at((volatile struct cp_packet_header *)packets[i],h);
}
static uintptr_t test_bt_base(unsigned i){return (uintptr_t)bt_packets[i];}
static void test_bt_clear(const struct bt_packet_header *h){for(unsigned i=0;i<RPT_MIRRORS;++i)bt_packet_clear_at((volatile struct bt_packet_header *)bt_packets[i],h);}
#define bt_packet_base test_bt_base
#define bt_packet_clear_matching test_bt_clear
#define cp_packet_base test_base
#define cp_packet_clear_matching test_clear
#define main chainloader_unused_main
#define __attribute__(x)
#include "../../NanoOS/loader/chainloader.c"
#undef main
#undef __attribute__
const uint8_t default_payload_bin[1]={0},default_payload_bin_end[1]={0};
uint32_t hb_fs_size(const char *p){(void)p;return 0;}
uint32_t hb_fs_read(const char *p,void *out,uint32_t size){(void)p;(void)out;(void)size;return 0;}
bool hb_fs_mkdir(const char *p){assert(!strcmp(p,"/Apps/Data/NanoOS"));return true;}
bool hb_fs_write(const char *p,const void *data,uint32_t size){
 ++writes;
 if(is_btoc){
  if(writes==1){assert(!strcmp(p,"/Apps/Data/NanoOS/btoc.bin")&&size==BT_DATA_BYTES);assert(!memcmp(data,bt_packets[1]+RPT_DATA,size));}
  if(writes==2){assert(!strcmp(p,"/Apps/Data/NanoOS/btoc-records.bin")&&size==BT_RECORD_BYTES);assert(!memcmp(data,bt_packets[1]+RPT_DATA+BT_DATA_BYTES,size));}
  if(writes==3){assert(!strcmp(p,"/Apps/Data/NanoOS/btoc-header.bin")&&size==sizeof bt_want);assert(!memcmp(data,&bt_want,size));}
  return writes!=fail_at;
 }
 if(writes==1){assert(!strcmp(p,"/Apps/Data/NanoOS/checkpoint.bin")&&size==CP_DATA_BYTES);assert(!memcmp(data,packets[1]+RPT_DATA,size));}
 if(writes==2){assert(!strcmp(p,"/Apps/Data/NanoOS/checkpoint-records.bin")&&size==CP_RECORD_BYTES);assert(!memcmp(data,packets[1]+RPT_DATA+CP_DATA_BYTES,size));}
 if(writes==3){assert(!strcmp(p,"/Apps/Data/NanoOS/checkpoint-header.bin")&&size==sizeof want);assert(!memcmp(data,&want,size));}
 return writes!=fail_at;
}
uint32_t hw_shutdown_all(void){return 0;}
void handoff_to_payload(const void *p,uint32_t n){(void)p;(void)n;abort();}
static void setup(void){
 memset(packets,0,sizeof packets);writes=0;
 for(unsigned i=0;i<CP_PAYLOAD_BYTES;++i)packets[1][RPT_DATA+i]=(uint8_t)(i*13+i/4096);
 want=(struct cp_packet_header){{CP_MAGIC0,CP_MAGIC1},1,CP_DATA_BYTES,CP_RECORD_BYTES,3,9,100,9,147,0,0,0};
 want.data_sum=rpt_sum(packets[1]+RPT_DATA,CP_DATA_BYTES);want.record_sum=rpt_sum(packets[1]+RPT_DATA+CP_DATA_BYTES,CP_RECORD_BYTES);
 want.header_sum=rpt_sum(&want,__builtin_offsetof(struct cp_packet_header,header_sum));
 memcpy(packets[1],&want,sizeof want);memcpy(packets[2],packets[1],sizeof packets[1]);
 memcpy(packets[0],packets[1],sizeof packets[1]);packets[0][RPT_DATA+123]^=1;
}
static void setup_bt(void){
 memset(bt_packets,0,sizeof bt_packets);writes=0;
 for(unsigned i=0;i<BT_PAYLOAD_BYTES;++i)bt_packets[1][RPT_DATA+i]=(uint8_t)(i*17+i/4096);
 bt_want=(struct bt_packet_header){{BT_MAGIC0,BT_MAGIC1},1,BT_DATA_BYTES,BT_RECORD_BYTES,9,15,512,0,0,0};
 bt_want.data_sum=rpt_sum(bt_packets[1]+RPT_DATA,BT_DATA_BYTES);bt_want.record_sum=rpt_sum(bt_packets[1]+RPT_DATA+BT_DATA_BYTES,BT_RECORD_BYTES);
 bt_want.header_sum=rpt_sum(&bt_want,__builtin_offsetof(struct bt_packet_header,header_sum));
 memcpy(bt_packets[1],&bt_want,sizeof bt_want);memcpy(bt_packets[2],bt_packets[1],sizeof bt_packets[1]);
 memcpy(bt_packets[0],bt_packets[1],sizeof bt_packets[1]);bt_packets[0][RPT_DATA+123]^=1;
}
int main(void){
 for(fail_at=1;fail_at<=3;++fail_at){setup();export_checkpoint();assert(writes==fail_at);assert(((struct cp_packet_header *)packets[1])->magic[0]==CP_MAGIC0);}
 setup();fail_at=0;export_checkpoint();assert(writes==3);
 assert(((struct cp_packet_header *)packets[0])->magic[0]==CP_MAGIC0);
 assert(((struct cp_packet_header *)packets[1])->magic[0]==0&&((struct cp_packet_header *)packets[2])->magic[0]==0);
 setup();for(unsigned i=0;i<RPT_MIRRORS;++i)packets[i][0]=0;
 export_checkpoint();assert(!writes);
 is_btoc=1;
 for(fail_at=1;fail_at<=3;++fail_at){setup_bt();export_btoc();assert(writes==fail_at&&((struct bt_packet_header *)bt_packets[1])->magic[0]==BT_MAGIC0);}
 setup_bt();fail_at=0;export_btoc();assert(writes==3&&((struct bt_packet_header *)bt_packets[1])->magic[0]==0&&((struct bt_packet_header *)bt_packets[0])->magic[0]==BT_MAGIC0);
 setup_bt();for(unsigned i=0;i<RPT_MIRRORS;++i)bt_packets[i][0]=0;
 export_btoc();assert(!writes);
 puts("PASS loader checkpoint/BTOC export: corrupt-first fallback, exact three bounded outputs, every write failure retains packet, success clears matching headers, missing data writes nothing");
}
