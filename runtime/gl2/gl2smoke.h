/* Shared host/device smoke workload. No firmware access; caller owns GPU submission.
 * Fragment source must match the host-generated fs_cache_test.h byte-for-byte.
 * Prototype API: explicit context and client arrays, not a conformant GLES ABI. */
#ifndef GL2SMOKE_H
#define GL2SMOKE_H
#include "gl2api.h"
#include "fs_cache_test.h"
static const char gl2_smoke_vs[] =
    "uniform mat4 uM; attribute vec4 aPosition; attribute vec4 aColor; attribute vec2 aTexCoord; varying vec4 vColor; varying vec2 vUV; void main(){ gl_Position=uM*aPosition; vColor=aColor; vUV=aTexCoord; }\n";
static const char gl2_smoke_fs[] =
    "precision mediump float; varying vec4 vColor; varying vec2 vUV; uniform sampler2D s0; void main(){ gl_FragColor = texture2D(s0,vUV)*vColor; }";
/* Returns a stage-specific nonzero code on failure; sets the raw GL error. */
static inline int gl2_smoke_prepare(gl2_ctx *c, gl2_prog_obj *progs, uint32_t *seg,
                          uint32_t cap, gl2_submit_fn submit, void *user,
                          uint32_t texva, uint32_t *glerr) {
    static const float pos[16] = {-0.75f,-0.75f,0,1, 0.75f,-0.75f,0,1,
                                 -0.75f,0.75f,0,1, 0.75f,0.75f,0,1};
    static const float col[16] = {0.5f,0.5f,0.5f,1, 0.5f,0.5f,0.5f,1,
                                 0.5f,0.5f,0.5f,1, 0.5f,0.5f,0.5f,1};
    static const float uv[8] = {0,0, 1,0, 0,1, 1,1};
    static const float eye[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    gl2_init(c, progs, seg, cap, submit, user);
    gl2_set_fs_cache(c, fs_cache, FS_CACHE_N);
    *glerr = 0;
    uint32_t vs = glCreateShader(c, GL_VERTEX_SHADER), fs = glCreateShader(c, GL_FRAGMENT_SHADER);
    glShaderSource(c, vs, gl2_smoke_vs); glShaderSource(c, fs, gl2_smoke_fs);
    glCompileShader(c, vs); if (!glGetShaderiv(c, vs, GL_COMPILE_STATUS)) return 1;
    glCompileShader(c, fs); if (!glGetShaderiv(c, fs, GL_COMPILE_STATUS)) return 2;
    uint32_t p = glCreateProgram(c); glAttachShader(c, p, vs); glAttachShader(c, p, fs); glLinkProgram(c, p);
    if (!glGetProgramiv(c, p, GL_LINK_STATUS)) return 3;
    glUseProgram(c, p);
    glUniformMatrix4fv(c, glGetUniformLocation(c, p, "uM"), 1, 0, eye);
    int ap = glGetAttribLocation(c, p, "aPosition"), ac = glGetAttribLocation(c, p, "aColor"), at = glGetAttribLocation(c, p, "aTexCoord");
    if (ap < 0 || ac < 0 || at < 0) return 4;
    glEnableVertexAttribArray(c, (uint32_t)ap); glEnableVertexAttribArray(c, (uint32_t)ac); glEnableVertexAttribArray(c, (uint32_t)at);
    glVertexAttribPointer(c, (uint32_t)ap, 4, 0x1406, 0, 0, pos);
    glVertexAttribPointer(c, (uint32_t)ac, 4, 0x1406, 0, 0, col);
    glVertexAttribPointer(c, (uint32_t)at, 2, 0x1406, 0, 0, uv);
    gl2_bind_texture(c, 0, texva, 4); gl2_bind_texture(c, 1, texva, 4);
    glViewport(c, 0, 0, 240, 432);
    *glerr = glGetError(c);
    return *glerr ? 5 : 0;
}
static inline int gl2_smoke_draw(gl2_ctx *c, gl2_prog_obj *progs, uint32_t *seg,
                                uint32_t cap, gl2_submit_fn submit, void *user,
                                uint32_t texva, uint32_t *glerr) {
    int stage = gl2_smoke_prepare(c, progs, seg, cap, submit, user, texva, glerr);
    if (stage) return stage;
    glDrawArrays(c, GL_TRIANGLE_STRIP, 0, 4);
    *glerr = glGetError(c);
    return *glerr ? 5 : 0;
}
#endif
