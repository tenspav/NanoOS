#define _FILE_OFFSET_BITS 64
#include "hfs.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sha256.h"
struct source {FILE *file;unsigned fail;};
static int read_bytes(void *opaque,uint64_t off,void *out,uint32_t n){
 struct source *s=opaque;if(s->fail)return -7;
 if(off>262144||n>262144-off)return -7;
 if(fseeko(s->file,(off_t)off,SEEK_SET)||fread(out,1,n,s->file)!=n)return -7;
 return 0;
}
static struct ns_hfs fs;static unsigned char cat[32768],ext[32768],data[50208];
int main(int argc,char **argv){
 assert(argc>=2);struct source s={fopen(argv[1],"rb"),0};assert(s.file);
 int rc=ns_hfs_mount(&fs,read_bytes,&s,0,262144,cat,ext);struct ns_file f;
 if(!rc)rc=ns_hfs_open(&fs,"/Apps/Icons/NanoOS.bin",&f);
 uint32_t done=0;
 if(!rc)rc=ns_hfs_pread(&fs,&f,0,data,sizeof data,&done);
 if(argc>2){assert(rc<0);fclose(s.file);puts("PASS rejected malformed HFS metadata");return 0;}
 assert(!rc&&done==sizeof data&&f.fork.size==sizeof data);
 for(unsigned i=0;i<sizeof data;++i)assert(data[i]==(unsigned char)(i*13+(i/4096)*17+11));
 struct sha256 hash;unsigned char digest[32];sha_init(&hash);sha_update(&hash,data,sizeof data);sha_final(&hash,digest);
 for(unsigned i=0;i<32;++i)printf("%02x",digest[i]);puts("");
 assert(ns_hfs_open(&fs,"/missing",&f)==NS_NOT_FOUND);
 assert(ns_hfs_open(&fs,"/Apps/../Icons/NanoOS.bin",&f)==NS_UNSUPPORTED);
 assert(ns_hfs_open(&fs,"/Apps/Icons/NanoOS.bin/",&f)==NS_NOT_FILE);
 assert(!ns_hfs_open(&fs,"/apps/icons/nanoos.BIN",&f));
 assert(!ns_hfs_pread(&fs,&f,50200,data,100,&done)&&done==8);
 assert(!ns_hfs_pread(&fs,&f,50208,data,100,&done)&&!done);
 s.fail=1;assert(ns_hfs_pread(&fs,&f,0,data,100,&done)==-7&&!done);
 fclose(s.file);
}
