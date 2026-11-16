/* Representative multipass GLES2 application: texture -> FBO pass -> filtered compositing pass. */
#include "GLES2/gl2.h"
#include <stdint.h>
static int same_name(const char*a,const char*b){while(*a&&*a==*b){++a;++b;}return *a==*b;}
static GLint find_uniform(GLuint p,const char*want){GLint n=0;glGetProgramiv(p,GL_ACTIVE_UNIFORMS,&n);for(GLint i=0;i<n;++i){GLchar nm[64];GLsizei len=0;GLint sz=0;GLenum ty=0;glGetActiveUniform(p,(GLuint)i,sizeof nm,&len,&sz,&ty,nm);if(same_name(nm,want))return glGetUniformLocation(p,nm);}return -1;}
static GLuint sh(GLenum t,const char*s){GLuint o=glCreateShader(t);glShaderSource(o,1,&s,0);glCompileShader(o);GLint ok=0;glGetShaderiv(o,GL_COMPILE_STATUS,&ok);return ok?o:0;}
static GLuint prog(const char*vs,const char*fs){GLuint v=sh(GL_VERTEX_SHADER,vs),f=sh(GL_FRAGMENT_SHADER,fs);if(!v||!f)return 0;GLuint p=glCreateProgram();glAttachShader(p,v);glAttachShader(p,f);glLinkProgram(p);GLint ok=0;glGetProgramiv(p,GL_LINK_STATUS,&ok);return ok?p:0;}
int app_multipass(void){
 static const GLfloat pos[16]={-1,-1,0,1, 1,-1,0,1, -1,1,0,1, 1,1,0,1};
 static const GLfloat uv[8]={0,0, 1,0, 0,1, 1,1};
 uint8_t src[16*16*4];for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x){unsigned i=(y*16+x)*4;src[i]=(uint8_t)(x*17);src[i+1]=(uint8_t)(y*17);src[i+2]=0x40;src[i+3]=255;}
 GLuint in_tex;glGenTextures(1,&in_tex);glBindTexture(GL_TEXTURE_2D,in_tex);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,16,16,0,GL_RGBA,GL_UNSIGNED_BYTE,src);
 GLuint first=prog("attribute vec4 aPosition;attribute vec2 aTexCoord;varying vec2 vUV;vec2 place(vec4 p){return p.xy;}void main(){gl_Position=vec4(place(aPosition),aPosition.zw);vUV=aTexCoord;}",
  "precision mediump float;uniform sampler2D uTex;varying vec2 vUV;void main(){gl_FragColor=texture2D(uTex,vUV);}");
 if(!first)return -1;
 GLuint fbo_tex;glGenTextures(1,&fbo_tex);glBindTexture(GL_TEXTURE_2D,fbo_tex);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,16,16,0,GL_RGBA,GL_UNSIGNED_BYTE,0);
 GLuint fbo,rbo;glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,fbo_tex,0);
 glGenRenderbuffers(1,&rbo);glBindRenderbuffer(GL_RENDERBUFFER,rbo);glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT16,16,16);glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,rbo);
 if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)return -2;
 glViewport(0,0,16,16);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);glUseProgram(first);glBindTexture(GL_TEXTURE_2D,in_tex);
 GLint a=glGetAttribLocation(first,"aPosition"),t=glGetAttribLocation(first,"aTexCoord");glVertexAttribPointer((GLuint)a,4,GL_FLOAT,GL_FALSE,0,pos);glEnableVertexAttribArray((GLuint)a);glVertexAttribPointer((GLuint)t,2,GL_FLOAT,GL_FALSE,0,uv);glEnableVertexAttribArray((GLuint)t);glUniform1i(find_uniform(first,"uTex"),0);glDrawArrays(GL_TRIANGLE_STRIP,0,4);
 if(glGetError()!=GL_NO_ERROR)return -3;
 glBindFramebuffer(GL_FRAMEBUFFER,0);
 GLuint second=prog("attribute vec4 aPosition;attribute vec2 aTexCoord;varying vec2 vUV;vec2 place(vec4 p){return p.xy;}void main(){gl_Position=vec4(place(aPosition),aPosition.zw);vUV=aTexCoord;}",
  "precision mediump float;uniform sampler2D uTex;varying vec2 vUV;void main(){gl_FragColor=texture2D(uTex,vUV);}");
 if(!second)return -4;
 glViewport(0,0,16,16);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);glUseProgram(second);glBindTexture(GL_TEXTURE_2D,fbo_tex);
 a=glGetAttribLocation(second,"aPosition");t=glGetAttribLocation(second,"aTexCoord");glVertexAttribPointer((GLuint)a,4,GL_FLOAT,GL_FALSE,0,pos);glEnableVertexAttribArray((GLuint)a);glVertexAttribPointer((GLuint)t,2,GL_FLOAT,GL_FALSE,0,uv);glEnableVertexAttribArray((GLuint)t);glUniform1i(find_uniform(second,"uTex"),0);glDrawArrays(GL_TRIANGLE_STRIP,0,4);
 if(glGetError()!=GL_NO_ERROR)return -5;
 return 0;
}
