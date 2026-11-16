/* Independent implementation of Apple's public TN1150 on-disk format.
 * All disk values are decoded bytewise; no packed/unaligned native loads. */
#include "hfs.h"
static uint32_t be16(const unsigned char *p){return (uint32_t)p[0]<<8|p[1];}
static uint32_t be32(const unsigned char *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static uint64_t be64(const unsigned char *p){return (uint64_t)be32(p)<<32|be32(p+4);}
static int extents(struct ns_hfs *v,const unsigned char *p,struct ns_extent *e){
 int zero=0;
 for(unsigned k=0;k<8;++k){uint32_t start=be32(p+k*8),n=be32(p+k*8+4);
  if(!n){if(start)return NS_FORMAT;zero=1;}else if(zero||start>=v->blocks||n>v->blocks-start)return NS_FORMAT;
  e[k]=(struct ns_extent){start,n};
 }
 return 0;
}
static int fork_data(struct ns_hfs *v,const unsigned char *p,uint32_t id,struct ns_fork *out){
 struct ns_fork f={.size=be64(p),.blocks=be32(p+12),.id=id};
 if(f.blocks>v->blocks||f.size>((uint64_t)f.blocks<<v->shift))return NS_FORMAT;
 int rc=extents(v,p+16,f.extent);if(rc)return rc;
 uint32_t count=0;for(unsigned k=0;k<8;++k){if(f.extent[k].count>f.blocks-count)return NS_FORMAT;count+=f.extent[k].count;}
 if(f.size&&!count)return NS_FORMAT;
 *out=f;return 0;
}
static int fork_read(struct ns_hfs *,const struct ns_fork *,uint64_t,void *,uint32_t);
static int header(struct ns_hfs *v,const struct ns_fork *f,struct ns_tree *t){
 unsigned char b[512];int rc=fork_read(v,f,0,b,512);if(rc)return rc;
 struct ns_tree tree={be32(b+24),be32(b+28),be32(b+36),be16(b+32),be32(b+16)};
 if(b[8]!=1||be16(b+10)!=3||tree.size<512||tree.size>32768||
    (tree.size&(tree.size-1))||!tree.nodes||tree.first>=tree.nodes||tree.last>=tree.nodes||
    tree.root>=tree.nodes||(uint64_t)tree.nodes*tree.size>f->size)return NS_FORMAT;
 if(!tree.root&&tree.first)tree.root=tree.first;
 *t=tree;return 0;
}
static int leaf(struct ns_hfs *v,const struct ns_fork *f,const struct ns_tree *t,uint32_t index,
                 uint32_t previous,unsigned char *b,uint32_t *count,uint32_t *next){
 if(!index||index>=t->nodes)return NS_FORMAT;
 int rc=fork_read(v,f,(uint64_t)index*t->size,b,t->size);if(rc)return rc;
 uint32_t records=be16(b+10),forward=be32(b);
 if(b[8]!=255||b[9]!=1||be32(b+4)!=previous||forward>=t->nodes||
    records>(t->size-16)/2)return NS_FORMAT;
 uint32_t last=14,table=t->size-(records+1)*2;
 for(unsigned k=0;k<=records;++k){uint32_t off=be16(b+t->size-(k+1)*2);
  if(off<last||off>table||(k&&off==last))return NS_FORMAT;
  last=off;}
 *count=records;*next=forward;return 0;
}
static int find_overflow(struct ns_hfs *v,const struct ns_fork *f,uint32_t logical,
                          struct ns_extent *extent,uint32_t *first){
 if(!v->ext_tree.first)return NS_FORMAT;
 uint32_t node=v->ext_tree.first,prev=0,steps=0;
 while(node){if(++steps>v->ext_tree.nodes)return NS_FORMAT;uint32_t count,next;
  int rc=leaf(v,&v->overflow,&v->ext_tree,node,prev,v->ext_node,&count,&next);if(rc)return rc;
  for(unsigned k=0;k<count;++k){uint32_t start=be16(v->ext_node+v->ext_tree.size-(k+1)*2),end=be16(v->ext_node+v->ext_tree.size-(k+2)*2);
   const unsigned char *p=v->ext_node+start;
   if(end-start<76||be16(p)!=10)return NS_FORMAT;
   if(p[2]!=f->type||be32(p+4)!=f->id)continue;
   uint32_t base=be32(p+8);if(base>logical)continue;
   if(base>=f->blocks)return NS_FORMAT;
   rc=extents(v,p+12,extent);if(rc)return rc;
   uint32_t sum=0;for(unsigned j=0;j<8;++j){if(extent[j].count>f->blocks-sum)return NS_FORMAT;sum+=extent[j].count;}
   if(sum>f->blocks-base)return NS_FORMAT;
   if(logical-base<sum){*first=base;return 0;}
  }
  prev=node;node=next;
 }
 return NS_FORMAT;
}
static int fork_position(struct ns_hfs *v,const struct ns_fork *f,uint64_t off,uint64_t *position,uint32_t *available){
 if(off>=f->size)return NS_FORMAT;
 uint32_t logical=(uint32_t)(off>>v->shift),within=(uint32_t)off&(v->block_size-1),base=0;struct ns_extent e[8];
 for(unsigned k=0;k<8;++k)e[k]=f->extent[k];
 uint32_t count=0;for(unsigned k=0;k<8;++k)count+=e[k].count;
 if(logical>=count){if(f->id==3)return NS_UNSUPPORTED;
  int rc=find_overflow(v,f,logical,e,&base);if(rc)return rc;
 }
 unsigned k=0;for(;k<8;++k){if(logical-base<e[k].count)break;base+=e[k].count;}
 if(k==8)return NS_FORMAT;
 uint32_t physical=e[k].start+logical-base,bytes=v->block_size-within;
 if(physical>=v->blocks)return NS_FORMAT;
 uint64_t pos=((uint64_t)physical<<v->shift)+within;
 if(pos>v->bytes||bytes>v->bytes-pos)return NS_FORMAT;
 *position=v->offset+pos;*available=bytes;return 0;
}
static int fork_read(struct ns_hfs *v,const struct ns_fork *f,uint64_t off,void *out,uint32_t n){
 if(off>f->size||n>f->size-off)return NS_FORMAT;
 unsigned char *dst=out;uint32_t done=0;
 while(done<n){uint64_t position;uint32_t bytes;int rc=fork_position(v,f,off,&position,&bytes);if(rc)return rc;
  if(bytes>n-done)bytes=n-done;
  rc=v->read(v->opaque,position,dst+done,bytes);if(rc)return rc;
  off+=bytes;done+=bytes;
 }
 return 0;
}
int ns_hfs_sector(struct ns_hfs *v,const struct ns_file *f,uint64_t off,uint32_t *lba){
 if(!v||!f||!lba||(off&4095u))return NS_ARGUMENT;
 if(!v->mounted)return NS_NOT_READY;
 if(f->kind!=2)return NS_NOT_FILE;
 if(off>f->fork.size||4096>f->fork.size-off)return NS_ARGUMENT;
 uint64_t first=0;uint32_t done=0;
 while(done<4096){uint64_t pos;uint32_t n;int rc=fork_position(v,&f->fork,off+done,&pos,&n);if(rc)return rc;
  if(!done){first=pos;if((first&4095u)||(first>>12)>UINT32_MAX)return NS_UNSUPPORTED;}
  if(pos!=first+done)return NS_UNSUPPORTED;
  if(n>4096-done)n=4096-done;
  done+=n;
 }
 *lba=(uint32_t)(first>>12);return 0;
}
int ns_hfs_mount(struct ns_hfs *v,ns_bytes_read read,void *opaque,uint64_t offset,uint64_t bytes,unsigned char *cat,unsigned char *ext){
 if(!v)return NS_ARGUMENT;
 *v=(struct ns_hfs){0};
 if(!read||!cat||!ext||cat==ext||bytes<1536||offset>UINT64_MAX-bytes)return NS_ARGUMENT;
 unsigned char b[512];int rc=read(opaque,offset+1024,b,512);if(rc)return rc;
 uint32_t sig=be16(b),version=be16(b+2),block=be32(b+40),blocks=be32(b+44);
 if(!((sig==0x482b&&version==4)||(sig==0x4858&&version==5)))return NS_UNSUPPORTED;
 if(block<512||block>0x100000u||(block&(block-1))||!blocks||(uint64_t)blocks*block>bytes)return NS_FORMAT;
 v->read=read;v->opaque=opaque;v->offset=offset;v->bytes=bytes;v->block_size=block;v->blocks=blocks;v->attributes=be32(b+4);v->cat_node=cat;v->ext_node=ext;
 while((1u<<v->shift)!=block)++v->shift;
 rc=fork_data(v,b+192,3,&v->overflow);if(rc)return rc;
 rc=fork_data(v,b+272,4,&v->catalog);if(rc)return rc;
 rc=header(v,&v->overflow,&v->ext_tree);if(rc)return rc;
 rc=header(v,&v->catalog,&v->cat_tree);if(rc)return rc;
 /* Initial name matching is the ASCII subset of HFS+ case folding. HFSX
  * has additional comparison modes; reject it until those are implemented. */
 if(sig==0x4858)return NS_UNSUPPORTED;
 v->mounted=1;return 0;
}
static unsigned fold(unsigned c){return c>='A'&&c<='Z'?c+32:c;}
static int read_cat_node(struct ns_hfs *v,uint32_t index,uint32_t *count){
 const struct ns_tree *t=&v->cat_tree;
 if(!index||index>=t->nodes)return NS_FORMAT;
 int rc=fork_read(v,&v->catalog,(uint64_t)index*t->size,v->cat_node,t->size);if(rc)return rc;
 const unsigned char *b=v->cat_node;
 uint32_t records=be16(b+10),forward=be32(b),backward=be32(b+4);
 if((b[8]!=0&&b[8]!=255)||(b[8]==0&&b[9]<2)||(b[8]==255&&b[9]!=1)||
    forward>=t->nodes||backward>=t->nodes||records>(t->size-16)/2)return NS_FORMAT;
 uint32_t last=14,table=t->size-(records+1)*2;
 for(unsigned k=0;k<=records;++k){uint32_t off=be16(b+t->size-(k+1)*2);
  if(off<last||off>table||(k&&off==last))return NS_FORMAT;
  last=off;}
 for(unsigned k=0;k<records;++k){
  uint32_t start=be16(b+t->size-(k+1)*2),end=be16(b+t->size-(k+2)*2),bytes=end-start;
  if(bytes<10)return NS_FORMAT;
  uint32_t key=be16(b+start),chars=be16(b+start+6);
  if(chars>255||key!=6+chars*2||key+4>bytes)return NS_FORMAT;
 }
 *count=records;return 0;
}
static int cmp_cat_key(const unsigned char *p,uint32_t bytes,uint32_t parent,const char *name,unsigned len,uint32_t *key_out){
 if(bytes<10)return -2;
 uint32_t key=be16(p),chars=be16(p+6);
 if(chars>255||key!=6+chars*2||key+2>bytes)return -2;
 *key_out=key;
 uint32_t rp=be32(p+2);
 if(rp<parent)return -1;
 if(rp>parent)return 1;
 unsigned n=chars<len?chars:len;
 for(unsigned j=0;j<n;++j){unsigned a=fold(be16(p+8+j*2)),b=fold((unsigned char)name[j]);if(a<b)return -1;if(a>b)return 1;}
 if(chars<len)return -1;
 if(chars>len)return 1;
 return 0;
}
static int lookup(struct ns_hfs *v,uint32_t parent,const char *name,unsigned len,struct ns_file *out){
 uint32_t node=v->cat_tree.root,steps=0;
 if(!node)return NS_NOT_FOUND;
 while(node){if(++steps>v->cat_tree.nodes)return NS_FORMAT;uint32_t count;
  int rc=read_cat_node(v,node,&count);if(rc)return rc;
  if(v->cat_node[8]==0){
   if(!count)return NS_NOT_FOUND;
   uint32_t lo=0,hi=count;int best=-1;uint32_t best_start=0,best_key=0,best_bytes=0;
   while(lo<hi){uint32_t mid=lo+(hi-lo)/2;
    uint32_t start=be16(v->cat_node+v->cat_tree.size-(mid+1)*2),end=be16(v->cat_node+v->cat_tree.size-(mid+2)*2);
    uint32_t bytes=end-start,key=0;
    int c=cmp_cat_key(v->cat_node+start,bytes,parent,name,len,&key);
    if(c==-2||key+6>bytes)return NS_FORMAT;
    if(c<=0){best=(int)mid;best_start=start;best_key=key;best_bytes=bytes;lo=mid+1;}else{hi=mid;}
   }
   if(best<0||best_key+6>best_bytes)return NS_NOT_FOUND;
   node=be32(v->cat_node+best_start+2+best_key);
   continue;
  }
  uint32_t lo=0,hi=count;
  while(lo<hi){uint32_t mid=lo+(hi-lo)/2;
   uint32_t start=be16(v->cat_node+v->cat_tree.size-(mid+1)*2),end=be16(v->cat_node+v->cat_tree.size-(mid+2)*2);
   const unsigned char *p=v->cat_node+start;uint32_t bytes=end-start,key=0;
   int c=cmp_cat_key(p,bytes,parent,name,len,&key);
   if(c==-2)return NS_FORMAT;
   if(c<0){lo=mid+1;}else if(c>0){hi=mid;}else{
    if(key+4>bytes)return NS_FORMAT;
    const unsigned char *r=p+key+2;uint32_t type=be16(r),record_bytes=bytes-key-2;
    struct ns_file f={0};
    if(type==1){if(record_bytes<88)return NS_FORMAT;f.kind=1;f.id=be32(r+8);}
    else if(type==2){if(record_bytes<248)return NS_FORMAT;f.kind=2;f.id=be32(r+8);
     uint32_t mode=be16(r+42)&0xf000;if((mode&&mode!=0x8000)||be32(r+48)==0x686c6e6b)return NS_UNSUPPORTED;
     rc=fork_data(v,r+88,f.id,&f.fork);if(rc)return rc;
    }else return NS_NOT_FOUND;
    if(f.id<2)return NS_FORMAT;
    *out=f;return 0;
   }
  }
  return NS_NOT_FOUND;
 }
 return NS_NOT_FOUND;
}
int ns_hfs_open(struct ns_hfs *v,const char *path,struct ns_file *out){
 if(!v||!path||!out||path[0]!='/')return NS_ARGUMENT;
 if(!v->mounted)return NS_NOT_READY;
 struct ns_file current={.id=2,.kind=1};unsigned total=0;
 while(*path){while(*path=='/')++path;if(!*path)break;const char *start=path;unsigned len=0;
  while(*path&&*path!='/'){if((unsigned char)*path>=128)return NS_UNSUPPORTED;if(++total>4096||++len>255)return NS_ARGUMENT;++path;}
  if((len==1&&start[0]=='.')||(len==2&&start[0]=='.'&&start[1]=='.'))return NS_UNSUPPORTED;
  if(current.kind!=1)return NS_NOT_FILE;
  int rc=lookup(v,current.id,start,len,&current);if(rc)return rc;
  if(current.kind!=1&&*path)return NS_NOT_FILE;
 }
 *out=current;return 0;
}
int ns_hfs_pread(struct ns_hfs *v,const struct ns_file *file,uint64_t off,void *out,uint32_t n,uint32_t *done){
 if(!done)return NS_ARGUMENT;
 *done=0;
 if(!v||!file||(!out&&n))return NS_ARGUMENT;
 if(!v->mounted)return NS_NOT_READY;
 if(file->kind!=2)return NS_NOT_FILE;
 if(off>=file->fork.size)return 0;
 if(file->fork.size-off<n)n=(uint32_t)(file->fork.size-off);
 while(*done<n){
  uint32_t count=v->block_size-((uint32_t)off&(v->block_size-1));
  if(count>n-*done)count=n-*done;
  int rc=fork_read(v,&file->fork,off,(unsigned char *)out+*done,count);
  if(rc)return rc;
  off+=count;*done+=count;
 }
 return 0;
}
