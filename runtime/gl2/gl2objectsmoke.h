/* Shared all-object smoke workload. Caller owns an idle mapped texture arena
 * and complete GPU scene lifecycle. This emits ONE indexed draw, no GPU waits.
 * Expected screen quadrants (grey): upper-left96, upper-right128, lower-left32,
 * lower-right64, with the existing 240x432 viewport and no blending. */
#ifndef GL2OBJECTSMOKE_H
#define GL2OBJECTSMOKE_H
#include "gl2smoke.h"
static inline int gl2_object_smoke_draw(gl2_ctx *c, gl2_prog_obj *progs, uint32_t *seg,
        uint32_t cap, gl2_submit_fn submit, void *user, void *arena, uint32_t texva,
        gl2_texture_sync_fn sync, void *sync_user, uint32_t *glerr) {
    /* Deliberately shuffled records: indices are necessary to recover the quad. */
    static const float vertices[4][10] = {
        { .75f, .75f,0,1, .5f,.5f,.5f,1, 1,1},
        {-.75f,-.75f,0,1, .5f,.5f,.5f,1, 0,0},
        {-.75f, .75f,0,1, .5f,.5f,.5f,1, 0,1},
        { .75f,-.75f,0,1, .5f,.5f,.5f,1, 1,0}
    };
    static const uint16_t indices[4] = {1,3,2,0};
    int stage = gl2_smoke_prepare(c, progs, seg, cap, submit, user, texva, glerr);
    if (stage) return stage;
    uint32_t buffers[2], texture;
    glGenBuffers(c, 2, buffers);
    glBindBuffer(c, GL_ARRAY_BUFFER, buffers[0]);
    glBufferData(c, GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    int ap = glGetAttribLocation(c, c->cur, "aPosition"), ac = glGetAttribLocation(c, c->cur, "aColor"), at = glGetAttribLocation(c, c->cur, "aTexCoord");
    glVertexAttribPointer(c, (uint32_t)ap, 4, GL_FLOAT, 0, 40, (void *)0);
    glVertexAttribPointer(c, (uint32_t)ac, 4, GL_FLOAT, 0, 40, (void *)(uintptr_t)16);
    glVertexAttribPointer(c, (uint32_t)at, 2, GL_FLOAT, 0, 40, (void *)(uintptr_t)32);
    glBindBuffer(c, GL_ARRAY_BUFFER, 0); /* captured bindings must survive */
    glBindBuffer(c, GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
    glBufferData(c, GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    *glerr = glGetError(c); if (*glerr) return 7;
    gl2_texture_arena(c, arena, texva, GL2_TEXTURE_ARENA_BYTES, sync, sync_user);
    glGenTextures(c, 1, &texture); glActiveTexture(c, GL_TEXTURE0); glBindTexture(c, GL_TEXTURE_2D, texture);
    glTexParameteri(c, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(c, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    uint8_t pixels[16 * 16 * 4];
    for (uint32_t y = 0; y < 16; ++y) for (uint32_t x = 0; x < 16; ++x) {
        uint8_t v = y < 8 ? (x < 8 ? 0 : 128) : (x < 8 ? 192 : 255);
        uint32_t i = (y * 16 + x) * 4;
        pixels[i] = pixels[i+1] = pixels[i+2] = v; pixels[i+3] = 255;
    }
    glTexImage2D(c, GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    for (uint32_t i = 0; i < 8 * 8; ++i) {
        pixels[i*4] = pixels[i*4+1] = pixels[i*4+2] = 64; pixels[i*4+3] = 255;
    }
    glTexSubImage2D(c, GL_TEXTURE_2D, 0, 0, 0, 8, 8, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    *glerr = glGetError(c); if (*glerr) return 8;
    glDrawElements(c, GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_SHORT, (void *)0);
    *glerr = glGetError(c);
    return *glerr ? 9 : 0;
}
#endif
