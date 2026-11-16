#include "partition.h"
static uint32_t be16(const unsigned char *p){return (uint32_t)p[0]<<8|p[1];}
static uint32_t be32(const unsigned char *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static uint32_t le32(const unsigned char *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
int ns_partition_hfs(ns_bytes_read read,void *opaque,uint64_t off,uint64_t size,uint64_t *vo,uint64_t *vs){
 if(!read||!vo||!vs||size<512||off>UINT64_MAX-size)return NS_ARGUMENT;
 unsigned char b[512];int rc=read(opaque,off,b,512);if(rc)return rc;
 uint64_t start=0,length=0;unsigned found=0;
 if(be16(b)==0x4552){
  uint32_t block=be16(b+2),blocks=be32(b+4);
  if(block<512||block>32768||(block&(block-1))||!blocks||(uint64_t)blocks*block>size)return NS_FORMAT;
  rc=read(opaque,off+block,b,512);if(rc)return rc;
  uint32_t entries=be32(b+4);if(!entries||entries>4096||entries>=blocks)return NS_FORMAT;
  for(unsigned i=1;i<=entries;++i){
   if(i>1){rc=read(opaque,off+(uint64_t)i*block,b,512);if(rc)return rc;}
   if(be16(b)!=0x504d||be32(b+4)!=entries)return NS_FORMAT;
   static const char type[]="Apple_HFS";unsigned j=0;for(;j<9;++j)if(b[48+j]!=(unsigned char)type[j])break;
   if(j!=9||b[57])continue;
   uint32_t first=be32(b+8),count=be32(b+12);
   if(!first||first>=blocks||!count||count>blocks-first)return NS_FORMAT;
   start=(uint64_t)first*block;length=(uint64_t)count*block;++found;
  }
 }else if(b[510]==0x55&&b[511]==0xaa){
  for(unsigned i=0;i<4;++i){const unsigned char *p=b+446+i*16;if(p[4]!=0xaf)continue;
   start=(uint64_t)le32(p+8)*512;length=(uint64_t)le32(p+12)*512;
   if(!start||!length||start>size||length>size-start)return NS_FORMAT;
   ++found;
  }
 }else return NS_UNSUPPORTED;
 if(!found)return NS_NOT_FOUND;
 if(found!=1)return NS_CONFLICT;
 *vo=off+start;*vs=length;return 0;
}
