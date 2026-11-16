/* Standard GLES2 API ABI smoke test (no custom gl2_ctx in app code). */
#include "GLES2/gl2.h"
#include "gl2std.h"
#include <assert.h>
#include <stdio.h>
int main(void){
 assert(gl2std_host_init()==0);
 glViewport(0,0,16,16);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
 const GLchar *vs="attribute vec4 aPosition;void main(){gl_Position=aPosition;}";
 const GLchar *fs="precision mediump float;uniform vec4 uColor;void main(){gl_FragColor=uColor;}";
 GLuint v=glCreateShader(GL_VERTEX_SHADER),f=glCreateShader(GL_FRAGMENT_SHADER);glShaderSource(v,1,&vs,0);glShaderSource(f,1,&fs,0);glCompileShader(v);glCompileShader(f);
 GLint ok=0;glGetShaderiv(v,GL_COMPILE_STATUS,&ok);assert(ok);glGetShaderiv(f,GL_COMPILE_STATUS,&ok);assert(ok);
 GLuint p=glCreateProgram();glAttachShader(p,v);glAttachShader(p,f);glLinkProgram(p);glGetProgramiv(p,GL_LINK_STATUS,&ok);assert(ok);glUseProgram(p);
 static const GLfloat verts[16]={-1,-1,0,1, 1,-1,0,1, -1,1,0,1, 1,1,0,1};
 GLint n=0;glGetProgramiv(p,GL_ACTIVE_UNIFORMS,&n);assert(n>=1);
 {GLint sz=0;GLenum ty=0;GLchar nm[64];GLsizei len=0;glGetActiveUniform(p,0,sizeof nm,&len,&sz,&ty,nm);assert(len==6&&sz==1&&ty==GL_FLOAT_VEC4);
  for(int i=0;i<6;++i)assert(nm[i]=="uColor"[i]);assert(nm[6]==0);}
 {GLint an=0;glGetProgramiv(p,GL_ACTIVE_ATTRIBUTES,&an);assert(an>=1);GLint sz=0;GLenum ty=0;GLchar nm[64];GLsizei len=0;glGetActiveAttrib(p,0,sizeof nm,&len,&sz,&ty,nm);assert(len==9&&sz==1&&ty==GL_FLOAT_VEC4);for(int i=0;i<9;++i)assert(nm[i]=="aPosition"[i]);}
 GLint uloc=glGetUniformLocation(p,"uColor");assert(uloc>=0);glUniform4f(uloc,1.0f,0.5f,0.25f,1.0f);
 GLint a=glGetAttribLocation(p,"aPosition");assert(a>=0);glVertexAttribPointer((GLuint)a,4,GL_FLOAT,GL_FALSE,0,verts);glEnableVertexAttribArray((GLuint)a);glDrawArrays(GL_TRIANGLE_STRIP,0,4);assert(glGetError()==GL_NO_ERROR);
 uint32_t *px=gl2std_host_target();assert(px[0]==0xffff8040u);assert(px[8*16+8]==0xffff8040u);
 puts("PASS standard GLES2 ABI: shader/program/attrib/draw over the prefixed driver");return 0;
}
