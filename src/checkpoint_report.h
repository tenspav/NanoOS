#ifndef NANOOS_CHECKPOINT_REPORT_H
#define NANOOS_CHECKPOINT_REPORT_H
#include "report.h"
/* Read-only NAND checkpoint capture. Each packet lives after a normal report
 * mirror, outside the active kernel/game/stack. Native owns RAM at capture;
 * RetailOS may reuse it after reset, so validate every byte before export. */
#define CP_DATA_BYTES (48u*4096u)
#define CP_RECORD_BYTES (48u*16u)
#define CP_PAYLOAD_BYTES (CP_DATA_BYTES+CP_RECORD_BYTES)
#define CP_PACKET_OFFSET 0x5000u
#define CP_MAGIC0 0x5043444eu
#define CP_MAGIC1 0x31574152u
struct cp_packet_header {
 uint32_t magic[2],version,data_bytes,record_bytes,root,first_gen,first_key,last_gen,last_key,data_sum,record_sum,header_sum;
};
static inline uint32_t cp_packet_base(unsigned index){return rpt_mirror_base[index]+CP_PACKET_OFFSET;}
static inline int cp_packet_header_valid(const struct cp_packet_header *h){
 return h->magic[0]==CP_MAGIC0&&h->magic[1]==CP_MAGIC1&&h->version==1&&
 h->data_bytes==CP_DATA_BYTES&&h->record_bytes==CP_RECORD_BYTES&&h->root<2084&&
 h->first_gen!=0xffffffffu&&h->last_gen!=0xffffffffu&&h->first_key<=65535&&h->last_key<=65535&&
 h->header_sum==rpt_sum(h,__builtin_offsetof(struct cp_packet_header,header_sum));
}
/* Snapshot header before copying; ignore packet if any CRC/shape/bounds check
 * fails. No disk writes or fixed-address dereferences in this pure helper. */
static inline int cp_packet_copy(const volatile struct cp_packet_header *src,const volatile uint8_t *payload,
 uint8_t *dst,uint32_t capacity,struct cp_packet_header *out){
 if(!src||!payload||!dst||!out||capacity<CP_PAYLOAD_BYTES)return 0;
 uintptr_t d=(uintptr_t)dst,s=(uintptr_t)payload,h=(uintptr_t)src,o=(uintptr_t)out;
 if(d>UINTPTR_MAX-CP_PAYLOAD_BYTES||s>UINTPTR_MAX-CP_PAYLOAD_BYTES||h>UINTPTR_MAX-sizeof *src||o>UINTPTR_MAX-sizeof *out)return 0;
 if((d<s+CP_PAYLOAD_BYTES&&s<d+CP_PAYLOAD_BYTES)||(d<h+sizeof *src&&h<d+CP_PAYLOAD_BYTES)||
    (o<d+CP_PAYLOAD_BYTES&&d<o+sizeof *out)||(o<s+CP_PAYLOAD_BYTES&&s<o+sizeof *out))return 0;
 struct cp_packet_header snapshot;
 for(unsigned i=0;i<sizeof snapshot/4;++i)((uint32_t *)&snapshot)[i]=((const volatile uint32_t *)src)[i];
 if(!cp_packet_header_valid(&snapshot))return 0;
 for(unsigned i=0;i<CP_PAYLOAD_BYTES;++i)dst[i]=payload[i];
 if(rpt_sum(dst,CP_DATA_BYTES)!=snapshot.data_sum||rpt_sum(dst+CP_DATA_BYTES,CP_RECORD_BYTES)!=snapshot.record_sum)return 0;
 *out=snapshot;return 1;
}
static inline void cp_packet_clear_at(volatile struct cp_packet_header *h,const struct cp_packet_header *exported){
 struct cp_packet_header copy;
 for(unsigned j=0;j<sizeof copy/4;++j)((uint32_t *)&copy)[j]=((volatile uint32_t *)h)[j];
 if(!cp_packet_header_valid(&copy))return;
 for(unsigned j=0;j<sizeof copy/4;++j)if(((uint32_t *)&copy)[j]!=((const uint32_t *)exported)[j])return;
 const volatile uint8_t *payload=(const volatile uint8_t *)h+RPT_DATA;
 if(rpt_sum(payload,CP_DATA_BYTES)!=copy.data_sum||rpt_sum(payload+CP_DATA_BYTES,CP_RECORD_BYTES)!=copy.record_sum)return;
 h->magic[0]=0;
}
static inline void cp_packet_clear_matching(const struct cp_packet_header *exported){
 for(unsigned i=0;i<RPT_MIRRORS;++i)
  cp_packet_clear_at((volatile struct cp_packet_header *)(uintptr_t)cp_packet_base(i),exported);
}
#endif
