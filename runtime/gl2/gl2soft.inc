/* Floating-point draw fallback. Private to gl2api.c, serialized scratch. */
static int s_finite(float x){return(f_bits(x)&0x7f800000u)!=0x7f800000u;}
static float s_clamp(float x){return x<0?0:x>1?1:x;}
int gl2_soft_validate(const gl2_soft_program *p){
 if(!p||!p->count||p->count>GL2_SOFT_INSNS)return -1;
 for(unsigned i=0;i<p->count;++i){const gl2_soft_insn *s=&p->code[i];if(s->op==GL2_SCONST){if(!s_finite(s->value))return -1;}else if(s->op==GL2_SINPUT){if(s->a>=GL2_SOFT_INPUTS)return -1;}else if(s->op>=GL2_SADD&&s->op<=GL2_SCLAMP){if(s->a>=i||(s->op!=GL2_SCLAMP&&s->b>=i))return -1;}else return -1;}
 for(unsigned k=0;k<4;++k){if(p->output[k]>=p->count)return -1;}return 0;
}
static void s_fragment(const gl2_soft_program *p,const float *primary,const float *texel,const float uniforms[16][4],float *rgba){
 float r[GL2_SOFT_INSNS];for(unsigned i=0;i<p->count;++i){const gl2_soft_insn *s=&p->code[i];switch(s->op){case GL2_SCONST:r[i]=s->value;break;case GL2_SINPUT:r[i]=s->a<4?primary[s->a]:s->a<8?texel[s->a-4]:uniforms[(s->a-8)/4][(s->a-8)%4];break;case GL2_SADD:r[i]=r[s->a]+r[s->b];break;case GL2_SMUL:r[i]=r[s->a]*r[s->b];break;case GL2_SSUB:r[i]=r[s->a]-r[s->b];break;default:r[i]=s_clamp(r[s->a]);break;}}
 for(unsigned k=0;k<4;++k)rgba[k]=s_clamp(r[p->output[k]]);
}
int gl2_soft_fragment_uniforms(const gl2_soft_program *p,const float primary[4],const float texel[4],const float uniforms[16][4],float rgba[4]){
 if(gl2_soft_validate(p)||!rgba)return -1;
 for(unsigned i=0;i<p->count;++i)if(p->code[i].op==GL2_SINPUT){unsigned a=p->code[i].a;const float *v=a<4?primary?primary+a:0:a<8?texel?texel+a-4:0:uniforms?&uniforms[(a-8)/4][(a-8)%4]:0;if(!v||!s_finite(*v))return -1;}
 float out[4];s_fragment(p,primary,texel,uniforms,out);for(unsigned k=0;k<4;++k)if(!s_finite(out[k]))return -1;for(unsigned k=0;k<4;++k)rgba[k]=out[k];return 0;
}
int gl2_soft_fragment(const gl2_soft_program *p,const float primary[4],const float texel[4],float rgba[4]){return gl2_soft_fragment_uniforms(p,primary,texel,0,rgba);}

