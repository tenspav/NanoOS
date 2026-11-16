/* Exclusive NanoOS game backend, NOT a RetailOS GPU takeover mechanism.
 * Requires services table v3; compile/upload once, begin/draw/end each frame.
 * Driver end is synchronous. Keep b/ctx/programs/segment alive until close. */
#ifndef GL2NANO_H
#define GL2NANO_H
#include "gl2api.h"
#include "gameabi.h"
#define GL2_NANO_OK 0
#define GL2_NANO_INVALID -1
#define GL2_NANO_BUSY -2
#define GL2_NANO_DRIVER -3
#define GL2_NANO_STATE -4
#define GL2_NANO_TARGET -5
#define GL2_NANO_FAULT -6
#define GL2_NANO_CPU_ERROR -7
#define GL2_NANO_UNSUPPORTED -8
typedef struct {
    const struct nanoos_api *api; gl2_ctx *ctx; uint32_t *target;
    uint32_t opened, frame_open, completed, fault, frame_draws;
    uint32_t width,height,stride;
    uint32_t *software_colour;uint32_t software_colour_bytes;
    uint32_t software_frame,software_frames,software_draw_start,force_gpu; /* force_gpu: diagnostic only */
    uint32_t frames, draws, words, uploads, errors;
    uint32_t direct_draws; /* immediate native-layout source span, no vertex copy */
    uint32_t cpu_active, cpu_passes, cpu_failures; /* explicit opt-in effects only */
    uint32_t *pending_target;
    uint32_t pending_bytes,pending_valid;
    uint32_t pending_width,pending_height,pending_stride;
    uint32_t *retired_target;
    uint32_t retired_width,retired_height,retired_stride;
    uint32_t presented;
    uint32_t fbo_texture,fbo_width,fbo_height,fbo_stride,fbo_surface;
} gl2_nano;
typedef int (*gl2_nano_cpu_fn)(uint32_t *target,uint32_t width,uint32_t height,void *user);
int gl2_nano_open(gl2_nano *b,const struct nanoos_api *api,gl2_ctx *c,
                  gl2_prog_obj *progs,uint32_t *segment,uint32_t cap);
/* Backend chooses a float frame when the current uncached program needs it;
 * otherwise it uses the GPU. The route is fixed until end. A later shader that
 * lacks an executable for that route fails rather than losing colour/depth.
 * Cached GPU rendering is unchanged. Float depth storage is initialized lazily.
 * Native portrait 240x432 only. clear_argb is AARRGGBB (converted for FIFO).
 * Target must be a caller-owned game-GPU buffer or the current free LCD backbuffer. */
int gl2_nano_begin(gl2_nano *b,uint32_t *target,uint32_t clear_argb);
/* Sized offscreen ARGB8888 target. Dimensions are multiples of16, bounded by
 * 240x432; stride is in pixels and >=width. Target storage must be game-GPU
 * memory, disjoint from texture storage. The GL viewport must fit the target.
 * This is backend groundwork, not a framebuffer-object API. */
int gl2_nano_begin_target(gl2_nano *,uint32_t *target,uint32_t width,uint32_t height,uint32_t stride,uint32_t clear_argb);
/* Render into the currently bound, complete framebuffer object. The SDK owns
 * the linear ARGB8888 surface and stride; the FBO supplies colour texture and
 * size. end_fbo finishes synchronously and tiles the surface into the attached
 * texture. Depth is the target's internal depth buffer; depth/stencil texture
 * attachments are not supported yet. */
int gl2_nano_begin_fbo(gl2_nano *,uint32_t *surface,uint32_t stride,uint32_t clear_argb);
int gl2_nano_end_fbo(gl2_nano *);
/* Optional caller-owned cached colour workspace. Float frames render here and
 * copy completed rows to the uncached target at end. Capacity is checked per
 * target; keep it alive while configured. Zero pointer disables staging. */
int gl2_nano_software_colour(gl2_nano *,uint32_t *buffer,uint32_t bytes);
int gl2_nano_end(gl2_nano *b);
/* Optional v22 offscreen queue. End consumes commands immediately, but target
 * pixels are unavailable until finish or the next successful queued end.
 * A second, disjoint offscreen target can be drawn while the first renders.
 * Queued end collects the previous target into retired (NULL on first use).
 * Textures remain immutable while any target is pending. LCD queuing is not
 * implemented; normal synchronous end/present remain available unchanged. */
int gl2_nano_end_queued(gl2_nano *,uint32_t **retired);
/* Collect the outstanding target, also permitted while a disjoint TA frame
 * is open. No pending target returns success with *completed=NULL. */
int gl2_nano_finish(gl2_nano *,uint32_t **completed);
/* v23: present a successfully completed native-sized offscreen target using
 * display DMA, no CPU copy. Busy leaves it available for retry. The backend
 * checks display leases before any later render into overlapping memory. */
int gl2_nano_present_buffer(gl2_nano *,uint32_t *target);
/* Copy a completed target into the bound RGB/RGBA ARGB8888 texture in one CPU
 * tiling pass. No rendering on CPU. Coordinates are bottom-left; padded source
 * stride is supported. This explicit adapter operation is not glCopyTexSubImage2D. */
int gl2_nano_copy_texture(gl2_nano *,int dx,int dy,int sx,int sy,int width,int height);
/* Queues a completed current LCD backbuffer; does not CPU-copy or wait for panel. */
int gl2_nano_present(gl2_nano *b);
/* EXPLICIT last-resort post-GPU CPU work, never automatic shader fallback.
 * Only after successful end and before present. Calls must be serialized; all
 * rendering/texture mutation/reentrant close is rejected while callback runs.
 * Failure invalidates presentation of this frame; begin can start a new one.
 * Callback writes are not rolled back by this generic adapter (effect helpers
 * can provide a transactional scratch buffer). CPU success cleans cache. */
int gl2_nano_apply_cpu(gl2_nano *b,gl2_nano_cpu_fn fn,void *user);
/* Reset/abort own MBX work and drain display DMA before releasing the lease.
 * A stuck display leaves opened=1/fault=1; service it and retry close before
 * releasing target storage. Closing during a CPU callback also retains it. */
void gl2_nano_close(gl2_nano *b);
/* Configure idle caller-owned game-GPU memory; uploads complete uncached writes with a memory barrier.
 * No allocation, pool enlargement, clock changes or firmware writes. */
int gl2_nano_textures(gl2_nano *b,void *arena,uint32_t bytes);
#endif
