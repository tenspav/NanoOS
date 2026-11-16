#include "gl2nano.h"
#include "memmap.h"
static gl2_nano *owner;
static float software_depth[240*432];
static int owned(gl2_nano *b) { return b && owner==b && b->opened; }
static int deny(void *u) { (void)u; return -1; }
static int deny_submit(const uint32_t *p,uint32_t n,void *u) { (void)p;(void)n;(void)u;return -1; }
static int gpu_range(uintptr_t p,uint32_t n) {
    return !(p&3u) && p>=MM_GAME_GPU && p<=MM_GAME_GPU_END && n<=MM_GAME_GPU_END-p;
}
static int overlaps(uintptr_t a,uint32_t an,uintptr_t b,uint32_t bn) {
    return a<b+bn && b<a+an;
}
static int display_busy(gl2_nano *b,const uint32_t *target,uint32_t bytes){
 return NGAME_API_TABLE(b->api)>=23&&b->api->lcd_buffer_busy&&b->api->lcd_buffer_busy(target,bytes);
}
static int nano_read(struct gl2_ctx *c,int x,int y,int w,int h,uint32_t format,uint32_t type,uint32_t pack,void *pixels,void *user){
 gl2_nano *b=user;(void)c;(void)type;
 if(!owned(b)||b->fault||b->cpu_active||b->frame_open||b->pending_valid||!b->completed||!b->target)return -1;
 if(x<0||y<0||w<0||h<0||(uint32_t)x> b->width||(uint32_t)y>b->height||(uint32_t)w>b->width-(uint32_t)x||(uint32_t)h>b->height-(uint32_t)y)return -1;
 uint32_t bpp=format==GL_RGBA?4u:3u,pitch=(uint32_t)w*bpp;if(pitch&(pack-1u))pitch=(pitch+pack-1u)&~(pack-1u);
 uint8_t *out=(uint8_t *)pixels;
 for(int gy=0;gy<h;++gy){
  const uint32_t *src=b->target+(uint32_t)(b->height-1u-(uint32_t)(y+gy))*b->stride+(uint32_t)x;
  for(int gx=0;gx<w;++gx){uint32_t v=src[gx];out[0]=(uint8_t)(v>>16);out[1]=(uint8_t)(v>>8);out[2]=(uint8_t)v;if(bpp==4)out[3]=(uint8_t)(v>>24);out+=bpp;}
  if(pitch>(uint32_t)w*bpp)out+=(pitch-(uint32_t)w*bpp);
 }
 return 0;
}
static int write_guard(void *user) {
    gl2_nano *b=user;
    return !owned(b) || b->fault || b->cpu_active || b->pending_valid || (b->frame_open && b->frame_draws);
}
static int texture_sync(void *user,void *cpu,uint32_t va,uint32_t bytes) {
    gl2_nano *b=user;
    if (!owned(b) || b->fault || b->cpu_active || b->pending_valid || !gpu_range((uintptr_t)cpu,bytes) ||
        va!=(uint32_t)((uintptr_t)cpu-MM_DRAM)) return -1;
    /* Native game-GPU sections10..31 are Strongly-ordered in NanoOS mmu.c.
     * CPU writes here cannot leave dirty cache lines. Complete writes before
     * GPU consumption; flushing the entire CPU cache adds unrelated work. */
#if (MM_UNCACHED & 0xfffffc00u) != 0xfffffc00u
#error Native texture synchronization requires uncached game-GPU sections
#endif
#if defined(__arm__)
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __sync_synchronize(); /* host mock only */
#endif
    ++b->uploads; return 0;
}
static int submit(const uint32_t *p,uint32_t n,void *user) {
    gl2_nano *b=user;
    if (!owned(b) || !b->frame_open || b->cpu_active || b->fault || !p || !n) {
        if (b) { ++b->errors; if (b->ctx) gl2_invalidate_state_cache(b->ctx); }
        return -1;
    }
    b->api->mbx_submit(p,n); ++b->draws; ++b->frame_draws; b->words+=n; return 0;
}
static int submit_parts(const uint32_t *header,uint32_t nh,const void *vertices,uint32_t nv,void *user){
    gl2_nano *b=user;
    if(!owned(b)||!b->frame_open||b->cpu_active||b->fault||!header||!nh||!vertices||!nv){
        if(b){++b->errors;if(b->ctx)gl2_invalidate_state_cache(b->ctx);}
        return -1;
    }
    b->api->mbx_submit_parts(header,nh,vertices,nv);++b->draws;++b->frame_draws;++b->direct_draws;b->words+=nh+nv;return 0;
}
static int nano_clear(struct gl2_ctx *c,uint32_t mask,uint32_t argb,float depth,uint32_t color_mask,uint32_t scissor_enabled,const int32_t scissor[4],void *user);
static int nano_read(struct gl2_ctx *c,int x,int y,int w,int h,uint32_t format,uint32_t type,uint32_t pack,void *pixels,void *user);
int gl2_nano_open(gl2_nano *b,const struct nanoos_api *api,gl2_ctx *c,
                  gl2_prog_obj *progs,uint32_t *segment,uint32_t cap) {
    /* Test version before accessing appended fields of an older services table. */
    if (!b || !api || !c || !progs || !segment || cap<64 || api->abi!=NGAME_ABI || NGAME_API_TABLE(api)<3) return GL2_NANO_INVALID;
    if (!api->mbx_init || !api->mbx_begin || !api->mbx_submit || !api->mbx_render || !api->mbx_wait_done ||
        !api->mbx_set_background || !api->mbx_halt || !api->mbx_set_target || !api->dcache_clean ||
        !api->lcd_back || !api->lcd_present || !api->lcd_ready) return GL2_NANO_INVALID;
    if (owner) return GL2_NANO_BUSY;
    uint8_t *p=(uint8_t *)b;for(uint32_t i=0;i<sizeof *b;++i)p[i]=0;
    b->api=api;b->ctx=c;
    if (!api->mbx_init()) { api->mbx_halt();b->fault=1;return GL2_NANO_DRIVER; }
    owner=b;b->opened=1;
    gl2_init(c,progs,segment,cap,submit,b);
    gl2_set_state_cache(c,0);
    if(NGAME_API_TABLE(api)>=21&&api->mbx_submit_parts)c->submit_parts=submit_parts;
    c->texture_write_guard=write_guard;c->texture_write_guard_user=b;
    c->clear=nano_clear;c->clear_user=b;
    c->read_pixels=nano_read;c->read_pixels_user=b;
    return GL2_NANO_OK;
}
int gl2_nano_textures(gl2_nano *b,void *arena,uint32_t bytes) {
    if (!owned(b) || b->fault || b->cpu_active || b->frame_open || b->pending_valid) return GL2_NANO_STATE;
    if (bytes<GL2_TEXTURE_ARENA_BYTES || !gpu_range((uintptr_t)arena,bytes) || ((uintptr_t)arena&127u)) return GL2_NANO_TARGET;
    /* Arena API rejects replacement while images are live. Do not consume an
     * application's pre-existing GL error to infer success. */
    for(uint32_t i=0;i<GL2_MAX_TEXTURES;++i)if(b->ctx->textures[i].defined)return GL2_NANO_STATE;
    gl2_texture_arena(b->ctx,arena,(uint32_t)((uintptr_t)arena-MM_DRAM),bytes,texture_sync,b);
    return GL2_NANO_OK;
}
int gl2_nano_software_colour(gl2_nano *b,uint32_t *buffer,uint32_t bytes){
 if(!owned(b)||b->fault||b->cpu_active||b->frame_open||b->pending_valid)return GL2_NANO_STATE;
 uintptr_t p=(uintptr_t)buffer;
 if(buffer&&((p&15u)||p<MM_GAME_HEAP||p>=MM_GAME_END||bytes>MM_GAME_END-p))return GL2_NANO_TARGET;
 b->software_colour=buffer;b->software_colour_bytes=buffer?bytes:0;return GL2_NANO_OK;
}
static void copy_colour(uint32_t *restrict dst,const uint32_t *restrict src,uint32_t count){
 while(count>=4){uint32_t a=src[0],b=src[1],c=src[2],d=src[3];dst[0]=a;dst[1]=b;dst[2]=c;dst[3]=d;dst+=4;src+=4;count-=4;}while(count--)*dst++=*src++;
}
static uint32_t bits(float f){union{float f;uint32_t u;}v={f};return v.u;}
static void submit_clear(gl2_nano *b,uint32_t argb){
 uint32_t clear=(argb&0xff00ff00u)|((argb&255u)<<16)|((argb>>16)&255u);
 uint32_t colour[20]={0x10000073u,0x22200e00u,0xe0000000u,0x2003801au,0x503u,0u,
     0x70000000u,0xb00b0800u,0u,0u,0x3f800000u,0u,0x43f00000u,0u,0x3f800000u,0u,0u,0x44580000u,0x3f800000u,0u};
 colour[11]=colour[15]=colour[19]=clear;colour[12]=bits(2.f*(float)b->width);colour[17]=bits(2.f*(float)b->height);
 b->api->mbx_submit(colour,20);
 gl2_invalidate_state_cache(b->ctx);
}
static int nano_clear(struct gl2_ctx *c,uint32_t mask,uint32_t argb,float depth,uint32_t color_mask,uint32_t scissor_enabled,const int32_t scissor[4],void *user){
 gl2_nano *b=user;(void)c;(void)scissor;
 if(!owned(b)||b->fault||b->cpu_active)return -1;
 if(!b->frame_open||b->software_frame)return -1;
 if(mask!=(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT)||color_mask!=0xfu||scissor_enabled||depth!=1.f)return -1;
 submit_clear(b,argb);return 0;
}
static int begin_target(gl2_nano *b,uint32_t *target,uint32_t w,uint32_t h,uint32_t stride,uint32_t clear_argb,int allow_lcd) {
    if (!owned(b) || b->cpu_active || b->frame_open) return GL2_NANO_STATE;
    if (b->fault) return GL2_NANO_FAULT;
    if(w<16||w>240||h<16||h>432||(w&15)||(h&15)||stride<w||stride>1024||(stride&3))return GL2_NANO_TARGET;
    if(!b->ctx->vp_w||!b->ctx->vp_h||b->ctx->vp_w>w||b->ctx->vp_h>h)return GL2_NANO_TARGET;
    uint32_t bytes=stride*h*4;
    int lcd=target==b->api->lcd_back();
    if (!target || (lcd ? (!allow_lcd||w!=240||h!=432||stride!=240) : !gpu_range((uintptr_t)target,bytes))) return GL2_NANO_TARGET;
    if (lcd && !b->api->lcd_ready()) return GL2_NANO_BUSY;
    if (b->ctx->texture_cpu && overlaps((uintptr_t)target,bytes,(uintptr_t)b->ctx->texture_cpu,GL2_TEXTURE_ARENA_BYTES)) return GL2_NANO_TARGET;
    if(display_busy(b,target,bytes))return GL2_NANO_BUSY;
    if(b->pending_valid&&(lcd||overlaps((uintptr_t)target,bytes,(uintptr_t)b->pending_target,b->pending_bytes)))return GL2_NANO_BUSY;
    uint32_t use_software=!b->force_gpu&&b->ctx->cur&&gl2_program_needs_software(b->ctx,&b->ctx->pr[b->ctx->cur]);
    if(use_software&&b->pending_valid)return GL2_NANO_BUSY;
    if(use_software&&b->software_colour&&b->software_colour_bytes<w*h*4)return GL2_NANO_TARGET;
    if(b->retired_target&&overlaps((uintptr_t)target,bytes,(uintptr_t)b->retired_target,b->retired_stride*b->retired_height*4u))b->retired_target=0;
    b->target=target;b->width=w;b->height=h;b->stride=stride;b->completed=0;b->frame_draws=0;
    b->ctx->soft_target=0;b->ctx->soft_depth=0;b->ctx->diagnostic_gpu=b->force_gpu;
    gl2_set_state_cache(b->ctx,0);
    b->software_frame=use_software;
    if(b->software_frame){b->software_draw_start=b->ctx->soft_draws;b->ctx->soft_target=b->software_colour?b->software_colour:target;b->ctx->soft_depth=software_depth;b->ctx->soft_width=w;b->ctx->soft_height=h;b->ctx->soft_stride=b->software_colour?w:stride;
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){b->ctx->soft_target[y*b->ctx->soft_stride+x]=clear_argb;}
        b->ctx->soft_depth_initialized=0;b->frame_open=1;return GL2_NANO_OK;
    }
    b->api->mbx_set_target(target,w,h,stride);
    uint32_t clear=(clear_argb&0xff00ff00u)|((clear_argb&255u)<<16)|((clear_argb>>16)&255u);
    b->api->mbx_set_background(clear);
    b->api->mbx_begin(); b->frame_open=1;
    static const uint32_t prefix[20]={0x10000073u,0x22200e00u,0xe0000000u,0x2003801au,0x503u,0u,
        0x70020000u,0xb00b0800u,0xc0000000u,0u,0u,0u,0xc0000000u,0x3f800000u,0u,0u,0xbf800000u,0u,0u,0u};
    /* The colour-clear triangle uses ALWAYS with Z writes enabled and z=1
     * at all three vertices. It covers every pixel centre of this target, so
     * it already initializes depth. A second depth-only triangle duplicated
     * the same work. Keep depth initialization independent of application state. */
    b->api->mbx_submit(prefix,20);submit_clear(b,clear_argb);
    gl2_set_state_cache(b->ctx,1);
    return GL2_NANO_OK;
}
int gl2_nano_begin_target(gl2_nano *b,uint32_t *target,uint32_t w,uint32_t h,uint32_t stride,uint32_t colour){return begin_target(b,target,w,h,stride,colour,0);}
int gl2_nano_begin_fbo(gl2_nano *b,uint32_t *surface,uint32_t stride,uint32_t clear_argb){
 if(!owned(b)||b->fault||b->cpu_active||b->frame_open||b->pending_valid)return GL2_NANO_STATE;
 uint32_t texture=0,w=0,h=0;int r=gl2_fbo_target(b->ctx,&texture,&w,&h);if(r)return GL2_NANO_TARGET;
 if(!surface||!w||!h||stride<w)return GL2_NANO_TARGET;
 if(!gpu_range((uintptr_t)surface,stride*h*4u))return GL2_NANO_TARGET;
 r=begin_target(b,surface,w,h,stride,clear_argb,0);if(r)return r;
 b->fbo_texture=texture;b->fbo_width=w;b->fbo_height=h;b->fbo_stride=stride;b->fbo_surface=(uint32_t)(uintptr_t)surface;
 return GL2_NANO_OK;
}
int gl2_nano_end_fbo(gl2_nano *b){
 if(!owned(b)||b->fault||b->cpu_active||!b->frame_open||!b->fbo_texture)return GL2_NANO_STATE;
 uint32_t texture=b->fbo_texture,w=b->fbo_width,h=b->fbo_height,stride=b->fbo_stride;uint32_t *surface=(uint32_t *)(uintptr_t)b->fbo_surface;
 int r=gl2_nano_end(b);if(r)return r;
 b->fbo_texture=b->fbo_width=b->fbo_height=b->fbo_stride=b->fbo_surface=0;
 if(gl2_texture_store_name(b->ctx,texture,0,0,(int)w,(int)h,surface,w,h,stride))return GL2_NANO_DRIVER;
 return GL2_NANO_OK;
}
int gl2_nano_begin(gl2_nano *b,uint32_t *target,uint32_t colour){
    return begin_target(b,target,240,432,240,colour,1);
}
static int queue_fault(gl2_nano *b){
 b->pending_valid=b->frame_open=b->completed=0;b->pending_target=b->retired_target=0;
 b->fault=1;++b->errors;gl2_set_state_cache(b->ctx,0);b->api->mbx_halt();return GL2_NANO_DRIVER;
}
int gl2_nano_finish(gl2_nano *b,uint32_t **completed){
 if(completed)*completed=0;
 if(!owned(b)||b->cpu_active)return GL2_NANO_STATE;
 if(b->fault)return GL2_NANO_FAULT;
 if(!b->pending_valid)return GL2_NANO_OK;
 if(b->api->mbx_wait_done(50000))return queue_fault(b);
 uint32_t *target=b->pending_target;b->retired_target=target;b->pending_target=0;b->pending_valid=0;++b->frames;
 b->retired_width=b->pending_width;b->retired_height=b->pending_height;b->retired_stride=b->pending_stride;
 if(!b->frame_open&&target==b->target)b->completed=1;
 if(completed)*completed=target;
 return GL2_NANO_OK;
}
int gl2_nano_present_buffer(gl2_nano *b,uint32_t *target){
 if(!owned(b)||b->cpu_active||b->frame_open)return GL2_NANO_STATE;
 if(b->fault)return GL2_NANO_FAULT;
 if(NGAME_API_TABLE(b->api)<23||!b->api->lcd_present_buffer||!b->api->lcd_buffer_busy)return GL2_NANO_UNSUPPORTED;
 unsigned old=target==b->retired_target;
 uint32_t w=old?b->retired_width:b->width,h=old?b->retired_height:b->height,stride=old?b->retired_stride:b->stride;
 if(!target||w!=240||h!=432||stride!=240||
    !(target==b->retired_target||(!b->pending_valid&&b->completed&&target==b->target)))return GL2_NANO_TARGET;
 int rc=b->api->lcd_present_buffer(target);
 if(rc==-2)return GL2_NANO_BUSY;
 if(rc)return GL2_NANO_TARGET;
 if(target==b->retired_target)b->retired_target=0;
 if(target==b->target&&!b->frame_open)b->completed=0;
 ++b->presented;return GL2_NANO_OK;
}
int gl2_nano_end_queued(gl2_nano *b,uint32_t **retired){
 if(retired)*retired=0;
 if(!owned(b)||b->cpu_active||!b->frame_open)return GL2_NANO_STATE;
 if(b->fault)return GL2_NANO_FAULT;
 if(NGAME_API_TABLE(b->api)<22||!b->api->mbx_queue_capacity||!b->api->mbx_render_async||!b->api->mbx_queue_capacity())return GL2_NANO_UNSUPPORTED;
 if(b->software_frame||!gpu_range((uintptr_t)b->target,b->stride*b->height*4u))return GL2_NANO_TARGET;
 int rc=gl2_nano_finish(b,retired);if(rc)return rc;
 gl2_set_state_cache(b->ctx,0);
 rc=b->api->mbx_render_async(b->target,b->width,b->height,b->stride);
 b->frame_open=0;b->completed=0;
 if(rc)return queue_fault(b);
 b->pending_target=b->target;b->pending_bytes=b->stride*b->height*4u;b->pending_valid=1;
 b->pending_width=b->width;b->pending_height=b->height;b->pending_stride=b->stride;
 return GL2_NANO_OK;
}
int gl2_nano_end(gl2_nano *b) {
    if (!owned(b) || b->cpu_active || !b->frame_open) return GL2_NANO_STATE;
    int finish_rc=gl2_nano_finish(b,0);if(finish_rc)return finish_rc;
    gl2_set_state_cache(b->ctx,0);
    if(b->software_frame){
        if(b->software_colour)for(uint32_t y=0;y<b->height;++y)copy_colour(b->target+y*b->stride,b->software_colour+y*b->width,b->width);
#if defined(__arm__)
        __asm__ volatile("dsb sy" ::: "memory");
#endif
        b->draws+=b->ctx->soft_draws-b->software_draw_start;b->ctx->soft_target=0;b->ctx->soft_depth=0;b->frame_open=0;b->completed=1;++b->frames;++b->software_frames;return GL2_NANO_OK;
    }
    int r=b->api->mbx_render(b->target,b->width,b->height,b->stride);
    if (!r) r=b->api->mbx_wait_done(50000);
    b->frame_open=0;
    if (r) { b->fault=1;b->completed=0;++b->errors;b->api->mbx_halt();return GL2_NANO_DRIVER; }
    b->completed=1;++b->frames;return GL2_NANO_OK;
}
int gl2_nano_copy_texture(gl2_nano *b,int dx,int dy,int sx,int sy,int w,int h){
 if(!owned(b)||b->fault||b->cpu_active||b->frame_open||!b->completed)return GL2_NANO_STATE;
 if(!gpu_range((uintptr_t)b->target,b->stride*b->height*4u)&&(b->target!=b->api->lcd_back()||!b->api->lcd_ready()))return GL2_NANO_TARGET;
 return gl2_texture_copy_argb(b->ctx,dx,dy,sx,sy,w,h,b->target,b->width,b->height,b->stride)?GL2_NANO_DRIVER:GL2_NANO_OK;
}
int gl2_nano_present(gl2_nano *b) {
    if (!owned(b) || b->fault || b->cpu_active || b->frame_open || !b->completed) return GL2_NANO_STATE;
    if (b->width!=240||b->height!=432||b->stride!=240||b->target!=b->api->lcd_back() || !b->api->lcd_ready()) return GL2_NANO_TARGET;
    b->api->lcd_present();b->completed=0;return GL2_NANO_OK;
}
int gl2_nano_apply_cpu(gl2_nano *b,gl2_nano_cpu_fn fn,void *user) {
    if (!fn) return GL2_NANO_INVALID;
    if (!owned(b) || b->fault || b->cpu_active || b->frame_open || !b->completed) return GL2_NANO_STATE;
    if (b->stride!=b->width) return GL2_NANO_TARGET; /* callback ABI has no row stride */
    if (!gpu_range((uintptr_t)b->target,b->stride*b->height*4u) &&
        (b->target!=b->api->lcd_back() || !b->api->lcd_ready())) return GL2_NANO_TARGET;
    gl2_invalidate_state_cache(b->ctx);
    if(display_busy(b,b->target,b->stride*b->height*4u))return GL2_NANO_BUSY;
    b->cpu_active=1;b->completed=0;
    int result=fn(b->target,b->width,b->height,user);
    if (result) { ++b->cpu_failures;++b->errors;b->cpu_active=0;return GL2_NANO_CPU_ERROR; }
    b->api->dcache_clean();b->completed=1;++b->cpu_passes;b->cpu_active=0;
    return GL2_NANO_OK;
}
void gl2_nano_close(gl2_nano *b) {
    if (!owned(b) || b->cpu_active) return;
    b->api->mbx_halt();
    /* External display DMA can outlive the final GPU job. Do not release the
     * arena while it is still read. A stuck display retains this owner so the
     * caller can retry close after servicing it, instead of freeing live RAM. */
    if(display_busy(b,(const uint32_t *)MM_GAME_GPU,MM_GAME_GPU_END-MM_GAME_GPU)){
        uint32_t start=b->api->usec?b->api->usec():0;
        for(unsigned pass=0;pass<128&&display_busy(b,(const uint32_t *)MM_GAME_GPU,MM_GAME_GPU_END-MM_GAME_GPU);++pass){
            if(b->api->lcd_service)b->api->lcd_service();
            if(b->api->usec&&b->api->usec()-start>=200000u)break;
        }
        if(display_busy(b,(const uint32_t *)MM_GAME_GPU,MM_GAME_GPU_END-MM_GAME_GPU)){b->fault=1;++b->errors;return;}
    }
    gl2_set_state_cache(b->ctx,0);
    b->ctx->soft_target=0;b->ctx->soft_depth=0;
    b->ctx->submit_parts=0;b->ctx->submit=deny_submit;b->ctx->submit_user=0;
    b->ctx->texture_write_guard=deny;b->ctx->texture_write_guard_user=0;
    b->ctx->texture_sync=0;b->ctx->texture_sync_user=0;
    b->frame_open=b->completed=b->opened=b->pending_valid=0;b->pending_target=b->retired_target=0;owner=0;
}
