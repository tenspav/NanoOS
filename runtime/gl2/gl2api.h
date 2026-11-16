/* gl2api: on-device GLES 2.0-style object layer (freestanding C, no malloc, fixed object pools).
 * Vertex shaders compile on-device (gl2vsc). Fragment shaders first consult an exact-source precompiled cache;
 * uncached straight-line expressions can compile on-device through an exact one-combiner/one-sampler lowering.
 * The same frontend emits a bounded float executable. The native backend selects a float triangle frame for
 * precision-sensitive uncached shaders; direct texture transfer retains the GPU. This is a subset, not full GLSL/GLES2.
 * Unsupported frontend syntax still fails with an info log. GPU-only cached shaders cannot join an active float frame.
 * Draws build a TA segment (gl2rt) and hand it to the submit callback. Typed client/buffer vertex arrays and byte/ushort indexed draws
 * are supported. Buffers are CPU-resident; bounded texture objects convert ES2 byte/packed formats into a caller-supplied GPU-mapped arena.
 * Managed textures support base-level square POT byte/packed ES2 formats, matched NEAREST/LINEAR min+mag and REPEAT; mixed filters, mipmaps and other wraps fail explicitly.
 * Cached sampler names have glUniform1i locations and default to unit0. Up to two units can be selected without changing shader code.
 * Varying names/widths/physical slots must match; unsupported coordinate expressions/layouts are rejected by the host cache tool.
 * NULL BufferData zero-initializes bounded storage. Disabled attributes use GLES (0,0,0,1).
 * Shader source is copied into bounded context-owned storage, including multi-string source updates.
 * A successful link snapshots the executable; attached shader edits and failed relinks do not replace a current executable.
 * Compiled shader cache entries are borrowed until link; linked plans/names/alpha are copied into program-owned storage.
 * Keep cache entries alive and unchanged while compiled shader objects reference them. Each context must have
 * its own program array and segment buffer. Calls must be serialized: compiler and draw scratch storage is not reentrant/thread-safe. */
