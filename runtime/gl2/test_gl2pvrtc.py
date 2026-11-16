from pathlib import Path
import subprocess, tempfile

ROOT = Path(__file__).resolve().parent

TEST_C = r'''
#include "gl2api.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static gl2_ctx c;
static gl2_prog_obj progs[GL2_MAX_PROGRAMS];
static uint32_t seg[2048];
static uint8_t arena[GL2_TEXTURE_ARENA_BYTES];

int main(void) {
    gl2_init(&c, progs, seg, 2048, 0, 0);
    gl2_texture_arena(&c, arena, 0x01000000u, sizeof arena, 0, 0);

    uint32_t tex = 0;
    glGenTextures(&c, 1, &tex);
    glBindTexture(&c, GL_TEXTURE_2D, tex);

    /* Reject bad enum, bad size, border != 0, NULL data */
    uint8_t dummy[2048];
    memset(dummy, 0x5a, sizeof dummy);
    glCompressedTexImage2D(&c, GL_TEXTURE_2D, 0, 0x1234u, 64, 64, 0, 2048, dummy);
    assert(glGetError(&c) == GL_INVALID_ENUM);

    glCompressedTexImage2D(&c, GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG, 64, 64, 0, 1024, dummy);
    assert(glGetError(&c) == GL_INVALID_VALUE);

    glCompressedTexImage2D(&c, GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG, 64, 64, 1, 2048, dummy);
    assert(glGetError(&c) == GL_INVALID_VALUE);

    glCompressedTexImage2D(&c, GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG, 64, 64, 0, 2048, 0);
    assert(glGetError(&c) == GL_INVALID_OPERATION);

    /* Valid 64x64 PVRTC 4bpp RGB + full mip chain down to 1x1 (min 32B) */
    glCompressedTexImage2D(&c, GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG, 64, 64, 0, 2048, dummy);
    assert(glGetError(&c) == GL_NO_ERROR);
    glGenerateMipmap(&c, GL_TEXTURE_2D);
    assert(glGetError(&c) == GL_INVALID_OPERATION);

    int mips[6][2] = { {32, 512}, {16, 128}, {8, 32}, {4, 32}, {2, 32}, {1, 32} };
    for (int l = 0; l < 6; ++l) {
        glCompressedTexImage2D(&c, GL_TEXTURE_2D, l + 1, GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG, mips[l][0], mips[l][0], 0, mips[l][1], dummy);
        assert(glGetError(&c) == GL_NO_ERROR);
    }

    glTexParameteri(&c, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    assert(gl2_resolve_textures(&c, 1, 1) == 0);
    assert((c.tex[0].storage & (GL2_TEX_PVRTC4_RGB | GL2_TEX_LINEAR | GL2_TEX_MIP_LINEAR)) ==
           (GL2_TEX_PVRTC4_RGB | GL2_TEX_LINEAR | GL2_TEX_MIP_LINEAR));

    /* Verify MBX TSP descriptor word0 has 0x38000 for PVRTC 4bpp RGB & RGBA */
    static const char *vs = "attribute vec4 p; attribute vec2 u; varying vec2 v; void main(){v=u;gl_Position=p;}";
    static const char *fs = "precision mediump float; uniform sampler2D s; varying vec2 v; void main(){gl_FragColor=texture2D(s,v);}";
    uint32_t vsh = glCreateShader(&c, GL_VERTEX_SHADER), fsh = glCreateShader(&c, GL_FRAGMENT_SHADER);
    glShaderSource(&c, vsh, vs); glShaderSource(&c, fsh, fs);
    glCompileShader(&c, vsh); glCompileShader(&c, fsh);
    uint32_t prog = glCreateProgram(&c);
    glAttachShader(&c, prog, vsh); glAttachShader(&c, prog, fsh);
    glLinkProgram(&c, prog);
    assert(glGetError(&c) == GL_NO_ERROR);

    uint32_t hdr[64];
    int n = gl2_build_draw_header(&progs[prog].prog, &c.st, GL_TRIANGLES, c.tex, 240, 432, hdr, 64);
    assert(n > 0);
    int e0 = -1;
    for (int i = 0; i + 1 < n; ++i) if (hdr[i] == 0xe0000000u) { e0 = i; break; }
    assert(e0 >= 0 && (hdr[e0 + 1] & 0x3f000u) == 0x38000u);

    /* Switch to PVRTC 4bpp RGBA */
    glCompressedTexImage2D(&c, GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG, 64, 64, 0, 2048, dummy);
    assert(glGetError(&c) == GL_NO_ERROR);
    glTexParameteri(&c, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    assert(gl2_resolve_textures(&c, 1, 1) == 0);
    n = gl2_build_draw_header(&progs[prog].prog, &c.st, GL_TRIANGLES, c.tex, 240, 432, hdr, 64);
    assert(n > 0);
    e0 = -1;
    for (int i = 0; i + 1 < n; ++i) if (hdr[i] == 0xe0000000u) { e0 = i; break; }
    if (e0 < 0 || (hdr[e0 + 1] & 0x3f000u) != 0x38000u) {
        fprintf(stderr, "n=%d e0=%d word=0x%08x storage=0x%x\n", n, e0, e0 >= 0 ? hdr[e0 + 1] : 0u, c.tex[0].storage);
    }
    assert(e0 >= 0 && (hdr[e0 + 1] & 0x3f000u) == 0x38000u);

    puts("PASS glCompressedTexImage2D PVRTC 4bpp RGB/RGBA + mipmaps + MBX TSP descriptor");
    return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    d = Path(tmp)
    (d / "test_pvrtc.c").write_text(TEST_C)
    objs = []
    for src in ["gl2api.c", "gl2rt.c", "gl2vsc.c", "gl2tex.c"]:
        o = d / (src + ".o")
        subprocess.run(["cc", "-std=c99", "-O1", "-g", "-fsanitize=address,undefined", "-Wall", "-Wextra", "-Werror",
                        f"-I{ROOT}", "-c", str(ROOT / src), "-o", str(o)], check=True)
        objs.append(str(o))
    o = d / "test_pvrtc.o"
    subprocess.run(["cc", "-std=c99", "-O1", "-g", "-fsanitize=address,undefined", "-Wall", "-Wextra", "-Werror",
                    f"-I{ROOT}", "-c", str(d / "test_pvrtc.c"), "-o", str(o)], check=True)
    objs.append(str(o))
    exe = d / "test_pvrtc"
    subprocess.run(["cc", "-fsanitize=address,undefined", *objs, "-lm", "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
