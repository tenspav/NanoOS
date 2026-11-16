#include "../../NanoOS/src/checkpoint_report.h"
#include "../../NanoOS/src/btoc_report.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t payload[CP_PAYLOAD_BYTES],copy[CP_PAYLOAD_BYTES];
int main(void){
 for(unsigned i=0;i<CP_PAYLOAD_BYTES;++i)payload[i]=(uint8_t)(i*37+i/4096);
 struct cp_packet_header h={{CP_MAGIC0,CP_MAGIC1},1,CP_DATA_BYTES,CP_RECORD_BYTES,3,9,100,9,147,0,0,0},out;
 h.data_sum=rpt_sum(payload,CP_DATA_BYTES);h.record_sum=rpt_sum(payload+CP_DATA_BYTES,CP_RECORD_BYTES);
 h.header_sum=rpt_sum(&h,__builtin_offsetof(struct cp_packet_header,header_sum));
 assert(cp_packet_copy(&h,payload,copy,sizeof copy,&out)&&!memcmp(payload,copy,sizeof copy)&&!memcmp(&h,&out,sizeof h));
 assert(!cp_packet_copy(&h,payload,copy,sizeof copy-1,&out));
 assert(!cp_packet_copy(&h,payload,payload,sizeof copy,&out));
 assert(!cp_packet_copy(&h,payload,copy,sizeof copy,(struct cp_packet_header *)copy));
 payload[123]^=1;assert(!cp_packet_copy(&h,payload,copy,sizeof copy,&out));payload[123]^=1;
 payload[CP_DATA_BYTES+100]^=1;assert(!cp_packet_copy(&h,payload,copy,sizeof copy,&out));payload[CP_DATA_BYTES+100]^=1;
 for(unsigned i=0;i<sizeof h/4;++i){uint32_t saved=((uint32_t *)&h)[i];((uint32_t *)&h)[i]^=1;
  assert(!cp_packet_copy(&h,payload,copy,sizeof copy,&out));((uint32_t *)&h)[i]=saved;}
 assert(cp_packet_copy(&h,payload,copy,sizeof copy,&out));
 for(unsigned i=0;i<RPT_MIRRORS;++i){uint32_t base=cp_packet_base(i);
  assert(base==rpt_mirror_base[i]+RPT_DATA+RPT_MIR_SIZE);
  assert(base+RPT_DATA+CP_PAYLOAD_BYTES<0x0c000000u);
  assert(base>=0x0a000000u&&(base+RPT_DATA+CP_PAYLOAD_BYTES<=0x0b000000u||base>=0x0ba00000u));
  for(unsigned j=0;j<i;++j)assert(base+RPT_DATA+CP_PAYLOAD_BYTES<=cp_packet_base(j)||cp_packet_base(j)+RPT_DATA+CP_PAYLOAD_BYTES<=base);
 }
 struct bt_packet_header bh={{BT_MAGIC0,BT_MAGIC1},1,BT_DATA_BYTES,BT_RECORD_BYTES,9,15,512,0,0,0},bo;
 bh.data_sum=rpt_sum(payload,BT_DATA_BYTES);bh.record_sum=rpt_sum(payload+BT_DATA_BYTES,BT_RECORD_BYTES);bh.header_sum=rpt_sum(&bh,__builtin_offsetof(struct bt_packet_header,header_sum));
 assert(bt_packet_copy(&bh,payload,copy,sizeof copy,&bo));
 for(unsigned i=0;i<sizeof bh/4;++i){uint32_t v=((uint32_t *)&bh)[i];((uint32_t *)&bh)[i]^=1;assert(!bt_packet_copy(&bh,payload,copy,sizeof copy,&bo));((uint32_t *)&bh)[i]=v;}
 payload[BT_DATA_BYTES+100]^=1;assert(!bt_packet_copy(&bh,payload,copy,sizeof copy,&bo));payload[BT_DATA_BYTES+100]^=1;
 assert(!bt_packet_copy(&bh,payload,copy,BT_PAYLOAD_BYTES-1,&bo)&&!bt_packet_copy(&bh,payload,payload,sizeof payload,&bo));
 for(unsigned i=0;i<RPT_MIRRORS;++i){uint32_t base=bt_packet_base(i);assert(base>cp_packet_base(i)+RPT_DATA+CP_PAYLOAD_BYTES);
  assert(base+RPT_DATA+BT_PAYLOAD_BYTES<0x0c000000u&&(base+RPT_DATA+BT_PAYLOAD_BYTES<=0x0b000000u||base>=0x0ba00000u));}
 puts("PASS checkpoint/BTOC packet: exact capture copy, every header-word corruption, payload/record corruption, capacity/alias guards, all mirror ranges separate from active game/stack");
}
