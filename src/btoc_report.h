#ifndef NANOOS_BTOC_REPORT_H
#define NANOOS_BTOC_REPORT_H
#include "checkpoint_report.h"
#define BT_DATA_BYTES 16384u
#define BT_RECORD_BYTES (512u*64u)
#define BT_PAYLOAD_BYTES (BT_DATA_BYTES+BT_RECORD_BYTES)
#define BT_MAGIC0 0x434f5442u
#define BT_MAGIC1 0x31574152u
/* Starts after each complete checkpoint packet, with a0xd00byte gap. */
#define BT_PACKET_OFFSET (CP_PACKET_OFFSET+0x32000u)
struct bt_packet_header {uint32_t magic[2],version,data_bytes,record_bytes,block,mask,pages,data_sum,record_sum,header_sum;};
static inline uint32_t bt_packet_base(unsigned index){return rpt_mirror_base[index]+BT_PACKET_OFFSET;}
static inline int bt_packet_header_valid(const struct bt_packet_header *h){
 unsigned banks=0;for(unsigned i=0;i<4;++i)banks+=(h->mask>>i)&1u;
 return h->magic[0]==BT_MAGIC0&&h->magic[1]==BT_MAGIC1&&h->version==1&&h->data_bytes==BT_DATA_BYTES&&h->record_bytes==BT_RECORD_BYTES&&
 h->block>=4&&h->block<2084&&h->mask&&!(h->mask&~15u)&&h->pages==banks*128u&&
 h->header_sum==rpt_sum(h,__builtin_offsetof(struct bt_packet_header,header_sum));
}
static inline int bt_packet_copy(const volatile struct bt_packet_header *src,const volatile uint8_t *payload,uint8_t *dst,uint32_t cap,struct bt_packet_header *out){
 if(!src||!payload||!dst||!out||cap<BT_PAYLOAD_BYTES)return 0;
 uintptr_t d=(uintptr_t)dst,s=(uintptr_t)payload,h=(uintptr_t)src,o=(uintptr_t)out;
 if(d>UINTPTR_MAX-BT_PAYLOAD_BYTES||s>UINTPTR_MAX-BT_PAYLOAD_BYTES||h>UINTPTR_MAX-sizeof *src||o>UINTPTR_MAX-sizeof *out)return 0;
 if((d<s+BT_PAYLOAD_BYTES&&s<d+BT_PAYLOAD_BYTES)||(d<h+sizeof *src&&h<d+BT_PAYLOAD_BYTES)||
    (o<d+BT_PAYLOAD_BYTES&&d<o+sizeof *out)||(o<s+BT_PAYLOAD_BYTES&&s<o+sizeof *out))return 0;
 struct bt_packet_header hcopy;
 for(unsigned i=0;i<sizeof hcopy/4;++i)((uint32_t *)&hcopy)[i]=((const volatile uint32_t *)src)[i];
 if(!bt_packet_header_valid(&hcopy))return 0;
 for(unsigned i=0;i<BT_PAYLOAD_BYTES;++i)dst[i]=payload[i];
 if(rpt_sum(dst,BT_DATA_BYTES)!=hcopy.data_sum||rpt_sum(dst+BT_DATA_BYTES,BT_RECORD_BYTES)!=hcopy.record_sum)return 0;
 *out=hcopy;return 1;
}
static inline void bt_packet_clear_at(volatile struct bt_packet_header *h,const struct bt_packet_header *exported){
 struct bt_packet_header copy;for(unsigned i=0;i<sizeof copy/4;++i)((uint32_t *)&copy)[i]=((volatile uint32_t *)h)[i];
 if(!bt_packet_header_valid(&copy))return;
 for(unsigned i=0;i<sizeof copy/4;++i)if(((uint32_t *)&copy)[i]!=((const uint32_t *)exported)[i])return;
 const volatile uint8_t *p=(const volatile uint8_t *)h+RPT_DATA;
 if(rpt_sum(p,BT_DATA_BYTES)!=copy.data_sum||rpt_sum(p+BT_DATA_BYTES,BT_RECORD_BYTES)!=copy.record_sum)return;
 h->magic[0]=0;
}
static inline void bt_packet_clear_matching(const struct bt_packet_header *h){
 for(unsigned i=0;i<RPT_MIRRORS;++i)bt_packet_clear_at((volatile struct bt_packet_header *)(uintptr_t)bt_packet_base(i),h);
}
#endif