static int s_input_channel(const gl2_soft_program *p,unsigned id,unsigned channel){return id<p->count&&p->code[id].op==GL2_SINPUT&&p->code[id].a==channel+4;}
static int s_square_channel(const gl2_soft_program *p,unsigned id,unsigned channel){return id<p->count&&p->code[id].op==GL2_SMUL&&s_input_channel(p,p->code[id].a,channel)&&s_input_channel(p,p->code[id].b,channel);}
int gl2_soft_is_texture_cube(const gl2_soft_program *p){
 if(!p||gl2_soft_validate(p))return 0;
 for(unsigned k=0;k<4;++k){const gl2_soft_insn *i=&p->code[p->output[k]];if(i->op!=GL2_SMUL||!((s_input_channel(p,i->a,k)&&s_square_channel(p,i->b,k))||(s_input_channel(p,i->b,k)&&s_square_channel(p,i->a,k))))return 0;}return 1;
}
int gl2_program_needs_software(const gl2_ctx *c,const gl2_prog_obj *p){
 if(c->scissor_enabled||c->color_mask!=0xfu)return 1;
 const gl2_soft_program *s=p&&p->fs?p->fs->software:0;if(!s)return 0;
 /* A compiled combiner plan is an exact fixed-function lowering of the
  * fragment AST. If the sampler/state are in the verified GPU set, execute the
  * plan on hardware instead of forcing the float fallback. Blending, packed
  * source formats and non-nearest filtering stay on the software route. */
 if(p->fs->nplan&&!c->st.blend){
  unsigned unit=p->sampler_unit[0];
  if(unit<2){
   if(!c->texture_managed[unit]){if(c->raw_unit_tex[unit].storage==GL2_TEX_ARGB8888)return 0;}
   else for(unsigned i=0;i<GL2_MAX_TEXTURES;++i){const gl2_texture_obj *t=&c->textures[i];if(t->used&&t->defined&&t->name==c->texture_binding[unit]&&t->storage==GL2_TEX_ARGB8888&&t->min_filter==GL_NEAREST&&t->mag_filter==GL_NEAREST)return 0;}
  }
 }
 /* Exhaustive physical RGBA8 colour/alpha matrix verifies the unblended cubic
  * path within one byte. Blending and packed formats keep the float route. */
 if(p->prog.rounding_bias&&!c->st.blend){
  unsigned unit=p->sampler_unit[0];if(unit<2){
   if(!c->texture_managed[unit]){if(c->raw_unit_tex[unit].storage==GL2_TEX_ARGB8888)return 0;}
   else for(unsigned i=0;i<GL2_MAX_TEXTURES;++i){const gl2_texture_obj *t=&c->textures[i];if(t->used&&t->defined&&t->name==c->texture_binding[unit]&&t->storage==GL2_TEX_ARGB8888&&t->min_filter==GL_NEAREST&&t->mag_filter==GL_NEAREST)return 0;}
  }
 }
 /* Exact single-pass flat/modulate combiners (C, P, T, T*C, T*P) run natively
  * on the MBX Lite GPU even when blending or linear filtering is enabled. */
 if(p->fs->nplan==1&&p->fs->nsampler<=1&&!p->prog.rounding_bias&&!gl2_soft_is_texture_cube(s)){
  int ok=1;
  for(unsigned k=0;k<4;++k){
   const gl2_soft_insn *i=&s->code[s->output[k]];
   if(i->op==GL2_SINPUT||i->op==GL2_SCONST)continue;
   if(i->op==GL2_SMUL){
    const gl2_soft_insn *a=&s->code[i->a],*b=&s->code[i->b];
    if((a->op==GL2_SINPUT&&a->a==k+4&&b->op==GL2_SINPUT&&b->a!=k+4)||
       (b->op==GL2_SINPUT&&b->a==k+4&&a->op==GL2_SINPUT&&a->a!=k+4))continue;
   }
   ok=0;break;
  }
  if(ok)return 0;
 }
 /* Direct RGBA texture transfer/opaque RGB is measured exact on the GPU.
  * Other uncached expressions retain a float execution route. */
 for(unsigned k=0;k<3;++k){const gl2_soft_insn *i=&s->code[s->output[k]];if(i->op!=GL2_SINPUT||i->a!=k+4)return 1;}
 const gl2_soft_insn *a=&s->code[s->output[3]];return !((a->op==GL2_SINPUT&&a->a==7)||(a->op==GL2_SCONST&&a->value==1));
}
typedef struct {float v[5][4];} s_vertex_out;
static s_vertex_out s_vertices[256];
static float s_rsqrt(float x){union{float f;uint32_t u;}v={x<0?-x:x};if(!v.f){v.u=0x7f800000u;return v.f;}float half=.5f*v.f;v.u=0x5f3759dfu-(v.u>>1);float y=v.f;for(unsigned k=0;k<4;++k)y=y*(1.5f-half*y*y);return y;}
static float s_exp2(float x){if(!s_finite(x))return x;if(x<=-126)return 0;if(x>=128){union{float f;uint32_t u;}v;v.u=0x7f800000u;return v.f;}int n=(int)x;if((float)n>x)--n;float t=x-(float)n;union{float f;uint32_t u;}v;v.u=(uint32_t)(n+127)<<23;return v.f*(1+t*(.69314718056f+t*(.24022650696f+t*(.05550410866f+t*(.00961812911f+t*.00133335581f)))));}
static int s_vertex_execute(const gl2_prog_obj *p,const float in[5][4],const float constants[128][4],s_vertex_out *out){
 float temp[16][4]={{0}},clip_distances[4]={0};for(unsigned i=0;i<5;++i)for(unsigned k=0;k<4;++k)out->v[i][k]=k==3?1:0;
 for(unsigned pc=0;pc<p->vs.ncode;++pc){uint32_t a=p->vs.code[pc][0],b=p->vs.code[pc][1],op=a&31,dst=b>>26,mask=b>>22&15;float s[3][4],r[4];
  for(unsigned q=0;q<3;++q){unsigned kind=b>>(16+2*q)&3,index=b>>(4*q)&15,sw=a>>(8+8*q)&255;const float *v=0;if(!kind){if(index>=5)return -1;v=in[index];}else if(kind==1){unsigned ci=((b>>12&15)<<4)|index;if(ci>=128)return -1;v=constants[ci];}else if(kind==2)v=temp[index];for(unsigned k=0;k<4;++k){s[q][k]=v?v[sw>>(2*k)&3]:0;if(a&(1u<<(5+q)))s[q][k]=-s[q][k];}}
  if(op==7||op==8){float dot=0;for(unsigned k=0;k<(op==7?3u:4u);++k)dot+=s[0][k]*s[1][k];for(unsigned k=0;k<4;++k)r[k]=dot;}
  else for(unsigned k=0;k<4;++k)switch(op){case 1:r[k]=s[0][k];break;case 2:r[k]=s[0][k]+s[1][k];break;case 3:r[k]=s[0][k]*s[1][k]+s[2][k];break;case 4:r[k]=s[0][k]*s[1][k];break;case 5:r[k]=1/s[0][3];break;case 6:r[k]=s_rsqrt(s[0][3]);break;case 9:r[k]=s[0][k]<s[1][k]?s[0][k]:s[1][k];break;case 10:r[k]=s[0][k]>s[1][k]?s[0][k]:s[1][k];break;case 12:r[k]=s[0][k]>=s[1][k]?1:0;break;case 19:r[k]=s_exp2(s[0][3]);break;default:return -1;}
  float *d=dst>=16&&dst<32?temp[dst-16]:dst<5?out->v[dst]:dst==5?clip_distances:0;if(!d)return -1;for(unsigned k=0;k<4;++k)if(mask&(1u<<k))d[k]=r[k];
 }
 /* VGP colour output is BGRA; preserve its float values, without RGBA8 packing. */
 float red=out->v[1][2];out->v[1][2]=out->v[1][0];out->v[1][0]=red;
 for(unsigned k=0;k<4;++k){if(!s_finite(out->v[0][k]))return -1;}return 0;
}
static uint32_t s_spread(uint32_t v){v=(v|(v<<8))&0x00ff00ffu;v=(v|(v<<4))&0x0f0f0f0fu;v=(v|(v<<2))&0x33333333u;return(v|(v<<1))&0x55555555u;}
static float s_fract(float v){if(v>=16777216.f||v<=-16777216.f)return 0;float r=v-(float)(int)v;return r<0?r+1:r;}
static uint32_t s_tex_index(const gl2_tex *t,float u,float v){
 if(!s_finite(u))u=0;
 if(!s_finite(v))v=0;
 unsigned size=1u<<t->log2size;
 if(t->storage&GL2_TEX_CLAMP_S)
  u=u<0?0:u>1?1:u;
 else
  u=s_fract(u);
 if(t->storage&GL2_TEX_CLAMP_T)
  v=v<0?0:v>1?1:v;
 else
  v=s_fract(v);
 unsigned x=(unsigned)(u*size),y=(unsigned)(v*size);if(x>=size)x=size-1;if(y>=size)y=size-1;return(s_spread(x)<<1)|s_spread(y);
}
static const uint16_t s_morton[64]={0,1,4,5,16,17,20,21,64,65,68,69,80,81,84,85,256,257,260,261,272,273,276,277,320,321,324,325,336,337,340,341,1024,1025,1028,1029,1040,1041,1044,1045,1088,1089,1092,1093,1104,1105,1108,1109,1280,1281,1284,1285,1296,1297,1300,1301,1344,1345,1348,1349,1360,1361,1364,1365};
static uint32_t s_fixed_uv(float value){if(!s_finite(value))return 0;float v=s_fract(value);return v>=1?UINT32_MAX:(uint32_t)(v*4294967296.f);}
static uint32_t s_fixed_uv_clamp(float value){if(!s_finite(value))return 0;if(value<=0)return 0;if(value>=1)return UINT32_MAX;return(uint32_t)(value*4294967296.f);}
static void s_texel(const gl2_ctx *c,const gl2_tex *t,uint32_t index,float *out){
 const uint8_t *p=c->texture_cpu+(t->va-c->texture_va);unsigned format=t->storage&0x1fu,bpp=format?2:4;p+=index*bpp;uint32_t v=p[0]|(uint32_t)p[1]<<8;
 if(!format){v|=(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;out[0]=(v>>16&255)/255.f;out[1]=(v>>8&255)/255.f;out[2]=(v&255)/255.f;out[3]=(v>>24)/255.f;}
 else if(format==GL2_TEX_RGB565){out[0]=(v>>11)/31.f;out[1]=(v>>5&63)/63.f;out[2]=(v&31)/31.f;out[3]=1;}
 else if(format==GL2_TEX_ARGB4444){out[0]=(v>>8&15)/15.f;out[1]=(v>>4&15)/15.f;out[2]=(v&15)/15.f;out[3]=(v>>12)/15.f;}
 else if(format==GL2_TEX_AL88){out[0]=out[1]=out[2]=(v&255)/255.f;out[3]=(v>>8)/255.f;}
 else{out[0]=(v>>10&31)/31.f;out[1]=(v>>5&31)/31.f;out[2]=(v&31)/31.f;out[3]=(v>>15)?1:0;}
}
/* Q32 coordinates, shifted by half a texel before interpolation. */
static void s_bilinear(const gl2_ctx *c,const gl2_tex *t,uint32_t u,uint32_t v,float *out){
 unsigned shift=32u-t->log2size,size=1u<<t->log2size,mask=(1u<<shift)-1u;uint32_t half=1u<<(shift-1u);
 if(t->storage&GL2_TEX_CLAMP_S){
  if(u<half)u=half;
  if(u>0u-half)u=0u-half;
 }
 if(t->storage&GL2_TEX_CLAMP_T){
  if(v<half)v=half;
  if(v>0u-half)v=0u-half;
 }
 u-=half;v-=half;
 unsigned x=u>>shift,y=v>>shift;float a=(float)(u&mask)/(float)(1u<<shift),b=(float)(v&mask)/(float)(1u<<shift),q[4][4];
 int x0,x1,y0,y1;
 if(t->storage&GL2_TEX_CLAMP_S){
  x0=(int)x;
  x1=x+1<size?(int)(x+1):(int)(size-1);
 }else{
  x0=(int)(x&(size-1u));
  x1=(x0+1)&((int)size-1);
 }
 if(t->storage&GL2_TEX_CLAMP_T){
  y0=(int)y;
  y1=y+1<size?(int)(y+1):(int)(size-1);
 }else{
  y0=(int)(y&(size-1u));
  y1=(y0+1)&((int)size-1);
 }
 for(unsigned j=0;j<2;++j)for(unsigned i=0;i<2;++i)s_texel(c,t,(s_spread((unsigned)(i?x1:x0))<<1)|s_spread((unsigned)(j?y1:y0)),q[j*2+i]);
 for(unsigned k=0;k<4;++k){float top=q[0][k]+a*(q[1][k]-q[0][k]),bottom=q[2][k]+a*(q[3][k]-q[2][k]);out[k]=top+b*(bottom-top);}
}
static unsigned s_byte(float v){if(!(v>0))return 0;if(v>=1)return 255;return(unsigned)(v*255.f+.5f);}
static float s_blend_factor(unsigned mode,unsigned channel,const float *src,const float *dst){switch(mode){case 0:return 0;case 1:return 1;case 0x300:return src[channel];case 0x301:return 1-src[channel];case 0x302:return src[3];case 0x303:return 1-src[3];case 0x304:return dst[3];case 0x305:return 1-dst[3];case 0x306:return dst[channel];case 0x307:return 1-dst[channel];case 0x308:return channel==3?1:(src[3]<1-dst[3]?src[3]:1-dst[3]);default:return 0;}}
static uint32_t s_pixel(const gl2_state *st,const float *src,uint32_t old,uint32_t cmask){float dst[4]={(old>>16&255)/255.f,(old>>8&255)/255.f,(old&255)/255.f,(old>>24)/255.f},v[4];for(unsigned k=0;k<4;++k)v[k]=st->blend?src[k]*s_blend_factor(st->src,k,src,dst)+dst[k]*s_blend_factor(st->dst,k,src,dst):src[k];uint32_t pixel=s_byte(v[3])<<24|s_byte(v[0])<<16|s_byte(v[1])<<8|s_byte(v[2]);return (old&~cmask)|(pixel&cmask);}
static int s_depth(unsigned f,float z,float old){switch(f){case 0x200:return 0;case 0x201:return z<old;case 0x202:return z==old;case 0x203:return z<=old;case 0x204:return z>old;case 0x205:return z!=old;case 0x206:return z>=old;default:return 1;}}
/* Tagged bounded cache: collisions cost work, never change shader results. */
#define S_CACHE_SIZE 256
static float s_cache_rgba[S_CACHE_SIZE][4];static uint32_t s_cache_old[S_CACHE_SIZE],s_cache_pixel[S_CACHE_SIZE];static uint16_t s_cache_tag[S_CACHE_SIZE];static uint8_t s_cache_valid[S_CACHE_SIZE],s_cache_blend[S_CACHE_SIZE];
static float s_distance(const s_vertex_out *v,unsigned plane){return v->v[0][3]+(plane&1?-v->v[0][plane/2]:v->v[0][plane/2]);}
static int s_clip(const s_vertex_out *a,const s_vertex_out *b,const s_vertex_out *d,s_vertex_out out[12]){
 s_vertex_out p[2][12];p[0][0]=*a;p[0][1]=*b;p[0][2]=*d;unsigned n=3,source=0;
 for(unsigned plane=0;plane<6&&n;++plane){unsigned count=0;for(unsigned i=0;i<n;++i){const s_vertex_out *v=&p[source][i],*w=&p[source][(i+1)%n];float dv=s_distance(v,plane),dw=s_distance(w,plane);if(dv>=0){if(count>=12)return -1;p[1-source][count++]=*v;}if((dv>=0)!=(dw>=0)){if(count>=12)return -1;float t=dv/(dv-dw);s_vertex_out *x=&p[1-source][count++];for(unsigned q=0;q<5;++q)for(unsigned k=0;k<4;++k)x->v[q][k]=v->v[q][k]+t*(w->v[q][k]-v->v[q][k]);}}n=count;source=1-source;}
 for(unsigned i=0;i<n;++i){out[i]=p[source][i];}return(int)n;
}
static int32_t s_edge(int32_t ax,int32_t ay,int32_t bx,int32_t by,int32_t x,int32_t y){return(bx-ax)*(y-ay)-(by-ay)*(x-ax);}
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("O2")))
#endif
static void s_triangle(gl2_ctx *restrict c,const gl2_prog_obj *p,const s_vertex_out *vertices[3],int constant_primary){
 const s_vertex_out *v[3]={vertices[0],vertices[1],vertices[2]};int32_t x[3],y[3];float iw[3],z[3];
 for(unsigned i=0;i<3;++i){if(!(v[i]->v[0][3]>0))return;iw[i]=1/v[i]->v[0][3];if(!s_finite(iw[i]))return;x[i]=(int32_t)((v[i]->v[0][0]*iw[i]+1)*(float)c->vp_w*8+.5f);y[i]=(int32_t)((1-v[i]->v[0][1]*iw[i])*(float)c->vp_h*8+.5f);z[i]=(v[i]->v[0][2]*iw[i]+1)*.5f;}
 int32_t area=s_edge(x[0],y[0],x[1],y[1],x[2],y[2]);if(!area)return;
 if(c->cull_enabled){int front=(area<0)==(c->front_face==GL_CCW);if(c->cull_face==GL_FRONT_AND_BACK||(c->cull_face==GL_FRONT?front:!front))return;}
 if(area<0){const s_vertex_out *tmp=v[1];v[1]=v[2];v[2]=tmp;int32_t tx=x[1],ty=y[1];x[1]=x[2];y[1]=y[2];x[2]=tx;y[2]=ty;float f=iw[1];iw[1]=iw[2];iw[2]=f;f=z[1];z[1]=z[2];z[2]=f;area=-area;}
 int minx=x[0],maxx=x[0],miny=y[0],maxy=y[0];for(unsigned k=1;k<3;++k){if(x[k]<minx)minx=x[k];if(x[k]>maxx)maxx=x[k];if(y[k]<miny)miny=y[k];if(y[k]>maxy)maxy=y[k];}minx/=16;miny/=16;maxx=(maxx+15)/16;maxy=(maxy+15)/16;if(maxx>(int)c->vp_w)maxx=(int)c->vp_w;if(maxy>(int)c->vp_h)maxy=(int)c->vp_h;
 if(c->scissor_enabled){
  int gx0=c->scissor_box[0],gy0=c->scissor_box[1],gx1=gx0+c->scissor_box[2],gy1=gy0+c->scissor_box[3];
  if(gx0<0)gx0=0;
  if(gy0<0)gy0=0;
  if(gx1>(int)c->soft_width)gx1=(int)c->soft_width;
  if(gy1>(int)c->soft_height)gy1=(int)c->soft_height;
  int ty0=(int)c->soft_height-gy1,ty1=(int)c->soft_height-gy0;
  if(gx1<=gx0||ty1<=ty0)return;
  if(minx<gx0)minx=gx0;
  if(maxx>gx1)maxx=gx1;
  if(miny<ty0)miny=ty0;
  if(maxy>ty1)maxy=ty1;
  if(maxx<=minx||maxy<=miny)return;
 }
 int32_t dx[3],dy[3],row[3];int top[3];for(unsigned i=0;i<3;++i){unsigned a=(i+1)%3,b=(i+2)%3;dx[i]=-(y[b]-y[a])*16;dy[i]=(x[b]-x[a])*16;top[i]=y[b]<y[a]||(y[b]==y[a]&&x[b]>x[a]);row[i]=s_edge(x[a],y[a],x[b],y[b],minx*16+8,miny*16+8);}
 uint32_t *restrict target=c->soft_target;float *restrict depths=c->soft_depth;
 int linear=p->fs->nsampler&&(c->tex[0].storage&GL2_TEX_LINEAR),cacheable=constant_primary&&!linear;
 int clamp_uv=c->tex[0].storage&(GL2_TEX_CLAMP_S|GL2_TEX_CLAMP_T);uint32_t cmask=0;if(c->color_mask&1)cmask|=0x00ff0000u;if(c->color_mask&2)cmask|=0x0000ff00u;if(c->color_mask&4)cmask|=0x000000ffu;if(c->color_mask&8)cmask|=0xff000000u;
 float inv=1.f/(float)area;int affine=iw[0]==iw[1]&&iw[0]==iw[2]&&!clamp_uv;float uv_dx[2]={0,0};if(affine)for(unsigned k=0;k<2;++k)for(unsigned i=0;i<3;++i)uv_dx[k]+=(float)dx[i]*inv*v[i]->v[3][k];
 uint32_t uv_step[2]={s_fixed_uv(uv_dx[0]),s_fixed_uv(uv_dx[1])};
 for(int yy=miny;yy<maxy;++yy){int32_t e[3]={row[0],row[1],row[2]};float uv[2]={0,0};if(affine)for(unsigned k=0;k<2;++k)for(unsigned i=0;i<3;++i)uv[k]+=(float)e[i]*inv*v[i]->v[3][k];
  uint32_t fixed_uv[2]={s_fixed_uv(uv[0]),s_fixed_uv(uv[1])};
  for(int xx=minx;xx<maxx;++xx){if((e[0]>0||(e[0]==0&&top[0]))&&(e[1]>0||(e[1]==0&&top[1]))&&(e[2]>0||(e[2]==0&&top[2]))){unsigned offset=(unsigned)yy*c->soft_stride+(unsigned)xx;float lambda[3]={0,0,0},depth=0;if(c->st.depth_test||!affine||!constant_primary){for(unsigned i=0;i<3;++i)lambda[i]=(float)e[i]*inv;}if(c->st.depth_test)depth=lambda[0]*z[0]+lambda[1]*z[1]+lambda[2]*z[2];
   if(!c->st.depth_test||s_depth(c->st.depth_func,depth,depths[(unsigned)yy*c->soft_width+(unsigned)xx])){float primary[4],coords[2];float q=affine?1:lambda[0]*iw[0]+lambda[1]*iw[1]+lambda[2]*iw[2];
    
    if(!affine)for(unsigned k=0;k<2;++k){coords[k]=0;for(unsigned i=0;i<3;++i)coords[k]+=lambda[i]*v[i]->v[3][k]*iw[i]/q;}
    uint32_t index=!p->fs->nsampler?0:affine?((uint32_t)s_morton[fixed_uv[0]>>(32-c->tex[0].log2size)]<<1)|s_morton[fixed_uv[1]>>(32-c->tex[0].log2size)]:s_tex_index(&c->tex[0],coords[0],coords[1]),old=target[offset],slot=index&(S_CACHE_SIZE-1);int cached=!linear&&s_cache_valid[slot]&&s_cache_tag[slot]==index;float texel[4],colour[4];uint32_t pixel;
    if(constant_primary&&cached&&s_cache_blend[slot]&&s_cache_old[slot]==old)pixel=s_cache_pixel[slot];else{
     if(constant_primary&&cached){for(unsigned k=0;k<4;++k)colour[k]=s_cache_rgba[slot][k];}else{for(unsigned k=0;k<4;++k){primary[k]=constant_primary?v[0]->v[1][k]:0;if(!constant_primary)for(unsigned i=0;i<3;++i)primary[k]+=lambda[i]*v[i]->v[1][k]*(affine?1:iw[i]/q);}if(p->fs->nsampler){
  if(linear){
   uint32_t qu=affine?fixed_uv[0]:((c->tex[0].storage&GL2_TEX_CLAMP_S)?s_fixed_uv_clamp(coords[0]):s_fixed_uv(coords[0]));
   uint32_t qv=affine?fixed_uv[1]:((c->tex[0].storage&GL2_TEX_CLAMP_T)?s_fixed_uv_clamp(coords[1]):s_fixed_uv(coords[1]));
      s_bilinear(c,&c->tex[0],qu,qv,texel);
  }else s_texel(c,&c->tex[0],index,texel);
 }s_fragment(p->fs->software,primary,texel,p->fragment_values,colour);if(cacheable){for(unsigned k=0;k<4;++k)s_cache_rgba[slot][k]=colour[k];s_cache_valid[slot]=1;s_cache_tag[slot]=(uint16_t)index;s_cache_blend[slot]=0;}}
     pixel=s_pixel(&c->st,colour,old,cmask);if(cacheable){s_cache_old[slot]=old;s_cache_pixel[slot]=pixel;s_cache_blend[slot]=1;}
    }target[offset]=pixel;if(c->st.depth_test&&c->st.depth_write)depths[(unsigned)yy*c->soft_width+(unsigned)xx]=depth;
   }
  }for(unsigned i=0;i<3;++i)e[i]+=dx[i];if(affine){fixed_uv[0]+=uv_step[0];fixed_uv[1]+=uv_step[1];}}
  for(unsigned i=0;i<3;++i)row[i]+=dy[i];
 }
}
static int s_draw(gl2_ctx *c,const gl2_prog_obj *p,unsigned mode,unsigned count,const float *pos,const float *normal,const float *col,const float *uv0,const float *uv1){
 if(!c->soft_target||!c->soft_depth||!p->fs->software||gl2_soft_validate(p->fs->software)||c->vp_w>c->soft_width||c->vp_h>c->soft_height||c->soft_width>240||c->soft_height>432)return -1;
 const gl2_tex *t=&c->tex[0];if(p->fs->nsampler){if(!c->texture_cpu||t->log2size<3||t->log2size>6||t->va<c->texture_va||(t->storage&(GL2_TEX_MIP_NEAREST|GL2_TEX_MIP_LINEAR)))return -1;unsigned bytes=(1u<<(2*t->log2size))*((t->storage&~GL2_TEX_LINEAR)?2:4);if(t->va-c->texture_va>GL2_TEXTURE_ARENA_BYTES||bytes>GL2_TEXTURE_ARENA_BYTES-(t->va-c->texture_va))return -1;}
 float constants[128][4]={{0}};for(unsigned i=0;i<p->prog.nlit;++i){unsigned index=p->prog.lit_idx[i];if(index>=128)return -1;for(unsigned k=0;k<4;++k)constants[index][k]=p->prog.lit_val[i*4+k];}for(unsigned i=0;i<p->prog.nuni;++i)if(p->prog.uset[i])for(unsigned row=0;row<p->prog.uni_cnt[i];++row){unsigned index=p->prog.uni_idx[i]+row;if(index>=128)return -1;for(unsigned k=0;k<4;++k)constants[index][k]=p->prog.uval[i][row][k];}
 for(unsigned i=0;i<count;++i){float input[5][4];for(unsigned a=0;a<5;++a)for(unsigned k=0;k<4;++k)input[a][k]=k==3?1:0;for(unsigned k=0;k<4;++k){input[0][k]=pos[i*4+k];if(normal)input[1][k]=normal[i*4+k];if(col)input[2][k]=col[i*4+k];}for(unsigned k=0;k<2;++k){input[3][k]=uv0[i*2+k];input[4][k]=uv1[i*2+k];}if(s_vertex_execute(p,input,constants,&s_vertices[i]))return -1;}
 if(c->st.depth_test&&!c->soft_depth_initialized){for(unsigned i=0;i<c->soft_width*c->soft_height;++i)c->soft_depth[i]=1;c->soft_depth_initialized=1;}
 int constant=1;for(unsigned i=1;p->fs->needs_colour&&i<count&&constant;++i)for(unsigned k=0;k<4;++k)if(s_vertices[i].v[1][k]!=s_vertices[0].v[1][k])constant=0;
 if(constant)for(unsigned i=0;i<S_CACHE_SIZE;++i)s_cache_valid[i]=s_cache_blend[i]=0;
 for(unsigned tri=0;tri+2<count;tri+=mode==GL_TRIANGLES?3:1){unsigned a=mode==GL_TRIANGLE_FAN?0:tri,b=tri+1,d=tri+2;if(mode==GL_TRIANGLE_STRIP&&(tri&1)){unsigned tmp=a;a=b;b=tmp;}s_vertex_out polygon[12];int n=s_clip(&s_vertices[a],&s_vertices[b],&s_vertices[d],polygon);if(n<0)return -1;for(int i=1;i+1<n;++i){const s_vertex_out *v[3]={&polygon[0],&polygon[i],&polygon[i+1]};s_triangle(c,p,v,constant);}}
 return 0;
}
void gl2_soft_cache_flush(void){for(unsigned i=0;i<S_CACHE_SIZE;++i){s_cache_valid[i]=0;s_cache_blend[i]=0;}}
