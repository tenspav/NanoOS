/* Minimal EGL subset over gl2_nano + the standard GL bridge. */
#include "gl2prefix.h"
#include "../gl2api.h"
#include "gl2prefix_undef.h"
#include "../gl2nano.h"
#include "gl2std.h"
#include "gl2_egl.h"
#include "EGL/egl.h"
static const struct nanoos_api *g_api;
static gl2_ctx c;static gl2_prog_obj progs[GL2_MAX_PROGRAMS];static gl2_nano b;static uint32_t segment[4096];
static uint32_t *const target=(uint32_t *)0x09f00000u;
static uint32_t width=64,height=64,opened,current,fault,window_surface,egl_error=EGL_SUCCESS;
/* Wait only for LCD ownership; rendering goes straight to the panel backbuffer.
 * No CPU colour conversion or full-screen copy is required. */
static int wait_lcd(void){
 if(!g_api||!g_api->lcd_ready||!g_api->lcd_back||!g_api->lcd_service||!g_api->lcd_present)return -1;
 uint32_t start=g_api->usec?g_api->usec():0;
 for(unsigned i=0;!g_api->lcd_ready();++i){
  g_api->lcd_service();
  if((g_api->usec&&g_api->usec()-start>=200000u)||i>=1000000u)return -1;
 }
 return 0;
}
static uint32_t stat_frames,stat_software,stat_fault;
static EGLDisplay display=(EGLDisplay)1;static EGLConfig config=(EGLConfig)1;static EGLContext context=(EGLContext)1;static EGLSurface surface=(EGLSurface)1;
static void ensure_frame(void){
 if(b.frame_open)return;
 int rc;
 if(c.fbo_active)rc=gl2_nano_begin_fbo(&b,target,width,0);
 else if(window_surface){rc=wait_lcd();if(!rc)rc=gl2_nano_begin(&b,g_api->lcd_back(),0);}
 else rc=gl2_nano_begin_target(&b,target,width,height,width,0);
 if(rc){fault=1;b.fault=1;}
}
static void changed_target(void){
 if(!b.frame_open)return;
 int rc=c.fbo_active?gl2_nano_end_fbo(&b):gl2_nano_end(&b);
 if(rc){fault=1;b.fault=1;}
}
static int finish(void){
 if(!b.frame_open)return 0;
 return c.fbo_active?gl2_nano_end_fbo(&b):gl2_nano_end(&b);
}
void gl2_egl_set_platform(const struct nanoos_api *api){g_api=api;}
void gl2_egl_stats(uint32_t *f,uint32_t *s,uint32_t *x){if(f)*f=opened?b.frames:stat_frames;if(s)*s=opened?b.software_frames:stat_software;if(x)*x=opened?fault:stat_fault;}
static int open_context(void){
 if(opened)return 0;
 if(!g_api)return -1;
 int rc=gl2_nano_open(&b,g_api,&c,progs,segment,4096);if(rc)return rc;
 rc=gl2_nano_textures(&b,(void*)0x08a00000,GL2_TEXTURE_ARENA_BYTES);if(rc){gl2_nano_close(&b);return rc;}
 gl2std_set_context(&c);gl2std_set_frame_hooks(ensure_frame,changed_target);opened=1;return 0;
}
EGLDisplay eglGetDisplay(EGLNativeDisplayType d){(void)d;return display;}
EGLBoolean eglInitialize(EGLDisplay d,EGLint *major,EGLint *minor){
 if(d!=display)return EGL_FALSE;
 if(!g_api)return EGL_FALSE;
 if(major)*major=1;
 if(minor)*minor=4;
 return EGL_TRUE;
}
EGLBoolean eglTerminate(EGLDisplay d){
 if(d!=display)return EGL_FALSE;
 if(opened){finish();stat_frames=b.frames;stat_software=b.software_frames;stat_fault=fault;gl2_nano_close(&b);opened=0;}
 gl2std_set_context(0);current=0;return EGL_TRUE;
}
EGLBoolean eglChooseConfig(EGLDisplay d,const EGLint *a,EGLConfig *configs,EGLint size,EGLint *n){
 (void)a;if(d!=display){egl_error=EGL_BAD_DISPLAY;return EGL_FALSE;}if(configs&&size>0)configs[0]=config;if(n)*n=1;return EGL_TRUE;
}
EGLBoolean eglGetConfigAttrib(EGLDisplay d,EGLConfig cfg,EGLint a,EGLint *v){
 if(d!=display||cfg!=config){egl_error=EGL_BAD_CONFIG;return EGL_FALSE;}
 switch(a){case EGL_RED_SIZE:case EGL_GREEN_SIZE:case EGL_BLUE_SIZE:case EGL_ALPHA_SIZE:*v=8;break;case EGL_DEPTH_SIZE:*v=16;break;case EGL_STENCIL_SIZE:*v=0;break;case EGL_SURFACE_TYPE:*v=EGL_PBUFFER_BIT|EGL_WINDOW_BIT;break;case EGL_RENDERABLE_TYPE:*v=EGL_OPENGL_ES2_BIT;break;default:egl_error=EGL_BAD_ATTRIBUTE;return EGL_FALSE;}return EGL_TRUE;
}
EGLContext eglCreateContext(EGLDisplay d,EGLConfig cfg,EGLContext share,const EGLint *a){
 (void)share;(void)a;if(d!=display||cfg!=config){egl_error=EGL_BAD_CONFIG;return EGL_NO_CONTEXT;}return context;
}
EGLSurface eglCreateWindowSurface(EGLDisplay d,EGLConfig cfg,EGLNativeWindowType win,const EGLint *a){
 if(d!=display||cfg!=config){egl_error=EGL_BAD_CONFIG;return EGL_NO_SURFACE;}
 if(win){egl_error=EGL_BAD_NATIVE_WINDOW;return EGL_NO_SURFACE;}
 if(a&&*a!=EGL_NONE){egl_error=EGL_BAD_ATTRIBUTE;return EGL_NO_SURFACE;}
 if(current||(opened&&b.frame_open)){egl_error=EGL_BAD_ACCESS;return EGL_NO_SURFACE;}
 if(wait_lcd()){egl_error=EGL_NOT_INITIALIZED;return EGL_NO_SURFACE;}
 width=240;height=432;window_surface=1;return surface;
}
EGLSurface eglCreatePbufferSurface(EGLDisplay d,EGLConfig cfg,const EGLint *a){
 if(d!=display||cfg!=config){egl_error=EGL_BAD_CONFIG;return EGL_NO_SURFACE;}
 if(current||(opened&&b.frame_open)){egl_error=EGL_BAD_ACCESS;return EGL_NO_SURFACE;}
 uint32_t w=64,h=64;
 for(const EGLint *p=a;p&&*p!=EGL_NONE;p+=2){if(p[0]==EGL_WIDTH)w=(uint32_t)p[1];else if(p[0]==EGL_HEIGHT)h=(uint32_t)p[1];else{egl_error=EGL_BAD_ATTRIBUTE;return EGL_NO_SURFACE;}}
 if(w<16||h<16||w>64||h>64||(w&15)||(h&15)){egl_error=EGL_BAD_ATTRIBUTE;return EGL_NO_SURFACE;}
 width=w;height=h;window_surface=0;return surface;
}
EGLBoolean eglDestroySurface(EGLDisplay d,EGLSurface s){if(d!=display||s!=surface)return EGL_FALSE;return EGL_TRUE;}
EGLBoolean eglDestroyContext(EGLDisplay d,EGLContext x){if(d!=display||x!=context)return EGL_FALSE;return EGL_TRUE;}
EGLBoolean eglMakeCurrent(EGLDisplay d,EGLSurface draw,EGLSurface read,EGLContext x){
 if(d!=display){egl_error=EGL_BAD_DISPLAY;return EGL_FALSE;}
 if(x==EGL_NO_CONTEXT){current=0;gl2std_set_context(0);return EGL_TRUE;}
 if(draw!=surface||read!=surface||x!=context){egl_error=EGL_BAD_MATCH;return EGL_FALSE;}
 if(open_context()){egl_error=EGL_NOT_INITIALIZED;return EGL_FALSE;}
 gl2std_set_context(&c);gl2std_set_frame_hooks(ensure_frame,changed_target);current=1;return EGL_TRUE;
}
EGLBoolean eglSwapBuffers(EGLDisplay d,EGLSurface s){
 if(d!=display||s!=surface||!current){egl_error=EGL_BAD_SURFACE;return EGL_FALSE;}
 if(c.fbo_active){egl_error=EGL_BAD_MATCH;return EGL_FALSE;}
 int had_frame=b.frame_open||b.completed;
 if(finish()||(window_surface&&had_frame&&gl2_nano_present(&b))){fault=1;egl_error=EGL_BAD_ACCESS;return EGL_FALSE;}
 return EGL_TRUE;
}
EGLint eglGetError(void){EGLint e=egl_error;egl_error=EGL_SUCCESS;return e;}
const char *eglQueryString(EGLDisplay d,EGLint n){
 if(d!=display)return 0;
 if(n==EGL_VENDOR)return "Nano7 GLES2";
 if(n==EGL_VERSION)return "1.4";
 if(n==EGL_EXTENSIONS)return "";
 return 0;
}
EGLBoolean eglQuerySurface(EGLDisplay d,EGLSurface s,EGLint a,EGLint *v){
 if(d!=display||s!=surface)return EGL_FALSE;
 if(a==EGL_WIDTH)*v=(EGLint)width;
 else if(a==EGL_HEIGHT)*v=(EGLint)height;
 else{if(v)*v=0;}
 return EGL_TRUE;
}
