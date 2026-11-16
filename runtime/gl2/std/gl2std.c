/* Standard GLES2 entry points over the explicit-context driver. The driver
 * sources in this build are compiled with gl2prefix.h, so their gl* symbols are
 * gl2x_gl*; these wrappers expose the standard ABI. */
#include "gl2prefix.h"
#include "../gl2api.h"
#include "gl2prefix_undef.h"
#include "GLES2/gl2.h"
#include "gl2std.h"
static struct gl2_ctx *g_ctx;static gl2_prog_obj *std_program(GLuint p){if(!g_ctx||!g_ctx->pr||p>=4)return 0;gl2_prog_obj*o=&g_ctx->pr[p];return o->used?o:0;}
static void std_name(const char*s,GLsizei n,GLsizei*len,GLchar*out){GLsizei i=0;if(!out||n<=0){if(len)*len=0;return;}if(s)while(s[i]&&i<n-1){out[i]=s[i];++i;}out[i]=0;if(len)*len=i;}
static GLenum std_utype(uint32_t w,uint32_t cnt){if(cnt>1&&w==cnt*cnt)return cnt==2?GL_FLOAT_MAT2:cnt==3?GL_FLOAT_MAT3:GL_FLOAT_MAT4;return w==1?GL_FLOAT:w==2?GL_FLOAT_VEC2:w==3?GL_FLOAT_VEC3:GL_FLOAT_VEC4;}
static GLenum std_atype(uint32_t w){return w==1?GL_FLOAT:w==2?GL_FLOAT_VEC2:w==3?GL_FLOAT_VEC3:GL_FLOAT_VEC4;}
static uint32_t std_nuniforms(gl2_prog_obj*o){return o->vs.nuni+(o->fs?o->fs->nuniform:0);}
static int std_uniform_at(gl2_prog_obj*o,uint32_t i,const char**name,uint32_t*w,uint32_t*cnt){
    if(i<o->vs.nuni){*name=o->vs.uni_name[i];*w=o->vs.uni_width[i];*cnt=o->vs.uni_cnt[i];return 1;}
    i-=o->vs.nuni;if(!o->fs||i>=o->fs->nuniform)return 0;*name=o->fs->uniform_names[i];*w=o->fs->uniform_widths[i];*cnt=1;return 1;
}
static gl2std_frame_fn g_ensure,g_changed;
void gl2std_set_context(struct gl2_ctx *c){g_ctx=c;}
void gl2std_set_frame_hooks(gl2std_frame_fn e,gl2std_frame_fn c){g_ensure=e;g_changed=c;}
static void copylog(const char *s,GLsizei n,GLsizei *len,GLchar *out){
    GLsizei i=0;if(!out||n<=0){if(len)*len=0;return;}
    if(s)while(s[i]&&i<n-1){out[i]=s[i];++i;}
    out[i]=0;if(len)*len=i;
}
void glClearColor(GLfloat r,GLfloat g,GLfloat b,GLfloat a){gl2x_glClearColor(g_ctx,r,g,b,a);}
void glClearDepthf(GLclampf d){gl2x_glClearDepthf(g_ctx,d);}
void glClearStencil(GLint s){gl2x_glClearStencil(g_ctx,s);}
void glClear(GLbitfield mask){if(g_ensure)g_ensure();gl2x_glClear(g_ctx,mask);}
void glEnable(GLenum cap){gl2x_glEnable(g_ctx,cap);}
void glDisable(GLenum cap){gl2x_glDisable(g_ctx,cap);}
void glViewport(GLint x,GLint y,GLsizei w,GLsizei h){gl2x_glViewport(g_ctx,x,y,w,h);}
void glBlendFunc(GLenum s,GLenum d){gl2x_glBlendFunc(g_ctx,s,d);}
void glDepthFunc(GLenum f){gl2x_glDepthFunc(g_ctx,f);}
void glDepthMask(GLboolean f){gl2x_glDepthMask(g_ctx,f);}
void glCullFace(GLenum m){gl2x_glCullFace(g_ctx,m);}
void glFrontFace(GLenum m){gl2x_glFrontFace(g_ctx,m);}
void glColorMask(GLboolean r,GLboolean g,GLboolean b,GLboolean a){gl2x_glColorMask(g_ctx,r,g,b,a);}
void glScissor(GLint x,GLint y,GLsizei w,GLsizei h){gl2x_glScissor(g_ctx,x,y,w,h);}
void glPixelStorei(GLenum p,GLint v){gl2x_glPixelStorei(g_ctx,p,v);}
GLenum glGetError(void){return (GLenum)gl2x_glGetError(g_ctx);}
const GLubyte *glGetString(GLenum n){return (const GLubyte *)gl2x_glGetString(g_ctx,n);}
void glGetIntegerv(GLenum p,GLint *v){gl2x_glGetIntegerv(g_ctx,p,v);}
void glGetFloatv(GLenum p,GLfloat *v){gl2x_glGetFloatv(g_ctx,p,v);}
void glGetBooleanv(GLenum p,GLboolean *v){gl2x_glGetBooleanv(g_ctx,p,v);}
void glGetShaderPrecisionFormat(GLenum t,GLenum p,GLint *r,GLint *q){gl2x_glGetShaderPrecisionFormat(g_ctx,t,p,r,q);}
void glFinish(void){gl2x_glFinish(g_ctx);}
void glFlush(void){gl2x_glFlush(g_ctx);}
GLuint glCreateShader(GLenum t){return gl2x_glCreateShader(g_ctx,t);}
void glShaderSource(GLuint sh,GLsizei n,const GLchar *const *s,const GLint *l){gl2x_glShaderSourceStrings(g_ctx,sh,n,(const char *const *)s,l);}
void glCompileShader(GLuint sh){gl2x_glCompileShader(g_ctx,sh);}
void glGetShaderiv(GLuint sh,GLenum p,GLint *v){*v=gl2x_glGetShaderiv(g_ctx,sh,p);}
void glGetShaderInfoLog(GLuint sh,GLsizei n,GLsizei *l,GLchar *out){copylog(gl2x_glGetShaderInfoLog(g_ctx,sh),n,l,out);}
void glGetShaderSource(GLuint sh,GLsizei n,GLsizei *l,GLchar *out){copylog(gl2x_glGetShaderSource(g_ctx,sh),n,l,out);}
GLboolean glIsShader(GLuint sh){return (GLboolean)gl2x_glIsShader(g_ctx,sh);}
void glDeleteShader(GLuint sh){gl2x_glDeleteShader(g_ctx,sh);}
GLuint glCreateProgram(void){return gl2x_glCreateProgram(g_ctx);}
void glAttachShader(GLuint p,GLuint sh){gl2x_glAttachShader(g_ctx,p,sh);}
void glDetachShader(GLuint p,GLuint sh){gl2x_glDetachShader(g_ctx,p,sh);}
void glLinkProgram(GLuint p){gl2x_glLinkProgram(g_ctx,p);}
void glUseProgram(GLuint p){gl2x_glUseProgram(g_ctx,p);}
void glDeleteProgram(GLuint p){gl2x_glDeleteProgram(g_ctx,p);}
GLboolean glIsProgram(GLuint p){return (GLboolean)gl2x_glIsProgram(g_ctx,p);}
void glGetProgramiv(GLuint p,GLenum n,GLint *v){
    if(n==GL_ACTIVE_UNIFORMS||n==GL_ACTIVE_ATTRIBUTES){gl2_prog_obj*o=std_program(p);if(!o){if(g_ctx)g_ctx->err=GL_INVALID_VALUE;if(v)*v=0;return;}*v=(GLint)(n==GL_ACTIVE_UNIFORMS?std_nuniforms(o):o->vs.nattr);return;}
    *v=gl2x_glGetProgramiv(g_ctx,p,n);
}
void glGetActiveUniform(GLuint p,GLuint i,GLsizei n,GLsizei*len,GLint*size,GLenum*type,GLchar*out){
    gl2_prog_obj*o=std_program(p);const char*nm;uint32_t w,c;if(!o||!std_uniform_at(o,i,&nm,&w,&c)){if(g_ctx)g_ctx->err=GL_INVALID_VALUE;if(len)*len=0;return;}
    std_name(nm,n,len,out);if(size)*size=1;if(type)*type=std_utype(w,c);
}
void glGetActiveAttrib(GLuint p,GLuint i,GLsizei n,GLsizei*len,GLint*size,GLenum*type,GLchar*out){
    gl2_prog_obj*o=std_program(p);if(!o||i>=o->vs.nattr){if(g_ctx)g_ctx->err=GL_INVALID_VALUE;if(len)*len=0;return;}
    std_name(o->vs.attr_name[i],n,len,out);if(size)*size=1;if(type)*type=std_atype(o->vs.attr_w[i]);
}
void glGetProgramInfoLog(GLuint p,GLsizei n,GLsizei *l,GLchar *out){copylog(gl2x_glGetProgramInfoLog(g_ctx,p),n,l,out);}
void glGetAttachedShaders(GLuint p,GLsizei n,GLsizei *c,GLuint *s){gl2x_glGetAttachedShaders(g_ctx,p,n,c,(uint32_t*)s);}
void glValidateProgram(GLuint p){gl2x_glValidateProgram(g_ctx,p);}
GLint glGetAttribLocation(GLuint p,const GLchar *n){return gl2x_glGetAttribLocation(g_ctx,p,n);}
GLint glGetUniformLocation(GLuint p,const GLchar *n){return gl2x_glGetUniformLocation(g_ctx,p,n);}
void glBindAttribLocation(GLuint p,GLuint i,const GLchar *n){(void)p;(void)i;(void)n;}
void glUniform1i(GLint l,GLint x){gl2x_glUniform1i(g_ctx,l,x);}
void glUniform1f(GLint l,GLfloat x){gl2x_glUniform1f(g_ctx,l,x);}
void glUniform2f(GLint l,GLfloat x,GLfloat y){gl2x_glUniform2f(g_ctx,l,x,y);}
void glUniform3f(GLint l,GLfloat x,GLfloat y,GLfloat z){gl2x_glUniform3f(g_ctx,l,x,y,z);}
void glUniform4f(GLint l,GLfloat x,GLfloat y,GLfloat z,GLfloat w){gl2x_glUniform4f(g_ctx,l,x,y,z,w);}
void glUniform1fv(GLint l,GLsizei c,const GLfloat *v){for(GLsizei i=0;i<c;++i)gl2x_glUniform1f(g_ctx,l+i,v[i]);}
void glUniform2fv(GLint l,GLsizei c,const GLfloat *v){for(GLsizei i=0;i<c;++i)gl2x_glUniform2f(g_ctx,l+i,v[i*2],v[i*2+1]);}
void glUniform3fv(GLint l,GLsizei c,const GLfloat *v){for(GLsizei i=0;i<c;++i)gl2x_glUniform3f(g_ctx,l+i,v[i*3],v[i*3+1],v[i*3+2]);}
void glUniform4fv(GLint l,GLsizei c,const GLfloat *v){for(GLsizei i=0;i<c;++i)gl2x_glUniform4f(g_ctx,l+i,v[i*4],v[i*4+1],v[i*4+2],v[i*4+3]);}
void glUniformMatrix2fv(GLint l,GLsizei c,GLboolean t,const GLfloat *v){gl2x_glUniformMatrix2fv(g_ctx,l,c,t,v);}
void glUniformMatrix3fv(GLint l,GLsizei c,GLboolean t,const GLfloat *v){gl2x_glUniformMatrix3fv(g_ctx,l,c,t,v);}
void glUniformMatrix4fv(GLint l,GLsizei c,GLboolean t,const GLfloat *v){gl2x_glUniformMatrix4fv(g_ctx,l,c,t,v);}
void glEnableVertexAttribArray(GLuint i){gl2x_glEnableVertexAttribArray(g_ctx,i);}
void glDisableVertexAttribArray(GLuint i){gl2x_glDisableVertexAttribArray(g_ctx,i);}
void glVertexAttribPointer(GLuint i,GLint s,GLenum t,GLboolean n,GLsizei st,const void *p){gl2x_glVertexAttribPointer(g_ctx,i,s,t,n,st,p);}
void glVertexAttrib1f(GLuint i,GLfloat x){gl2x_glVertexAttrib1f(g_ctx,i,x);}
void glVertexAttrib2f(GLuint i,GLfloat x,GLfloat y){gl2x_glVertexAttrib2f(g_ctx,i,x,y);}
void glVertexAttrib3f(GLuint i,GLfloat x,GLfloat y,GLfloat z){gl2x_glVertexAttrib3f(g_ctx,i,x,y,z);}
void glVertexAttrib4f(GLuint i,GLfloat x,GLfloat y,GLfloat z,GLfloat w){gl2x_glVertexAttrib4f(g_ctx,i,x,y,z,w);}
void glGenBuffers(GLsizei n,GLuint *b){gl2x_glGenBuffers(g_ctx,n,(uint32_t*)b);}
void glBindBuffer(GLenum t,GLuint b){gl2x_glBindBuffer(g_ctx,t,b);}
void glDeleteBuffers(GLsizei n,const GLuint *b){gl2x_glDeleteBuffers(g_ctx,n,(const uint32_t*)b);}
GLboolean glIsBuffer(GLuint b){return (GLboolean)gl2x_glIsBuffer(g_ctx,b);}
void glBufferData(GLenum t,GLsizeiptr s,const void *d,GLenum u){gl2x_glBufferData(g_ctx,t,s,d,u);}
void glBufferSubData(GLenum t,GLintptr o,GLsizeiptr s,const void *d){gl2x_glBufferSubData(g_ctx,t,o,s,d);}
void glGetBufferParameteriv(GLenum t,GLenum p,GLint *v){*v=gl2x_glGetBufferParameteriv(g_ctx,t,p);}
void glDrawArrays(GLenum m,GLint f,GLsizei c){if(g_ensure)g_ensure();gl2x_glDrawArrays(g_ctx,m,f,c);}
void glDrawElements(GLenum m,GLsizei c,GLenum t,const void *i){if(g_ensure)g_ensure();gl2x_glDrawElements(g_ctx,m,c,t,i);}
void glGenTextures(GLsizei n,GLuint *t){gl2x_glGenTextures(g_ctx,n,(uint32_t*)t);}
void glBindTexture(GLenum t,GLuint n){gl2x_glBindTexture(g_ctx,t,n);}
void glDeleteTextures(GLsizei n,const GLuint *t){gl2x_glDeleteTextures(g_ctx,n,(const uint32_t*)t);}
GLboolean glIsTexture(GLuint t){return (GLboolean)gl2x_glIsTexture(g_ctx,t);}
void glActiveTexture(GLenum t){gl2x_glActiveTexture(g_ctx,t);}
void glTexParameteri(GLenum t,GLenum p,GLint v){gl2x_glTexParameteri(g_ctx,t,p,v);}
void glTexParameterf(GLenum t,GLenum p,GLfloat v){gl2x_glTexParameteri(g_ctx,t,p,(GLint)v);}
void glGetTexParameteriv(GLenum t,GLenum p,GLint *v){*v=gl2x_glGetTexParameteriv(g_ctx,t,p);}
void glTexImage2D(GLenum t,GLint l,GLint i,GLsizei w,GLsizei h,GLint b,GLenum f,GLenum y,const void *p){gl2x_glTexImage2D(g_ctx,t,l,i,w,h,b,f,y,p);}
void glCompressedTexImage2D(GLenum t,GLint l,GLenum i,GLsizei w,GLsizei h,GLint b,GLsizei s,const void *p){gl2x_glCompressedTexImage2D(g_ctx,t,l,i,w,h,b,s,p);}
void glTexSubImage2D(GLenum t,GLint l,GLint x,GLint y,GLsizei w,GLsizei h,GLenum f,GLenum ty,const void *p){gl2x_glTexSubImage2D(g_ctx,t,l,x,y,w,h,f,ty,p);}
void glGenerateMipmap(GLenum t){gl2x_glGenerateMipmap(g_ctx,t);}
void glCopyTexImage2D(GLenum t,GLint l,GLenum i,GLint x,GLint y,GLsizei w,GLsizei h,GLint b){gl2x_glCopyTexImage2D(g_ctx,t,l,i,x,y,w,h,b);}
void glCopyTexSubImage2D(GLenum t,GLint l,GLint xo,GLint yo,GLint x,GLint y,GLsizei w,GLsizei h){gl2x_glCopyTexSubImage2D(g_ctx,t,l,xo,yo,x,y,w,h);}
void glReadPixels(GLint x,GLint y,GLsizei w,GLsizei h,GLenum f,GLenum t,void *p){if(g_changed)g_changed();gl2x_glReadPixels(g_ctx,x,y,w,h,f,t,p);}
void glGenFramebuffers(GLsizei n,GLuint *f){gl2x_glGenFramebuffers(g_ctx,n,(uint32_t*)f);}
void glBindFramebuffer(GLenum t,GLuint f){if(g_changed)g_changed();gl2x_glBindFramebuffer(g_ctx,t,f);}
void glDeleteFramebuffers(GLsizei n,const GLuint *f){gl2x_glDeleteFramebuffers(g_ctx,n,(const uint32_t*)f);}
GLboolean glIsFramebuffer(GLuint f){return (GLboolean)gl2x_glIsFramebuffer(g_ctx,f);}
GLenum glCheckFramebufferStatus(GLenum t){return (GLenum)gl2x_glCheckFramebufferStatus(g_ctx,t);}
void glFramebufferTexture2D(GLenum t,GLenum a,GLenum tt,GLuint x,GLint l){gl2x_glFramebufferTexture2D(g_ctx,t,a,tt,x,l);}
void glFramebufferRenderbuffer(GLenum t,GLenum a,GLenum rt,GLuint r){gl2x_glFramebufferRenderbuffer(g_ctx,t,a,rt,r);}
void glGetFramebufferAttachmentParameteriv(GLenum t,GLenum a,GLenum p,GLint *v){gl2x_glGetFramebufferAttachmentParameteriv(g_ctx,t,a,p,v);}
void glGenRenderbuffers(GLsizei n,GLuint *r){gl2x_glGenRenderbuffers(g_ctx,n,(uint32_t*)r);}
void glBindRenderbuffer(GLenum t,GLuint r){gl2x_glBindRenderbuffer(g_ctx,t,r);}
void glDeleteRenderbuffers(GLsizei n,const GLuint *r){gl2x_glDeleteRenderbuffers(g_ctx,n,(const uint32_t*)r);}
GLboolean glIsRenderbuffer(GLuint r){return (GLboolean)gl2x_glIsRenderbuffer(g_ctx,r);}
void glRenderbufferStorage(GLenum t,GLenum i,GLsizei w,GLsizei h){gl2x_glRenderbufferStorage(g_ctx,t,i,w,h);}
void glGetRenderbufferParameteriv(GLenum t,GLenum p,GLint *v){gl2x_glGetRenderbufferParameteriv(g_ctx,t,p,v);}
