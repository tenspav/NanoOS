#include "partition.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
static unsigned char disk[524288];
static int fail;
static int read_bytes(void *p,uint64_t off,void *out,uint32_t n){
 (void)p;if(fail)return -7;
 if(off>sizeof disk||n>sizeof disk-off)return NS_FORMAT;
 memcpy(out,disk+off,n);return 0;
}
static void be16(unsigned char *p,unsigned v){p[0]=v>>8;p[1]=v;}
static void be32(unsigned char *p,unsigned v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static void le32(unsigned char *p,unsigned v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
static void apm(void){
 memset(disk,0,sizeof disk);be16(disk,0x4552);be16(disk+2,4096);be32(disk+4,128);
 for(unsigned i=1;i<=2;++i){unsigned char *p=disk+i*4096;be16(p,0x504d);be32(p+4,2);
  be32(p+8,i==1?1:64);be32(p+12,i==1?62:64);strcpy((char *)p+48,i==1?"Apple_partition_map":"Apple_HFS");}
}
static int discover(uint64_t *off,uint64_t *size){return ns_partition_hfs(read_bytes,0,0,sizeof disk,off,size);}
int main(void){
 uint64_t off=9,size=9;apm();assert(!discover(&off,&size)&&off==262144&&size==262144);
 be32(disk+8192+12,65);assert(discover(&off,&size)==NS_FORMAT);
 apm();strcpy((char *)disk+4096+48,"Apple_HFS");assert(discover(&off,&size)==NS_CONFLICT);
 apm();be32(disk+8192+4,3);assert(discover(&off,&size)==NS_FORMAT);
 apm();be16(disk+2,513);assert(discover(&off,&size)==NS_FORMAT);
 apm();disk[8192+48]='?';assert(discover(&off,&size)==NS_NOT_FOUND);
 memset(disk,0,sizeof disk);disk[510]=0x55;disk[511]=0xaa;disk[446+4]=0xaf;
 le32(disk+446+8,512);le32(disk+446+12,512);
 assert(!discover(&off,&size)&&off==262144&&size==262144);
 le32(disk+446+12,513);assert(discover(&off,&size)==NS_FORMAT);
 le32(disk+446+12,512);memcpy(disk+462,disk+446,16);assert(discover(&off,&size)==NS_CONFLICT);
 fail=1;off=9;size=9;assert(discover(&off,&size)==-7&&off==9&&size==9);
 assert(ns_partition_hfs(read_bytes,0,UINT64_MAX-511,512,&off,&size)==NS_ARGUMENT);
 puts("PASS partitions: 4096-byte Apple map, 512-byte MBR, unique HFS selection, bounds/corruption/I/O rejection");
}
