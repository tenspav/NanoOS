#include "save.h"
static uint32_t get(const unsigned char *b){return b[0]|(uint32_t)b[1]<<8|(uint32_t)b[2]<<16|(uint32_t)b[3]<<24;}
static void put(unsigned char *b,uint32_t v){for(unsigned k=0;k<4;++k)b[k]=(unsigned char)(v>>(k*8));}
static uint32_t crc(const unsigned char *b,uint32_t n,int header){
 uint32_t c=0xffffffffu;for(unsigned i=0;i<n;++i){c^=header&&i>=32&&i<36?0:b[i];for(unsigned k=0;k<8;++k)c=(c>>1)^(0xedb88320u&(0u-(c&1u)));}return ~c;
}
static int overlap(uintptr_t a,uintptr_t n,uintptr_t b,uintptr_t m){return a>UINTPTR_MAX-n||b>UINTPTR_MAX-m||(a<b+m&&b<a+n);}
static int private_alias(const struct ns_save *s,const void *p,uint32_t n){
 uintptr_t a=(uintptr_t)p;
 return overlap(a,n,(uintptr_t)s,sizeof *s)||overlap(a,n,(uintptr_t)s->slots,32768)||
 overlap(a,n,(uintptr_t)s->disk->data,16384)||overlap(a,n,(uintptr_t)s->disk->record,64)||
 overlap(a,n,(uintptr_t)s->fs->cat_node,32768)||overlap(a,n,(uintptr_t)s->fs->ext_node,32768);
}
static int envelope(struct ns_save *s,unsigned slot,uint64_t *sequence,uint32_t *length){
 const unsigned char *b=(const unsigned char *)s->slots+slot*16384;
 static const unsigned char magic[8]={'N','G','S','A','V','E','1',0};
 for(unsigned i=0;i<8;++i)if(b[i]!=magic[i])return 0;
 if(get(b+8)!=s->game||get(b+12)!=s->schema||get(b+24)>NS_SAVE_MAX_BYTES||get(b+32)!=crc(b,64,1))return 0;
 for(unsigned i=36;i<64;++i)if(b[i])return 0;
 uint32_t n=get(b+24);if(get(b+28)!=crc(b+64,n,0))return 0;
 for(unsigned i=64+n;i<16384;++i)if(b[i])return 0;
 *sequence=(uint64_t)get(b+16)|(uint64_t)get(b+20)<<32;*length=n;return *sequence!=0;
}
static int scan(struct ns_save *s){
 ns_disk_invalidate(s->disk);uint32_t done;int error[2]={0,0};uint64_t seq[2];uint32_t len[2];int good[2]={0,0};s->faulted=s->epoch_missing;
 for(unsigned slot=0;slot<2;++slot){int rc=ns_hfs_pread(s->fs,&s->file,slot*16384u,s->slots+slot*4096u,16384,&done);
  error[slot]=rc?rc:done!=16384?NS_IO:0;
  if(!error[slot])good[slot]=envelope(s,slot,seq+slot,len+slot);else s->faulted=1;
 }
 int blank[2]={1,1};
 for(unsigned slot=0;slot<2;++slot){const unsigned char *b=(const unsigned char *)s->slots+slot*16384;unsigned value=b[0];
  if(error[slot]||(value!=0&&value!=255))blank[slot]=0;
  for(unsigned i=0;i<16384&&blank[slot];++i)if(b[i]!=value)blank[slot]=0;
 }
 if(!good[0]&&!good[1]){s->sequence=0;s->length=0;s->active=1;s->recovered=0;return error[0]?error[0]:error[1]?error[1]:blank[0]&&blank[1]?NS_NOT_FOUND:NS_FORMAT;}
 if(good[0]&&good[1]&&seq[0]==seq[1])return NS_CONFLICT;
 unsigned which=good[0]&&(!good[1]||seq[0]>seq[1])?0:1;
 s->active=which;s->sequence=seq[which];s->length=len[which];s->recovered=!good[1-which]&&!blank[1-which];return 0;
}
int ns_save_open(struct ns_save *s,struct ns_hfs *fs,struct ns_disk *disk,struct ns_append *append,
 const char *path,uint32_t game,uint32_t schema,uint32_t *slots){
 if(!s||!fs||!disk||!disk->map||!append||!path||!game||!slots||((uintptr_t)slots&3u))return NS_ARGUMENT;
 if(overlap((uintptr_t)slots,32768,(uintptr_t)s,sizeof *s)||
    overlap((uintptr_t)slots,32768,(uintptr_t)disk->data,16384)||overlap((uintptr_t)slots,32768,(uintptr_t)disk->record,64)||
    overlap((uintptr_t)slots,32768,(uintptr_t)fs->cat_node,32768)||overlap((uintptr_t)slots,32768,(uintptr_t)fs->ext_node,32768))return NS_ARGUMENT;
 *s=(struct ns_save){.fs=fs,.disk=disk,.append=append,.game=game,.schema=schema,.slots=slots};
 int rc=ns_hfs_open(fs,path,&s->file);if(rc)return rc;
 if(s->file.kind!=2||s->file.fork.size!=32768)return NS_ARGUMENT;
 for(unsigned i=0;i<8;++i){rc=ns_hfs_sector(fs,&s->file,i*4096u,s->lba+i);if(rc)return rc;
  for(unsigned j=0;j<i;++j)if(s->lba[i]==s->lba[j])return NS_CONFLICT;
  rc=ns_disk_read(disk,s->lba[i],slots);if(rc){s->epoch_missing=1;continue;}
  uint32_t vba;rc=ns_sftl_lookup(disk->map,s->lba[i],&vba);if(rc)return rc;
  s->epoch[i]=disk->record[(vba&3u)*4u+3];
 }
 rc=scan(s);if(rc!=0&&rc!=NS_NOT_FOUND)return rc;
 s->opened=1;return rc==NS_NOT_FOUND?NS_SAVE_EMPTY:0;
}
int ns_save_load(struct ns_save *s,void *out,uint32_t capacity,uint32_t *length){
 if(!length)return NS_ARGUMENT;
 if(s&&s->opened&&(private_alias(s,length,4)||private_alias(s,out,capacity)))return NS_ARGUMENT;
 *length=0;if(!s||!s->opened||(!out&&capacity))return NS_ARGUMENT;
 int rc=scan(s);if(rc)return rc;
 *length=s->length;if(capacity<s->length)return NS_SPACE;
 const unsigned char *b=(const unsigned char *)s->slots+s->active*16384+64;unsigned char *dst=out;
 for(unsigned i=0;i<s->length;++i)dst[i]=b[i];
 return 0;
}
int ns_save_store(struct ns_save *s,const void *payload,uint32_t n){
 if(!s||!s->opened||(!payload&&n)||n>NS_SAVE_MAX_BYTES)return NS_ARGUMENT;
 if(private_alias(s,payload,n))return NS_ARGUMENT;
 if(!s->append->active||s->append->writer->map!=s->disk->map)return NS_NOT_READY;
 struct ns_writer *w=s->append->writer;
 if(overlap((uintptr_t)s->slots,32768,(uintptr_t)w->stage,16384)||overlap((uintptr_t)s->slots,32768,(uintptr_t)w->scratch,16384)||
    overlap((uintptr_t)s->slots,32768,(uintptr_t)w->stage_record,64)||overlap((uintptr_t)s->slots,32768,(uintptr_t)w->scratch_record,64))return NS_ARGUMENT;
 int rc=scan(s);if(rc&&rc!=NS_NOT_FOUND)return rc;
 if(s->faulted)return NS_NOT_READY;
 if(s->sequence==UINT64_MAX)return NS_SPACE;
 uint64_t seq=s->sequence+1;unsigned target=1-s->active;unsigned char *b=(unsigned char *)s->slots+target*16384;
 for(unsigned i=0;i<16384;++i)b[i]=0;
 static const unsigned char magic[8]={'N','G','S','A','V','E','1',0};for(unsigned i=0;i<8;++i)b[i]=magic[i];
 put(b+8,s->game);put(b+12,s->schema);put(b+16,(uint32_t)seq);put(b+20,(uint32_t)(seq>>32));put(b+24,n);
 for(unsigned i=0;i<n;++i)b[64+i]=((const unsigned char *)payload)[i];
 put(b+28,crc(b+64,n,0));put(b+32,crc(b,64,1));
 uint32_t expected_crc=crc(b,16384,0);
 rc=ns_append_page(s->append,s->lba+target*4,s->epoch+target*4,b);if(rc)return rc;
 ns_disk_invalidate(s->disk);uint32_t done;
 rc=ns_hfs_pread(s->fs,&s->file,target*16384u,b,16384,&done);
 uint64_t verified_seq;uint32_t verified_len;
 if(rc||done!=16384||crc(b,16384,0)!=expected_crc||!envelope(s,target,&verified_seq,&verified_len)||verified_seq!=seq||verified_len!=n){
  s->append->poisoned=1;w->poisoned=1;w->map->ready=0;return rc?rc:NS_VERIFY;
 }
 s->active=target;s->sequence=seq;s->length=n;s->recovered=0;return 0;
}

int ns_save_reclaim(struct ns_save *s,uint32_t budget,uint32_t *collected){
 if(!collected)return NS_ARGUMENT;
 *collected=0;if(!s||!s->opened||!budget)return NS_ARGUMENT;
 if(s->faulted)return NS_NOT_READY;
 struct ns_append *a=s->append;if(!a->active||!a->writer)return NS_NOT_READY;
 if(!a->writer->io.erase)return NS_UNSUPPORTED;
 struct ns_survivor proof[8];struct ns_collect g;uint32_t record[16];
 for(unsigned b=4;b<a->writer->map->blocks;++b)if(b!=a->writer->block&&a->leases[b]==10){
  int rc=ns_sftl_ready_block(a->writer->map,b,a->states[b]);if(rc==NS_CONFLICT)continue;if(rc)return rc;
  rc=ns_append_collect(a,&g,b,proof,8,s->slots,record,budget);if(rc)return rc;
  ++*collected;ns_disk_invalidate(s->disk);
 }
 return 0;
}
