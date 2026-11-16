#include "GLES2/gl2.h"
#include "gl2std.h"
#include <assert.h>
#include <stdio.h>
int app_multipass(void);
int main(void){assert(gl2std_host_init()==0);int rc=app_multipass();if(rc)return rc;assert(rc==0);assert(glGetError()==GL_NO_ERROR);uint32_t*p=gl2std_host_target();uint32_t c=p[8*16+8];assert((c>>24)==255);uint32_t r=c>>16&255,g=c>>8&255,b=c&255;assert(r>=134&&r<=138&&g>=117&&g<=121&&b>=62&&b<=66);printf("PASS standard multipass app: texture -> FBO -> composite (0x%08x)\n",c);return 0;}
