#include "checkpoint_plan.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t image[49152],record[192],out[49152],outrecord[192];
static uint8_t mask[2084];static struct ns_run base[8],runs[64];static struct ns_update updates[16],work[16];
static struct ns_sftl map;static struct ns_cp_plan plan;
static const unsigned sections[48]={1,1,1,1,2,2,2,2,3,3,3,3,3,3,3,3,5,5,5,5,5,5,5,5,5,5,5,5,6,6,6,6,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4};
int main(void){
 memset(mask,15,sizeof mask);image[0]=2;image[1]=7;image[2]=8;image[4095]=2084;
 for(unsigned i=0;i<2084;++i){image[8192+i*2]=i==7||i==8?7:i==10?1:9;image[8193+i*2]=2<<16;}
 image[8192+9*2]=8<<16|3;
 for(unsigned i=32768;i<49152;++i)image[i]=0xffffffffu;
 image[32768]=0;image[32769]=0xfffffff0u;image[32770]=9*2048;image[32771]=8;
 uint32_t gen=9,key=65520;
 for(unsigned i=0;i<48;++i){record[i*4]=key<<16|sections[i]<<8|0x1f;record[i*4+1]=gen;record[i*4+3]=0xffffffffu;
  unsigned offset=0,capacity=0;if(i>=8&&i<16){offset=(i-8)*512;capacity=512;}else if(i>=16&&i<28){offset=(i-16)*256;capacity=256;}else if(i>=28&&i<32){offset=(i-28)*1024;capacity=1024;}
  unsigned remain=offset<2084?2084-offset:0;if(capacity>remain)capacity=remain;record[i*4+2]=capacity<<16|offset;
  if(i!=47){if(key==65535){key=0;++gen;}else ++key;}}
 assert(!ns_sftl_init(&map,2084,8,mask,base,8,updates,16,gen,key));base[0]=(struct ns_run){0,9*2048,8};map.nruns=1;assert(!ns_sftl_finish(&map));
 assert(!ns_checkpoint_plan(&plan,&map,image,record,0,0,out,outrecord,runs,64,work,16)&&plan.phase==2&&plan.runs==1&&!plan.changed_blocks);
 assert(gen==10&&plan.first_gen==10&&plan.first_key==key+1);
 uint32_t r[4]={(key+1)<<16|1,gen,2,9};assert(!ns_sftl_replay(&map,10*2048,r)&&!ns_sftl_finish(&map));
 assert(ns_checkpoint_plan(&plan,&map,image,record,0,0,out,outrecord,runs,64,work,16)==NS_CONFLICT&&map.ready);
 struct ns_cp_closed closed={10,8,0,{9,9,9,1}};
 assert(!ns_checkpoint_plan(&plan,&map,image,record,&closed,1,out,outrecord,runs,64,work,16));
 assert(plan.phase==2&&plan.runs==3&&plan.changed_blocks==2&&plan.checks==10003);
 assert(out[8192+9*2]==(7<<16|3)&&out[8192+10*2]==(1<<16|0x1003));
 assert(ns_checkpoint_plan(&plan,&map,image,record,&closed,1,image,outrecord,runs,64,work,16)==NS_ARGUMENT);
 assert(ns_checkpoint_plan(&plan,&map,image,record,&closed,1,out,outrecord,runs,64,work,0)==NS_SPACE);
 image[4095]=2088;assert(ns_checkpoint_plan(&plan,&map,image,record,&closed,1,out,outrecord,runs,64,work,16)==NS_FORMAT);image[4095]=2084;
 image[8192+9*2]^=1u<<16;assert(ns_checkpoint_plan(&plan,&map,image,record,&closed,1,out,outrecord,runs,64,work,16)==NS_CONFLICT);
 assert(map.ready&&map.nruns==1&&map.nupdates==1&&base[0].count==8&&updates[0].vba==10*2048);
 puts("PASS checkpoint RAM planner ASan/UBSan: weave wrap, full replay materialization, counts/closed flags,10003lookup checks and source-preserving rejection; no I/O callback exists");
}
