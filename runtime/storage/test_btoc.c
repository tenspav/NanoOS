#include "btoc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct ns_btoc_entry entries[2048];static uint32_t data[8192],record[32];
int main(void){
 struct ns_btoc b;assert(!ns_btoc_init(&b,entries,2048));
 uint32_t r[4]={65534u<<16|1,9,100,8};assert(!ns_btoc_add(&b,r));
 r[0]=65535u<<16|2;r[2]=101;assert(!ns_btoc_add(&b,r));
 r[0]=1;r[1]=10;r[2]=102;assert(!ns_btoc_add(&b,r));
 assert(b.n==1&&b.entry[0].count==3&&b.span==3);
 r[0]=4u<<16|1;r[2]=500;r[3]=9;assert(!ns_btoc_add(&b,r));assert(b.n==2&&b.entry[1].delta==6&&b.span==7);
 assert(!ns_btoc_encode(&b,4,10,data,record)&&!ns_btoc_match(&b,4,10,data,record));
 assert(record[0]==(65534u<<16|0x81c)&&record[1]==9&&record[2]==10&&record[3]==0xffffffffu);
 uint32_t stats[4];ns_btoc_statistics(&b,stats);assert(stats[0]==8&&stats[1]==9&&stats[2]==8&&stats[3]==2);
 data[1000]^=1;assert(!ns_btoc_match(&b,4,10,data,record)); /* Ignored padding. */
 data[3]^=1;assert(ns_btoc_match(&b,4,10,data,record)==NS_VERIFY);data[3]^=1;
 record[15]^=1;assert(ns_btoc_match(&b,4,10,data,record)==NS_VERIFY);
 assert(ns_btoc_encode(&b,4,0,(uint32_t *)entries,record)==NS_ARGUMENT);
 unsigned sectors=b.sectors;r[1]=9;assert(ns_btoc_add(&b,r)==NS_CONFLICT&&b.sectors==sectors);
 r[1]=11;r[3]=12;assert(ns_btoc_add(&b,r)==NS_FORMAT&&b.sectors==sectors);
 assert(!ns_btoc_init(&b,entries,2048));
 for(unsigned i=0;i<2040;++i){uint32_t a[4]={i<<16|1,9,(i*37u)%4096,8};assert(!ns_btoc_add(&b,a));}
 assert(b.n==2040&&ns_btoc_encode(&b,4,0,data,record)==NS_ARGUMENT);
 assert(!ns_btoc_encode(&b,8,0,data,record)&&!ns_btoc_match(&b,8,0,data,record)&&record[0]==0x101c);
 assert(!ns_btoc_init(&b,entries,1));uint32_t one[4]={0x101,9,0,8};assert(!ns_btoc_add(&b,one));one[0]=1u<<16|0x101;one[2]=9;
 assert(ns_btoc_add(&b,one)==NS_SPACE&&b.n==1&&b.sectors==1);
 assert(!ns_btoc_encode(&b,4,0,data,record)&&record[0]==0x91c);
 puts("PASS BTOC format: weave gaps/wrap, epoch and LBA coalescing, entry-weighted statistics, role flags,4/8sector summaries, capacity/alias/corruption guards");
}