#ifndef GL2API_H
#define GL2API_H
#include <stdint.h>
#include "gl2rt.h"
#include "gl2vsc.h"
#include "gl2soft.h"
#define GL_NO_ERROR 0
#define GL_INVALID_ENUM 0x500
#define GL_INVALID_VALUE 0x501
#define GL_INVALID_OPERATION 0x502
#define GL_OUT_OF_MEMORY 0x505
#define GL_BYTE 0x1400
#define GL_SHORT 0x1402
#define GL_FIXED 0x140C
#define GL_FLOAT 0x1406
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_BUFFER_SIZE 0x8764
#define GL_BUFFER_USAGE 0x8765
#define GL_STREAM_DRAW 0x88E0
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MIRRORED_REPEAT 0x8370
#define GL_ALPHA 0x1906
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_LUMINANCE 0x1909
#define GL_LUMINANCE_ALPHA 0x190A
#define GL_UNSIGNED_SHORT_4_4_4_4 0x8033
#define GL_UNSIGNED_SHORT_5_5_5_1 0x8034
#define GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG 0x8C00
#define GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG 0x8C02
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_MAX_VIEWPORT_DIMS 0x0D3A
#define GL_SUBPIXEL_BITS 0x0D50
#define GL_RED_BITS 0x0D52
#define GL_GREEN_BITS 0x0D53
#define GL_BLUE_BITS 0x0D54
#define GL_ALPHA_BITS 0x0D55
#define GL_DEPTH_BITS 0x0D56
#define GL_STENCIL_BITS 0x0D57
#define GL_MAX_VERTEX_ATTRIBS 0x8869
#define GL_MAX_VERTEX_UNIFORM_VECTORS 0x8DFB
#define GL_MAX_VARYING_VECTORS 0x8DFC
#define GL_MAX_FRAGMENT_UNIFORM_VECTORS 0x8DFD
#define GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS 0x8B4D
#define GL_MAX_TEXTURE_IMAGE_UNITS 0x8872
#define GL_MAX_CUBE_MAP_TEXTURE_SIZE 0x851C
#define GL_MAX_RENDERBUFFER_SIZE 0x84E8
#define GL_VIEWPORT 0x0BA2
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_BLEND 0x0BE2
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE_MODE 0x0B45
#define GL_FRONT_FACE 0x0B46
#define GL_DEPTH_FUNC 0x0B74
#define GL_DEPTH_WRITEMASK 0x0B72
#define GL_VALIDATE_STATUS 0x8B83
#define GL_TEXTURE 0x1702
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_STENCIL_ATTACHMENT 0x8D20
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT 0x8CD6
#define GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT 0x8CD7
#define GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS 0x8CD9
#define GL_FRAMEBUFFER_UNSUPPORTED 0x8CDD
#define GL_INVALID_FRAMEBUFFER_OPERATION 0x0506
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_RENDERBUFFER_BINDING 0x8CA7
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE 0x8CD0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME 0x8CD1
#define GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL 0x8CD2
#define GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE 0x8CD3
#define GL_RENDERBUFFER_WIDTH 0x8D42
#define GL_RENDERBUFFER_HEIGHT 0x8D43
#define GL_RENDERBUFFER_INTERNAL_FORMAT 0x8D44
#define GL_RGBA4 0x8056
#define GL_RGB5_A1 0x8057
#define GL_RGB565 0x8D62
#define GL_DEPTH_COMPONENT16 0x81A5
#define GL_STENCIL_INDEX8 0x8D48
#define GL_NONE 0
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_STENCIL_BUFFER_BIT 0x0400
#define GL_SCISSOR_TEST 0x0C11
#define GL_SCISSOR_BOX 0x0C10
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_COLOR_CLEAR_VALUE 0x0C22
#define GL_DEPTH_CLEAR_VALUE 0x0B73
#define GL_STENCIL_CLEAR_VALUE 0x0B91
#define GL_PACK_ALIGNMENT 0x0D05
#define GL2_MAX_FRAMEBUFFERS 4
#define GL2_MAX_RENDERBUFFERS 4
#define GL_LOW_FLOAT 0x8DF0
#define GL_MEDIUM_FLOAT 0x8DF1
#define GL_HIGH_FLOAT 0x8DF2
#define GL_LOW_INT 0x8DF3
#define GL_MEDIUM_INT 0x8DF4
#define GL_HIGH_INT 0x8DF5
#ifndef GL2_MAX_TEXTURES
#define GL2_MAX_TEXTURES 8
#endif
#ifndef GL2_MAX_SHADERS
#define GL2_MAX_SHADERS 8
#endif
#ifndef GL2_MAX_PROGRAMS
#define GL2_MAX_PROGRAMS 4
#endif
#ifndef GL2_MAX_TEXTURE_SIZE
#define GL2_MAX_TEXTURE_SIZE 64
#endif
#ifndef GL2_TEXTURE_SLOT_BYTES
#define GL2_TEXTURE_SLOT_BYTES 24576u /* level0 16KiB + full twiddled mip chain */
#endif
#define GL2_TEXTURE_ARENA_BYTES (GL2_MAX_TEXTURES * GL2_TEXTURE_SLOT_BYTES)
#define GL2_FRAGMENT_UNIFORM_BASE (GL2_MAX_UNI+2)
#ifndef GL2_MAX_BUFFERS
#define GL2_MAX_BUFFERS 8
#endif
#ifndef GL2_BUFFER_BYTES
#define GL2_BUFFER_BYTES 16384
#endif
#define GL2_MAX_HEADER_WORDS 1184u
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_SHADER_TYPE 0x8B4F
#define GL_DELETE_STATUS 0x8B80
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_ATTACHED_SHADERS 0x8B85
#define GL_SHADER_SOURCE_LENGTH 0x8B88
#ifndef GL2_SHADER_SOURCE_BYTES
#define GL2_SHADER_SOURCE_BYTES 4096
#endif
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_CW 0x0900
#define GL_CCW 0x0901
#define GL_CULL_FACE 0x0B44
#define GL_TRIANGLES 4
#define GL_TRIANGLE_STRIP 5
#define GL_TRIANGLE_FAN 6
typedef struct {                     /* precompiled fragment shader */
    uint32_t hash; uint8_t nplan, has_constp, nsampler, needs_colour; float constp[4]; uint8_t plan[2][12];
    const char *source; const char *sampler_names[2];
    uint8_t nvary; const char *vary_names[3]; uint8_t vary_slots[3], vary_widths[3];
    uint8_t nuniform;const char *uniform_names[16];uint8_t uniform_widths[16];
    const gl2_soft_program *software; /* owned executable for float rendering */
    uint8_t has_alpha; /* bit0 custom alpha; bit1 diagnostic DOT3 stage0 */ uint16_t alpha[2]; /* zero flag preserves legacy cached alpha */
} gl2_fs_entry;
typedef struct {gl2_fs_entry e;char samplers[2][32],varyings[3][32],uniforms[16][32];gl2_soft_program software;} gl2_fs_owned;
typedef struct { uint32_t type, src_hash, ok, used, attachments, delete_pending, source_length; char log[64]; const char *src; vsc_result *vs; const gl2_fs_entry *fs; } gl2_shader;
typedef struct { uint32_t used, ok, vsh, fsh, delete_pending, executable, validate_status; gl2_program prog; vsc_result vs; const gl2_fs_entry *fs; gl2_fs_owned fs_owned; uint8_t sampler_unit[2],fragment_vertex_alias[16];float fragment_values[16][4]; uint32_t blob[1]; char log[64]; } gl2_prog_obj;
typedef int (*gl2_submit_fn)(const uint32_t *seg, uint32_t words, void *user); /* nonzero return fails draw */
typedef struct {
    uint32_t name, reserved, used, size, usage;
    uint8_t data[GL2_BUFFER_BYTES];
} gl2_buffer;
typedef struct {
    uint32_t enabled; const void *ptr; uint32_t size, stride, buffer, type, normalized; float current[4]; /* byte stride; ptr is offset when buffer != 0 */
} gl2_attribute;
typedef int (*gl2_texture_sync_fn)(void *user, void *cpu, uint32_t gpuva, uint32_t bytes);
typedef struct {
    uint32_t name, reserved, used, defined, size, log2size, format, storage;
    uint32_t min_filter, mag_filter, wrap_s, wrap_t;
    uint32_t level_mask, mip_complete;
} gl2_texture_obj;
typedef struct { uint32_t name,reserved,used,width,height,format,defined; } gl2_renderbuffer;
typedef struct { uint32_t name,reserved,used,color_texture,depth_renderbuffer,stencil_renderbuffer; } gl2_framebuffer;
typedef struct { uint32_t *target; float *depth; uint32_t width,height,stride,depth_initialized; } gl2_soft_saved;
struct gl2_ctx;
typedef int (*gl2_clear_fn)(struct gl2_ctx *c, uint32_t mask, uint32_t argb, float depth, uint32_t color_mask, uint32_t scissor_enabled, const int32_t scissor[4], void *user);
typedef int (*gl2_read_fn)(struct gl2_ctx *c, int x, int y, int w, int h, uint32_t format, uint32_t type, uint32_t pack, void *pixels, void *user);
typedef struct gl2_ctx {
    uint32_t err; const gl2_fs_entry *cache; uint32_t ncache;
    char shader_source[GL2_MAX_SHADERS][GL2_SHADER_SOURCE_BYTES];
    gl2_shader sh[GL2_MAX_SHADERS]; vsc_result sh_vs[GL2_MAX_SHADERS]; gl2_fs_owned sh_fs[GL2_MAX_SHADERS]; gl2_prog_obj *pr; uint32_t cur; /* one result per shader ID; slot 0 unused */
    gl2_attribute attr[5];
    gl2_buffer buffers[GL2_MAX_BUFFERS];
    uint32_t array_buffer, element_buffer, next_buffer;
    uint32_t cull_enabled, cull_face, front_face;
    gl2_state st; gl2_tex tex[2], raw_unit_tex[2]; uint32_t vp_w, vp_h;
    gl2_texture_obj textures[GL2_MAX_TEXTURES];
    uint32_t texture_binding[2], texture_managed[2], active_texture, next_texture, unpack_alignment;
    uint8_t *texture_cpu; uint32_t texture_va;
    gl2_texture_sync_fn texture_sync; void *texture_sync_user;
    int (*texture_write_guard)(void *user); void *texture_write_guard_user; /* optional BEFORE-write veto */
    uint32_t soft_draws,diagnostic_gpu,soft_depth_initialized;
    uint32_t *soft_target;float *soft_depth;uint32_t soft_width,soft_height,soft_stride;
    gl2_submit_fn submit; void *submit_user;
    /* Optional immediate two-span submission; exactly one primitive packet.
     * The callback must consume both spans before returning. */
    int (*submit_parts)(const uint32_t *header,uint32_t header_words,const void *vertices,uint32_t vertex_words,void *user); uint32_t *seg; uint32_t seg_cap;
    uint8_t state_cache_enabled, cached_hdr_valid;
    uint32_t cached_hdr_words, cached_hdr[GL2_MAX_HEADER_WORDS];
    gl2_framebuffer framebuffers[GL2_MAX_FRAMEBUFFERS]; gl2_renderbuffer renderbuffers[GL2_MAX_RENDERBUFFERS];
    uint32_t framebuffer_binding, renderbuffer_binding, next_framebuffer, next_renderbuffer;
    uint32_t fbo_color[GL2_MAX_TEXTURE_SIZE * GL2_MAX_TEXTURE_SIZE];
    float fbo_depth[GL2_MAX_TEXTURE_SIZE * GL2_MAX_TEXTURE_SIZE];
    uint8_t copy_pixels[GL2_MAX_TEXTURE_SIZE * GL2_MAX_TEXTURE_SIZE * 4];
    gl2_soft_saved fbo_saved; uint32_t fbo_active;
    float clear_color[4]; float clear_depth; uint32_t clear_stencil;
    uint32_t color_mask, scissor_enabled; int32_t scissor_box[4];
    gl2_clear_fn clear; void *clear_user;
    gl2_read_fn read_pixels; void *read_pixels_user; uint32_t pack_alignment;
} gl2_ctx;
void gl2_set_state_cache(gl2_ctx *c, int enabled);
void gl2_invalidate_state_cache(gl2_ctx *c);
int gl2_program_needs_software(const gl2_ctx *,const gl2_prog_obj *); /* internal backend route */
uint32_t gl2_hash(const char *s);
void gl2_init(gl2_ctx *c, gl2_prog_obj *progs /* array of 4 */, uint32_t *segbuf, uint32_t seg_cap, gl2_submit_fn fn, void *user);
void gl2_set_fs_cache(gl2_ctx *c, const gl2_fs_entry *e, uint32_t n);
uint32_t glGetError(gl2_ctx *c);
uint32_t glCreateShader(gl2_ctx *c, uint32_t type);
void glShaderSource(gl2_ctx *c, uint32_t sh, const char *src);
/* Multiple strings are concatenated exactly, with no inserted whitespace.
 * Negative or absent lengths mean NUL-terminated strings. An oversized update
 * reports OUT_OF_MEMORY and preserves source and compiled status. */
