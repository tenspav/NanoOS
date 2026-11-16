#include "disk.h"
int ns_disk_init(struct ns_disk *d,const struct ns_sftl *map,ns_page_read fn,
                 void *opaque,uint32_t *data,uint32_t *record) {
    if(!d)return NS_ARGUMENT;
    d->valid=0;
    if(!map||!fn||!data||!record||((uintptr_t)data&3u)||
       ((uintptr_t)record&3u))return NS_ARGUMENT;
    *d=(struct ns_disk){.map=map,.read_page=fn,.opaque=opaque,
        .data=data,.record=record,.cap=1u};
    d->slots[0]=(struct ns_disk_slot){.data=data,.record=record};
    return NS_OK;
}
int ns_disk_set_cache(struct ns_disk *d,uint32_t *data_pages,uint32_t *record_pages,uint32_t pages) {
    if(!d||!data_pages||!record_pages||!pages||pages>NS_DISK_CACHE_MAX||
       ((uintptr_t)data_pages&3u)||((uintptr_t)record_pages&3u))return NS_ARGUMENT;
    ns_disk_invalidate(d);
    d->cap=pages;
    for(uint32_t i=0;i<pages;++i)
        d->slots[i]=(struct ns_disk_slot){.data=data_pages+i*4096u,.record=record_pages+i*16u};
    return NS_OK;
}
void ns_disk_invalidate(struct ns_disk *d){
    if(!d)return;
    d->valid=0;
    for(uint32_t i=0;i<d->cap;++i)d->slots[i].valid=0;
}
static int sector(struct ns_disk *d,uint32_t lba,const unsigned char **data) {
    uint32_t vba;struct ns_address a;
    int rc=ns_sftl_lookup(d->map,lba,&vba);
    if(rc)return rc;
    rc=ns_vfl_address(d->map,vba,&a);if(rc)return rc;
    uint32_t row=(a.cau<<19)|(a.block<<7)|a.page;
    struct ns_disk_slot *s=0;
    for(uint32_t i=0;i<d->cap;++i){
        if(d->slots[i].valid&&d->slots[i].ce==a.ce&&d->slots[i].row==row){s=&d->slots[i];break;}
    }
    if(!s){
        uint32_t victim=0;
        for(uint32_t i=0;i<d->cap;++i){
            if(!d->slots[i].valid){victim=i;break;}
            if(d->slots[i].stamp<d->slots[victim].stamp)victim=i;
        }
        s=&d->slots[victim];
        s->valid=0;++d->reads;
        rc=d->read_page(d->opaque,a.ce,row,s->data,s->record);
        if(rc)return rc;
        s->ce=a.ce;s->row=row;s->valid=1;
    }else ++d->hits;
    s->stamp=++d->clock;
    const uint32_t *m=s->record+a.sector*4;
    uint32_t type=m[0]&255u;
    if((type!=1&&type!=2)||(m[0]&0x200u)||m[1]==0xffffffffu||
       m[2]!=lba||m[3]>m[1]){s->valid=0;return NS_FORMAT;}
    int replay=0;
    for(uint32_t i=d->map->nupdates;i;--i) {
        const struct ns_update *u=d->map->updates+i-1;
        if(u->lba!=lba)continue;
        replay=1;
        if(m[1]!=u->generation||(m[0]>>16)!=u->key){s->valid=0;return NS_CONFLICT;}
        break;
    }
    if(!replay&&(m[1]>d->map->checkpoint_generation||
       (m[1]==d->map->checkpoint_generation&&
        (m[0]>>16)>d->map->checkpoint_key))){s->valid=0;return NS_CONFLICT;}
    *data=(const unsigned char *)(s->data+a.sector*1024);return NS_OK;
}
static void fast_copy(unsigned char *dst,const unsigned char *src,uint32_t n) {
    if((((uintptr_t)dst|(uintptr_t)src)&3u)==0){
        uint32_t *d=(uint32_t *)(void *)dst;
        const uint32_t *s=(const uint32_t *)(const void *)src;
        while(n>=16u){
            d[0]=s[0];d[1]=s[1];d[2]=s[2];d[3]=s[3];
            d+=4;s+=4;n-=16u;
        }
        while(n>=4u){*d++=*s++;n-=4u;}
        dst=(unsigned char *)d;src=(const unsigned char *)s;
    }
    while(n--)*dst++=*src++;
}
int ns_disk_read(struct ns_disk *d,uint32_t lba,void *out) {
    if(!d||!out)return NS_ARGUMENT;
    const unsigned char *src;int rc=sector(d,lba,&src);if(rc)return rc;
    fast_copy(out,src,4096u);
    return NS_OK;
}
int ns_disk_pread(struct ns_disk *d,uint64_t off,void *out,uint32_t n,uint32_t *done) {
    if(!done)return NS_ARGUMENT;
    *done=0;
    if(!d||(!out&&n))return NS_ARGUMENT;
    if(!d->map->ready)return NS_NOT_READY;
    uint64_t size=(uint64_t)d->map->sectors<<12;
    if(off>=size)return NS_OK;
    if(size-off<n)n=(uint32_t)(size-off);
    unsigned char *dst=out;
    while(*done<n) {
        const unsigned char *src;int rc=sector(d,(uint32_t)(off>>12),&src);
        if(rc)return rc;
        uint32_t within=(uint32_t)off&4095u,count=4096u-within;
        if(count>n-*done)count=n-*done;
        fast_copy(dst+*done,src+within,count);
        off+=count;*done+=count;
    }
    return NS_OK;
}
