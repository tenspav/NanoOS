#include "disk.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
struct source {unsigned calls, fail_bank, corrupt, future, replay;};
static int page_read(void *opaque,uint32_t ce,uint32_t row,void *buf,uint32_t *meta) {
    struct source *s=opaque;unsigned bank=ce*2+(row>>19),page=row&127;
    ++s->calls;memset(buf,0xa5,16384);
    if(s->fail_bank==bank+1)return -10;
    for(unsigned k=0;k<4;++k) {
        unsigned lba=page*16+bank*4+k;
        meta[k*4]=2u<<16|1;meta[k*4+1]=s->future?10:9;
        meta[k*4+2]=lba+(s->corrupt?1000:0);meta[k*4+3]=9;
        if(s->replay&&lba==8){meta[k*4]=1u<<16|1;meta[k*4+1]=10;meta[k*4+2]=1;}
        for(unsigned i=0;i<4096;++i)((unsigned char *)buf)[k*4096+i]=(unsigned char)(lba*13+i);
    }
    return 0;
}
int main(void) {
    uint8_t mask[2]={15,15};struct ns_run runs[2];struct ns_update updates[8];
    struct ns_sftl map;uint32_t node[1024];
    memset(node,255,sizeof node);node[0]=0;node[1]=0xfffffff0;node[2]=0;node[3]=100;
    assert(!ns_sftl_init(&map,2,100,mask,runs,2,updates,8,9,100));
    assert(!ns_sftl_node(&map,node));assert(!ns_sftl_finish(&map));
    uint32_t data[4096],meta[16];unsigned char out[5000];
    struct source source={0};struct ns_disk disk;
    assert(!ns_disk_init(&disk,&map,page_read,&source,data,meta));
    for(unsigned lba=0;lba<16;++lba){
        assert(!ns_disk_read(&disk,lba,out));
        for(unsigned i=0;i<4096;++i)assert(out[i]==(unsigned char)(lba*13+i));
    }
    assert(source.calls==4&&disk.reads==4&&disk.hits==12);
    ns_disk_invalidate(&disk);source.corrupt=1;memset(out,0x5a,sizeof out);
    assert(ns_disk_read(&disk,0,out)==NS_FORMAT&&!disk.valid);
    for(unsigned i=0;i<sizeof out;++i)assert(out[i]==0x5a);
    source.corrupt=0;source.future=1;
    assert(ns_disk_read(&disk,0,out)==NS_CONFLICT&&!disk.valid);
    source.future=0;source.fail_bank=2;
    uint32_t done=99;
    assert(ns_disk_pread(&disk,3*4096+4090,out,20,&done)==-10&&done==6);
    for(unsigned i=0;i<6;++i)assert(out[i]==(unsigned char)(3*13+4090+i));
    for(unsigned i=6;i<sizeof out;++i)assert(out[i]==0x5a);
    assert(!disk.valid);source.fail_bank=0;
    assert(!ns_disk_pread(&disk,4090,out,20,&done)&&done==20);
    for(unsigned i=0;i<20;++i)assert(out[i]==(unsigned char)(i<6?4090+i:13+i-6));
    assert(!ns_disk_pread(&disk,100u*4096,out,20,&done)&&!done);
    uint32_t record[4]={1u<<16|1,10,1,9};
    assert(!ns_sftl_replay(&map,8,record));
    assert(ns_disk_read(&disk,1,out)==NS_NOT_READY);
    assert(!ns_sftl_finish(&map));source.replay=1;
    assert(!ns_disk_read(&disk,1,out));
    for(unsigned i=0;i<4096;++i)assert(out[i]==(unsigned char)(8*13+i));
    source.replay=0;ns_disk_invalidate(&disk);
    assert(ns_disk_read(&disk,1,out)==NS_FORMAT);
    puts("PASS disk cache: four page reads serve16sectors; metadata/weave guards, failure preservation, partial reads, EOF, replay revalidation");
}
