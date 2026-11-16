/* Minimal independent GLES2 + EGL example built purely through NanoOS SDK. */
#include "nano_app.h"
#include "EGL/egl.h"
#include "GLES2/gl2.h"
#ifdef HELLO_BENCH
#include "report.h"
static char report[1024];
static unsigned rused;
static void rtext(const char *s) { while (*s && rused + 1 < sizeof report) report[rused++] = *s++; }
static void rword(uint32_t x) {
    if (rused + 10 >= sizeof report) return;
    static const char h[] = "0123456789abcdef";
    for (unsigned i = 0; i < 8; ++i) report[rused++] = h[(x >> (28 - i * 4)) & 15u];
    report[rused++] = ' ';
}
#endif

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, 0);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    return ok ? sh : 0;
}

int nano_example_main(const struct nanoos_api *api) {
    if (nano_app_init(api) != 0) return 1;

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(dpy, 0, 0)) return 2;

    static const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };
    EGLConfig cfg = 0;
    EGLint ncfg = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1) return 3;

    EGLSurface sfc = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)0, 0);
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, 0);
    if (sfc == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT) return 4;
    if (!eglMakeCurrent(dpy, sfc, sfc, ctx)) return 5;

    static const char *vs_src =
        "attribute vec4 aPos;\n"
        "attribute vec4 aCol;\n"
        "uniform vec2 uOffset;\n"
        "varying vec4 vColor;\n"
        "void main() {\n"
        "    vColor = aCol;\n"
        "    gl_Position = vec4(aPos.xy + uOffset, 0.0, 1.0);\n"
        "}\n";
    static const char *fs_src =
        "precision mediump float;\n"
        "varying vec4 vColor;\n"
        "void main() {\n"
        "    gl_FragColor = vColor;\n"
        "}\n";

    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) return 6;

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) return 7;
    glUseProgram(prog);

    GLint a_pos = glGetAttribLocation(prog, "aPos");
    GLint a_col = glGetAttribLocation(prog, "aCol");
    GLint u_offset = glGetUniformLocation(prog, "uOffset");

    static const float verts[12] = {
         0.0f,  0.5f, 0.0f, 1.0f,
        -0.5f, -0.5f, 0.0f, 1.0f,
         0.5f, -0.5f, 0.0f, 1.0f
    };
    static const float cols[12] = {
        1.0f, 0.2f, 0.2f, 1.0f,
        0.2f, 1.0f, 0.2f, 1.0f,
        0.2f, 0.4f, 1.0f, 1.0f
    };

    glViewport(0, 0, (GLsizei)NANO_SCREEN_W, (GLsizei)NANO_SCREEN_H);
    glVertexAttribPointer((GLuint)a_pos, 4, GL_FLOAT, GL_FALSE, 0, verts);
    glEnableVertexAttribArray((GLuint)a_pos);
    glVertexAttribPointer((GLuint)a_col, 4, GL_FLOAT, GL_FALSE, 0, cols);
    glEnableVertexAttribArray((GLuint)a_col);

    float ox = 0.0f, oy = 0.0f;
    uint32_t center_px = 0, glerr = 0, e0 = (uint32_t)glGetError(), e1 = 0, e2 = 0;
    for (unsigned frame = 0; ; ++frame) {
        nano_input_t in;
        nano_app_poll_input(&in);
        if (in.home_pressed) break;
        if (in.down) {
            ox = ((float)in.x / (float)NANO_SCREEN_W) * 2.0f - 1.0f;
            oy = 1.0f - ((float)in.y / (float)NANO_SCREEN_H) * 2.0f;
        }
#ifdef HELLO_BENCH
        if (frame >= 30u) break;
#else
        (void)frame;
#endif
        glUniform2f(u_offset, ox, oy);
        glClearColor(0.05f, 0.08f, 0.14f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
#ifdef HELLO_BENCH
        if (frame == 0u) e1 = (uint32_t)glGetError();
        if (frame == 29u) {
            uint8_t px[4] = {0, 0, 0, 0};
            glReadPixels(120, 216, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            e2 = (uint32_t)glGetError();
            center_px = ((uint32_t)px[3] << 24) | ((uint32_t)px[0] << 16) | ((uint32_t)px[1] << 8) | px[2];
        }
#endif
        eglSwapBuffers(dpy, sfc);
    }

    glerr = (uint32_t)glGetError();
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglDestroySurface(dpy, sfc);
    eglTerminate(dpy);
#ifdef HELLO_BENCH
    {
        uint32_t frames = 0, sw = 0, fault = 0;
        nano_app_gfx_stats(&frames, &sw, &fault);
        rtext("NANOOS GL2 GAME build " __DATE__ " " __TIME__ "\nHELLO1 status/frames/software/fault/glerr/px ");
        rword(0u); rword(frames); rword(sw); rword(fault); rword(glerr); rword(center_px);
        rtext("\nHELLO2 e0/e1/e2 "); rword(e0); rword(e1); rword(e2);
        rtext("\n");
        rtext((frames == 30u && sw == 0u && fault == 0u && glerr == 0u && (center_px & 0xffffffu) != 0u) ? "HELLO PASS\n" : "HELLO FAIL\n");
        report[rused] = 0;
        rpt_mirror_write(report, rused, rpt_sum(report, rused));
        nano_app_dcache_clean();
    }
#else
    (void)center_px;
    (void)glerr;
    (void)e0;
    (void)e1;
    (void)e2;
#endif
    return 0;
}
