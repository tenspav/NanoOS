/* gl2rt: on-device GLES2-style runtime core for the MBX Lite (freestanding C99, no libc, no malloc).
 * Loads a precompiled program blob (made on the host by blobgen.py: VGP microcode + constants + combiner plan), keeps uniform values,
 * and builds the TA command segment for a draw. The caller submits the segment (mbx_submit_seg) -- this library never touches hardware.
 * Status: host-tested against the Python reference backend (word-for-word); physical run19/20 executes the cube draw builder on-device.
 * This establishes that measured subset, not arbitrary program blobs or GLES conformance. */
#ifndef GL2RT_H
#define GL2RT_H
#include <stdint.h>
#define GL2_MAX_UNI 16
#define GL2_BLOB_MAGIC 0x32504c47u            /* "GLP2" */
typedef struct {
    uint32_t nstage, nplan;                    /* vertex program instruction count, combiner stages (1 or 2) */
    const uint32_t *code;                      /* nstage pairs (A,B) */
    uint32_t nlit; const uint16_t *lit_idx; const float *lit_val;   /* literal constants: index + 4 floats each */
    uint32_t nuni; const uint8_t *uni_idx, *uni_cnt; const char *uni_names;  /* uniform table: constant index, row count, NUL-separated names */
    const uint8_t *plan;                       /* nplan * 12 combiner fields */
    uint32_t has_constp; float constp[4];      /* constant vertex colour (literal folded into the vertex stage) */
    float uval[GL2_MAX_UNI][4][4];             /* current uniform values (rows) */
    uint8_t uset[GL2_MAX_UNI];
    uint32_t dot3_mask; /* native layer0 bit13 per stage */
    uint32_t custom_alpha; uint16_t alpha[2];
    uint32_t omit_colour; /* linked shader does not consume packed colour input */
    uint32_t rounding_bias; /* zero disabled; otherwise hidden 1/255 colour constant index+1 */
    uint32_t repeat_uv0; /* second combiner samples the first stage's transformed UV */
    uint32_t normal_input; /* active slot1 width; zero keeps legacy FIFO layout */
} gl2_program;
typedef struct { uint32_t blend, src, dst, depth_test, depth_func, depth_write; } gl2_state;   /* GL enums */
/* Zero preserves the legacy ARGB8888 descriptor. Other values are the measured
 * MBX texture-format field, not public GL transfer enums. */
#define GL2_TEX_LINEAR 0x80000000u /* sampling flag in storage; format bits unchanged */
#define GL2_TEX_CLAMP_S 0x40000000u /* TEX_CLAMP_S: layer word0 bit31 */
#define GL2_TEX_CLAMP_T 0x20000000u /* TEX_CLAMP_T: layer word0 bit29 */
#define GL2_TEX_MIP_NEAREST 0x20u /* sampler word1 0x32/0xb2 */
#define GL2_TEX_MIP_LINEAR 0x40u  /* sampler word1 0x4e/0xce */
#define GL2_TEX_ARGB8888 0u
#define GL2_TEX_RGB565 5u
#define GL2_TEX_ARGB4444 9u
#define GL2_TEX_ARGB1555 1u
#define GL2_TEX_AL88 17u
#define GL2_TEX_PVRTC4_RGB 0x100u
#define GL2_TEX_PVRTC4_RGBA 0x200u
typedef struct { uint32_t va, log2size, storage; } gl2_tex;
int  gl2_program_load(gl2_program *p, const void *blob, uint32_t len);       /* 0 ok, <0 error */
int  gl2_uniform_loc(const gl2_program *p, const char *name);                 /* -1 if unknown */
void gl2_uniform4f(gl2_program *p, int loc, float x, float y, float z, float w);
void gl2_uniform_matrix4(gl2_program *p, int loc, const float *m_colmajor);   /* GL layout, transpose=FALSE */
void gl2_uniform_matrix3(gl2_program *p, int loc, const float *m_colmajor);
void gl2_uniform_matrix2(gl2_program *p, int loc, const float *m_colmajor);
/* Build one draw segment into out (capacity words). pos: n*4 floats, col: n*4 floats (0..1, ignored with constp), uv0/uv1: n*2 floats (uv1 may be 0).
 * mode: 4 list, 5 strip, 6 fan. Returns word count, or <0 on error. */
int  gl2_build_draw(const gl2_program *p, const gl2_state *st, int mode, uint32_t n, const float *pos, const float *col,
                    const float *uv0, const float *uv1, const gl2_tex *tex, uint32_t vp_w, uint32_t vp_h, uint32_t *out, uint32_t cap);
/* Extended vertex stream: normals are n*4 CPU floats, with normal_input lanes
 * emitted per vertex. Legacy gl2_build_draw requires normal_input==0. */
/* Validated aligned float views; strides are in floats. */
typedef struct {const float *data;uint32_t stride;} gl2_vertex_view;
/* Return native words/vertex for a contiguous float view, otherwise zero. */
uint32_t gl2_native_vertex_words(const gl2_program *,const gl2_vertex_view views[5],const void **source);
int gl2_build_draw_header(const gl2_program *,const gl2_state *,int mode,
 const gl2_tex *,uint32_t vp_w,uint32_t vp_h,uint32_t *out,uint32_t cap);
int gl2_build_draw_header_mask(const gl2_program *,const gl2_state *,int mode,
 const gl2_tex *,uint32_t vp_w,uint32_t vp_h,uint32_t *out,uint32_t cap,uint32_t color_mask);
int gl2_build_draw_views_mask(const gl2_program *,const gl2_state *,int mode,uint32_t n,
 const gl2_vertex_view views[5],const gl2_tex *,uint32_t vp_w,uint32_t vp_h,uint32_t *out,uint32_t cap,uint32_t color_mask);
int gl2_build_draw_inputs_mask(const gl2_program *,const gl2_state *,int mode,uint32_t n,
 const float *pos,const float *normal,const float *col,const float *uv0,const float *uv1,
 const gl2_tex *,uint32_t vp_w,uint32_t vp_h,uint32_t *out,uint32_t cap,uint32_t color_mask);
int gl2_build_draw_views(const gl2_program *,const gl2_state *,int mode,uint32_t n,
 const gl2_vertex_view views[5],const gl2_tex *,uint32_t vp_w,uint32_t vp_h,uint32_t *out,uint32_t cap);
int gl2_build_draw_inputs(const gl2_program *,const gl2_state *,int mode,uint32_t n,
 const float *pos,const float *normal,const float *col,const float *uv0,const float *uv1,
 const gl2_tex *,uint32_t vp_w,uint32_t vp_h,uint32_t *out,uint32_t cap);
#endif
