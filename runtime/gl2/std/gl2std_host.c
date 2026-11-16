/* Host-only platform used by the standard-ABI regression. Not part of the device build. */
#include "../gl2api.h"
#include "gl2std.h"
static gl2_ctx ctx;static gl2_prog_obj progs[4];static uint32_t segment[1024];static uint32_t target[16*16];static float depth[16*16];static uint8_t tex_arena[GL2_TEXTURE_ARENA_BYTES];
int gl2std_host_init(void){gl2_init(&ctx,progs,segment,1024,0,0);gl2_texture_arena(&ctx,tex_arena,0x01000000,sizeof tex_arena,0,0);ctx.soft_target=target;ctx.soft_depth=depth;ctx.soft_width=ctx.soft_height=ctx.soft_stride=16;ctx.soft_depth_initialized=0;gl2std_set_context(&ctx);return 0;}
uint32_t *gl2std_host_target(void){return target;}