void glShaderSourceStrings(gl2_ctx *,uint32_t sh,int count,const char *const *strings,const int *lengths);
const char *glGetShaderSource(gl2_ctx *,uint32_t sh);
int glIsShader(gl2_ctx *,uint32_t sh);
void glDeleteShader(gl2_ctx *,uint32_t sh);
int glIsProgram(gl2_ctx *,uint32_t p);
void glDeleteProgram(gl2_ctx *,uint32_t p);
void glDetachShader(gl2_ctx *,uint32_t p,uint32_t sh);
void glGetAttachedShaders(gl2_ctx *,uint32_t p,int capacity,int *count,uint32_t *shaders);
void glCompileShader(gl2_ctx *c, uint32_t sh);
int  glGetShaderiv(gl2_ctx *c, uint32_t sh, uint32_t pname);
const char *glGetShaderInfoLog(gl2_ctx *c, uint32_t sh);
uint32_t glCreateProgram(gl2_ctx *c);
void glAttachShader(gl2_ctx *c, uint32_t p, uint32_t sh);
void glLinkProgram(gl2_ctx *c, uint32_t p);
int  glGetProgramiv(gl2_ctx *c, uint32_t p, uint32_t pname);
const char *glGetProgramInfoLog(gl2_ctx *c, uint32_t p);
void glUseProgram(gl2_ctx *c, uint32_t p);
int  glGetUniformLocation(gl2_ctx *c, uint32_t p, const char *name);
int  glGetAttribLocation(gl2_ctx *c, uint32_t p, const char *name);
/* Sampler locations live above the vertex-uniform namespace (GL2_MAX_UNI). */
void glUniform1i(gl2_ctx *c, int loc, int unit);
void glUniform1f(gl2_ctx *c, int loc, float x);
void glUniform2f(gl2_ctx *c,int loc,float x,float y);
void glUniform3f(gl2_ctx *c,int loc,float x,float y,float z);
void glUniform4f(gl2_ctx *c, int loc, float x, float y, float z, float w);
void glUniformMatrix4fv(gl2_ctx *c, int loc, int count, int transpose, const float *m);
void glUniformMatrix3fv(gl2_ctx *c, int loc, int count, int transpose, const float *m);
void glUniformMatrix2fv(gl2_ctx *c, int loc, int count, int transpose, const float *m);
void glEnableVertexAttribArray(gl2_ctx *c, uint32_t loc);
void glDisableVertexAttribArray(gl2_ctx *c, uint32_t loc);
void glVertexAttrib1f(gl2_ctx *,uint32_t loc,float x);
void glVertexAttrib2f(gl2_ctx *,uint32_t loc,float x,float y);
void glVertexAttrib3f(gl2_ctx *,uint32_t loc,float x,float y,float z);
void glVertexAttrib4f(gl2_ctx *,uint32_t loc,float x,float y,float z,float w);
void glVertexAttribPointer(gl2_ctx *c, uint32_t loc, int size, uint32_t type, int normalized, int stride_bytes, const void *ptr);
void glBlendFunc(gl2_ctx *c, uint32_t s, uint32_t d);
void glDepthFunc(gl2_ctx *c, uint32_t f);
void glDepthMask(gl2_ctx *c, int flag);
void glCullFace(gl2_ctx *c, uint32_t mode);
void glFrontFace(gl2_ctx *c, uint32_t mode);
int glIsEnabled(gl2_ctx *c, uint32_t cap);
void glEnable(gl2_ctx *c, uint32_t cap);
void glDisable(gl2_ctx *c, uint32_t cap);
void glViewport(gl2_ctx *c, int x, int y, int w, int h);
/* Legacy pre-twiddled binding bypasses managed objects/completeness for that unit. */
void gl2_bind_texture(gl2_ctx *c, uint32_t unit, uint32_t gpu_va, uint32_t log2size);
/* SDKs may bind preformatted GPU-resident data without CPU conversion. */
void gl2_bind_texture_native(gl2_ctx *c,uint32_t unit,uint32_t gpu_va,uint32_t log2size,uint32_t storage);
/* Caller owns arena mapping, lifetime, cache coherency and waits before overwrite/delete/reuse.
 * CPU arena must be 4-byte aligned (GPU VA remains 128-byte aligned).
 * Callback may flush/upload but never implies a GPU wait; NULL is valid for coherent memory. */
