#include "game.h"
static int external(struct ns_game *g,const void *p,uint32_t n){
 uintptr_t a=(uintptr_t)p,b=(uintptr_t)g;return a<=UINTPTR_MAX-n&&(!n||a>=b+sizeof *g||a+n<=b);
}
static int read_bytes(void *p,uint64_t off,void *out,uint32_t n){
 struct ns_game *g=p;uint32_t done;int rc=ns_disk_pread(&g->disk,off,out,n,&done);return rc?rc:done==n?0:NS_IO;
}
int ns_game_mount(struct ns_game *g,const struct ns_mount_io *read,const struct ns_write_io *write,uint32_t budget){
 if(!g||!read||!budget||!read->inspect||!read->context)return NS_ARGUMENT;
 if(write&&(write->inspect!=read->inspect||write->opaque!=read->opaque))return NS_ARGUMENT;
 struct ns_mount_io ri=*read;struct ns_write_io wi=write?*write:(struct ns_write_io){0};
 /* The configurations may be fields of a previous instance; copy them first. */
 unsigned char *b=(unsigned char *)g;for(unsigned i=0;i<sizeof *g;++i)b[i]=0;
 g->io=wi;g->budget_us=budget;
 uint32_t began=ri.usec?ri.usec(ri.opaque):0;
 int rc=ns_mount_n31(&g->mount,&ri,g->mask,g->states,g->fg,g->fk,g->page,g->record,g->runs,NS_GAME_RUNS,g->updates,NS_GAME_UPDATES,budget);
 g->strict_mount_rc=rc;
 if(rc==NS_IO||rc==NS_FORMAT){uint32_t elapsed=ri.usec?ri.usec(ri.opaque)-began:0;if(elapsed>=budget)return NS_TIMEOUT;
  rc=ns_mount_n31_recover(&g->mount,&ri,g->mask,g->states,g->fg,g->fk,g->page,g->record,g->runs,NS_GAME_RUNS,g->updates,NS_GAME_UPDATES,budget-elapsed);
 }
 if(rc)return rc;
 rc=ns_disk_init(&g->disk,&g->mount.map,ns_mount_page_read,&g->mount,g->cache,g->cache_record);if(rc)return rc;
 rc=ns_disk_set_cache(&g->disk,g->cache,g->cache_record,16u);if(rc)return rc;
 uint64_t prefix=(uint64_t)0xc040<<12,size=(uint64_t)g->mount.map.sectors<<12,off,bytes;
 if(size<=prefix)return NS_FORMAT;
 rc=ns_partition_hfs(read_bytes,g,prefix,size-prefix,&off,&bytes);if(rc)return rc;
 rc=ns_hfs_mount(&g->fs,read_bytes,g,off,bytes,g->cat,g->ext);if(rc)return rc;
 g->mounted=1;return 0;
}
int ns_game_open(struct ns_game *g,const char *path,struct ns_file *file){
 if(!g||!g->mounted)return NS_NOT_READY;
 if(!file||!external(g,file,sizeof *file))return NS_ARGUMENT;
 return ns_hfs_open(&g->fs,path,file);
}
int ns_game_read(struct ns_game *g,const struct ns_file *file,uint64_t off,void *out,uint32_t n,uint32_t *done){
 if(g&&done&&!external(g,done,4))return NS_ARGUMENT;
 if(done)*done=0;
 if(!g||!g->mounted)return NS_NOT_READY;
 if(!external(g,out,n))return NS_ARGUMENT;
 return ns_hfs_pread(&g->fs,file,off,out,n,done);
}
int ns_game_save_open(struct ns_game *g,const char *path,uint32_t game,uint32_t schema){
 if(!g||!g->mounted)return NS_NOT_READY;
 /* Changing ownership after an append session starts is intentionally refused. */
 if(g->append.active||g->save.opened)return NS_NOT_READY;
 return ns_save_open(&g->save,&g->fs,&g->disk,&g->append,path,game,schema,g->slots);
}
int ns_game_save_load(struct ns_game *g,void *out,uint32_t n,uint32_t *length){
 if(g&&length&&!external(g,length,4))return NS_ARGUMENT;
 if(length)*length=0;
 if(!g||!g->mounted)return NS_NOT_READY;
 if(!external(g,out,n))return NS_ARGUMENT;
 return ns_save_load(&g->save,out,n,length);
}
int ns_game_save_store(struct ns_game *g,const void *payload,uint32_t n){
 if(!g||!g->mounted||!g->save.opened)return NS_NOT_READY;
 if(g->mount.recovered_pages||g->save.faulted)return NS_NOT_READY;
 if(!g->io.program)return NS_UNSUPPORTED;
 if(!payload&&n)return NS_ARGUMENT;
 if(n>NS_SAVE_MAX_BYTES||!external(g,payload,n))return NS_ARGUMENT;
 if(!g->append.active){int rc=ns_append_open(&g->append,&g->writer,&g->mount.map,g->states,g->leases,sizeof g->leases,&g->io,g->stage,g->stage_record,g->scratch,g->scratch_record,g->budget_us,0);if(rc)return rc;}
 if(g->reclaim_enabled&&g->io.erase){uint32_t collected;int rc=ns_save_reclaim(&g->save,g->budget_us,&collected);g->reclaimed+=collected;if(rc)return rc;}
 return ns_save_store(&g->save,payload,n);
}