void gl2_texture_arena(gl2_ctx *c, void *cpu, uint32_t gpuva, uint32_t bytes, gl2_texture_sync_fn sync, void *user);
void glGenTextures(gl2_ctx *c, int n, uint32_t *names);
void glBindTexture(gl2_ctx *c, uint32_t target, uint32_t name);
void glDeleteTextures(gl2_ctx *c, int n, const uint32_t *names);
int glIsTexture(gl2_ctx *c, uint32_t name);
void glActiveTexture(gl2_ctx *c, uint32_t texture);
void glTexParameteri(gl2_ctx *c, uint32_t target, uint32_t pname, int value);
int glGetTexParameteriv(gl2_ctx *c, uint32_t target, uint32_t pname);
void glPixelStorei(gl2_ctx *c, uint32_t pname, int value);
void glTexImage2D(gl2_ctx *c, uint32_t target, int level, int internalformat, int width, int height, int border, uint32_t format, uint32_t type, const void *pixels);
void glCompressedTexImage2D(gl2_ctx *c, uint32_t target, int level, uint32_t internalformat, int width, int height, int border, int imageSize, const void *data);
void glTexSubImage2D(gl2_ctx *c, uint32_t target, int level, int x, int y, int width, int height, uint32_t format, uint32_t type, const void *pixels);
void glGenerateMipmap(gl2_ctx *c, uint32_t target);
/* Internal backend copy: caller must complete GPU writes first. Pixel coordinates
 * use the bottom-left convention; source rows are top-to-bottom ARGB8888.
 * Supports defined RGB/RGBA ARGB8888 images; source/destination must be disjoint. */
int gl2_texture_copy_argb(gl2_ctx *,int dx,int dy,int sx,int sy,int w,int h,const uint32_t *,uint32_t width,uint32_t height,uint32_t stride);
/* Internal draw resolution; exposed only to keep the freestanding units separate. */
int gl2_resolve_textures(gl2_ctx *c, uint32_t layers, uint32_t samplers); /* legacy implicit mapping helper */
int gl2_resolve_texture_units(gl2_ctx *c, uint32_t layers, uint32_t samplers, const uint8_t units[2]);
void glGenBuffers(gl2_ctx *c, int n, uint32_t *names);
void glBindBuffer(gl2_ctx *c, uint32_t target, uint32_t name);
void glDeleteBuffers(gl2_ctx *c, int n, const uint32_t *names);
int glIsBuffer(gl2_ctx *c, uint32_t name);
void glBufferData(gl2_ctx *c, uint32_t target, intptr_t size, const void *data, uint32_t usage);
void glBufferSubData(gl2_ctx *c, uint32_t target, intptr_t offset, intptr_t size, const void *data);
int glGetBufferParameteriv(gl2_ctx *c, uint32_t target, uint32_t pname);
void glDrawArrays(gl2_ctx *c, uint32_t mode, int first, int count);
void glDrawElements(gl2_ctx *c, uint32_t mode, int count, uint32_t type, const void *indices);
const char *glGetString(gl2_ctx *c, uint32_t name);
void glGetIntegerv(gl2_ctx *c, uint32_t pname, int *params);
void glGetBooleanv(gl2_ctx *c, uint32_t pname, unsigned char *params);
void glGetFloatv(gl2_ctx *c, uint32_t pname, float *params);
void glGetShaderPrecisionFormat(gl2_ctx *c, uint32_t shadertype, uint32_t precisiontype, int *range, int *precision);
void glValidateProgram(gl2_ctx *c, uint32_t program);
void glFlush(gl2_ctx *c);
void glFinish(gl2_ctx *c);
void glGenFramebuffers(gl2_ctx *c, int n, uint32_t *names);
void glBindFramebuffer(gl2_ctx *c, uint32_t target, uint32_t name);
void glDeleteFramebuffers(gl2_ctx *c, int n, const uint32_t *names);
int glIsFramebuffer(gl2_ctx *c, uint32_t name);
uint32_t glCheckFramebufferStatus(gl2_ctx *c, uint32_t target);
void glFramebufferTexture2D(gl2_ctx *c, uint32_t target, uint32_t attachment, uint32_t textarget, uint32_t texture, int level);
void glFramebufferRenderbuffer(gl2_ctx *c, uint32_t target, uint32_t attachment, uint32_t rbtarget, uint32_t renderbuffer);
void glGetFramebufferAttachmentParameteriv(gl2_ctx *c, uint32_t target, uint32_t attachment, uint32_t pname, int *params);
void glGenRenderbuffers(gl2_ctx *c, int n, uint32_t *names);
void glBindRenderbuffer(gl2_ctx *c, uint32_t target, uint32_t name);
void glDeleteRenderbuffers(gl2_ctx *c, int n, const uint32_t *names);
int glIsRenderbuffer(gl2_ctx *c, uint32_t name);
void glRenderbufferStorage(gl2_ctx *c, uint32_t target, uint32_t internalformat, int width, int height);
void glGetRenderbufferParameteriv(gl2_ctx *c, uint32_t target, uint32_t pname, int *params);
void glClearColor(gl2_ctx *c, float r, float g, float b, float a);
void glClearDepthf(gl2_ctx *c, float depth);
void glClearStencil(gl2_ctx *c, int stencil);
void glColorMask(gl2_ctx *c, int r, int g, int b, int a);
void glScissor(gl2_ctx *c, int x, int y, int width, int height);
void glClear(gl2_ctx *c, uint32_t mask);
void glReadPixels(gl2_ctx *c, int x, int y, int width, int height, uint32_t format, uint32_t type, void *pixels);
void glCopyTexImage2D(gl2_ctx *c, uint32_t target, int level, uint32_t internalformat, int x, int y, int width, int height, int border);
void glCopyTexSubImage2D(gl2_ctx *c, uint32_t target, int level, int xoffset, int yoffset, int x, int y, int width, int height);
int gl2_texture_store_argb(gl2_ctx *c, gl2_texture_obj *t, int dx, int dy, int w, int h, const uint32_t *surface, uint32_t sw, uint32_t sh, uint32_t stride);
int gl2_texture_store_name(gl2_ctx *c, uint32_t name, int dx, int dy, int w, int h, const uint32_t *surface, uint32_t sw, uint32_t sh, uint32_t stride);
int gl2_fbo_target(gl2_ctx *c, uint32_t *texture, uint32_t *width, uint32_t *height);
#endif
