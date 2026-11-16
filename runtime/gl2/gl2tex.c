/* Managed base-level textures for the measured ARGB8888 twiddled MBX path.
 * No register/clock access, allocation, or implicit GPU waits. Caller supplies
 * mapped GPU storage and a cache/upload hook; see gl2api.h for lifetime rules. */
#include "gl2api.h"
static void terr(gl2_ctx *c, uint32_t e) { if (!c->err) c->err = e; }
static gl2_texture_obj *named(gl2_ctx *c, uint32_t name) {
    if (!name) return 0;
    for (uint32_t i = 0; i < GL2_MAX_TEXTURES; ++i)
        if (c->textures[i].reserved && c->textures[i].name == name) return &c->textures[i];
    return 0;
}
static void reserve(gl2_texture_obj *t, uint32_t name) {
    t->name = name; t->reserved = 1; t->used = t->defined = t->size = t->log2size = 0;
    t->format = t->storage = 0;
    t->min_filter = GL_NEAREST_MIPMAP_LINEAR; t->mag_filter = GL_LINEAR;
    t->wrap_s = t->wrap_t = GL_REPEAT;
}
static gl2_texture_obj *bound(gl2_ctx *c, uint32_t target) {
    if (target != GL_TEXTURE_2D) { terr(c, GL_INVALID_ENUM); return 0; }
    gl2_texture_obj *t = named(c, c->texture_binding[c->active_texture]);
    if (!t || !t->used) { terr(c, GL_INVALID_OPERATION); return 0; }
    return t;
}
void gl2_texture_arena(gl2_ctx *c, void *cpu, uint32_t gpuva, uint32_t bytes, gl2_texture_sync_fn sync, void *user) {
    if (!cpu || ((uintptr_t)cpu & 3u) || bytes < GL2_TEXTURE_ARENA_BYTES || (gpuva & 127u) ||
        gpuva > 0x02000000u - GL2_TEXTURE_ARENA_BYTES ||
        (uintptr_t)cpu > UINTPTR_MAX - GL2_TEXTURE_ARENA_BYTES) { terr(c, GL_INVALID_VALUE); return; }
    for (uint32_t i = 0; i < GL2_MAX_TEXTURES; ++i) if (c->textures[i].defined) { terr(c, GL_INVALID_OPERATION); return; }
    c->texture_cpu = cpu; c->texture_va = gpuva;
    c->texture_sync = sync; c->texture_sync_user = user;
    gl2_invalidate_state_cache(c);
}
void glGenTextures(gl2_ctx *c, int n, uint32_t *names) {
    if (n < 0 || (n && !names)) { terr(c, GL_INVALID_VALUE); return; }
    uint32_t free_slots = 0;
    for (uint32_t i = 0; i < GL2_MAX_TEXTURES; ++i) free_slots += !c->textures[i].reserved;
    if ((uint32_t)n > free_slots) { terr(c, GL_OUT_OF_MEMORY); return; }
    for (int k = 0; k < n; ++k) {
        while (!c->next_texture || named(c, c->next_texture)) ++c->next_texture;
        for (uint32_t i = 0; i < GL2_MAX_TEXTURES; ++i) if (!c->textures[i].reserved) {
            reserve(&c->textures[i], c->next_texture++); names[k] = c->textures[i].name; break;
        }
    }
}
void glActiveTexture(gl2_ctx *c, uint32_t texture) {
    if (texture != GL_TEXTURE0 && texture != GL_TEXTURE1) { terr(c, GL_INVALID_ENUM); return; }
    c->active_texture = texture - GL_TEXTURE0;
    gl2_invalidate_state_cache(c);
}
void glBindTexture(gl2_ctx *c, uint32_t target, uint32_t name) {
    if (target != GL_TEXTURE_2D) { terr(c, GL_INVALID_ENUM); return; }
    if (name) {
        gl2_texture_obj *t = named(c, name);
        if (!t) for (uint32_t i = 0; i < GL2_MAX_TEXTURES; ++i) if (!c->textures[i].reserved) {
            t = &c->textures[i]; reserve(t, name); break;
        }
        if (!t) { terr(c, GL_OUT_OF_MEMORY); return; }
        t->used = 1;
    }
    c->texture_binding[c->active_texture] = name; c->texture_managed[c->active_texture] = 1;
    gl2_invalidate_state_cache(c);
}
void glDeleteTextures(gl2_ctx *c, int n, const uint32_t *names) {
    if (n < 0 || (n && !names)) { terr(c, GL_INVALID_VALUE); return; }
    for (int k = 0; k < n; ++k) {
        gl2_texture_obj *t = named(c, names[k]); if (!t) continue;
        for (uint32_t u = 0; u < 2; ++u) if (c->texture_binding[u] == t->name) {
            c->texture_binding[u] = 0; c->texture_managed[u] = 1;
        }
        t->reserved = t->used = t->defined = 0;
    }
    gl2_invalidate_state_cache(c);
}
int glIsTexture(gl2_ctx *c, uint32_t name) { gl2_texture_obj *t = named(c, name); return t && t->used; }
static uint32_t *parameter(gl2_texture_obj *t, uint32_t pname) {
    if (pname == GL_TEXTURE_MIN_FILTER) return &t->min_filter;
    if (pname == GL_TEXTURE_MAG_FILTER) return &t->mag_filter;
    if (pname == GL_TEXTURE_WRAP_S) return &t->wrap_s;
    if (pname == GL_TEXTURE_WRAP_T) return &t->wrap_t;
    return 0;
}
void glTexParameteri(gl2_ctx *c, uint32_t target, uint32_t pname, int value) {
    gl2_texture_obj *t = bound(c, target); if (!t) return;
    uint32_t *p = parameter(t, pname); if (!p) { terr(c, GL_INVALID_ENUM); return; }
    int valid;
    if (pname == GL_TEXTURE_MIN_FILTER) valid = value == GL_NEAREST || value == GL_LINEAR ||
        (value >= GL_NEAREST_MIPMAP_NEAREST && value <= GL_LINEAR_MIPMAP_LINEAR);
    else if (pname == GL_TEXTURE_MAG_FILTER) valid = value == GL_NEAREST || value == GL_LINEAR;
    else valid = value == GL_REPEAT || value == GL_CLAMP_TO_EDGE || value == GL_MIRRORED_REPEAT;
    if (!valid) { terr(c, GL_INVALID_ENUM); return; }
    uint32_t supported;
    if (pname == GL_TEXTURE_MIN_FILTER) supported = (uint32_t)value;
    else if (pname == GL_TEXTURE_MAG_FILTER) supported = (value == GL_LINEAR) ? GL_LINEAR : GL_NEAREST;
    else supported = (value == GL_CLAMP_TO_EDGE) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
    if ((uint32_t)value != supported) { terr(c, GL_INVALID_OPERATION); return; }
    *p = (uint32_t)value;
    gl2_invalidate_state_cache(c);
}
int glGetTexParameteriv(gl2_ctx *c, uint32_t target, uint32_t pname) {
    gl2_texture_obj *t = bound(c, target); if (!t) return 0;
    uint32_t *p = parameter(t, pname); if (!p) { terr(c, GL_INVALID_ENUM); return 0; }
    return (int)*p;
}
void glPixelStorei(gl2_ctx *c, uint32_t pname, int value) {
    if (pname != GL_UNPACK_ALIGNMENT && pname != GL_PACK_ALIGNMENT) { terr(c, GL_INVALID_ENUM); return; }
    if (value != 1 && value != 2 && value != 4 && value != 8) { terr(c, GL_INVALID_VALUE); return; }
    if (pname == GL_UNPACK_ALIGNMENT) c->unpack_alignment = (uint32_t)value;
    else c->pack_alignment = (uint32_t)value;
}
/* Six coordinate bits suffice for this arena. Spread once per row; increment
 * the interleaved x lane by carry propagation instead of a bit loop per texel. */
static uint32_t spread(uint32_t v) {
    v = (v | (v << 4)) & 0x0f0fu;
    v = (v | (v << 2)) & 0x3333u;
    return (v | (v << 1)) & 0x5555u;
}
static uint32_t slot_offset(gl2_ctx *c, const gl2_texture_obj *t) { return (uint32_t)(t - c->textures) * GL2_TEXTURE_SLOT_BYTES; }
static uint32_t storage_bytes(uint32_t storage);
static uint32_t level_size(const gl2_texture_obj *t,uint32_t level){return t->size>>level;}
static uint32_t level_bytes(const gl2_texture_obj *t,uint32_t level){
    uint32_t s=level_size(t,level);
    if(t->storage==GL2_TEX_PVRTC4_RGB||t->storage==GL2_TEX_PVRTC4_RGBA){if(s<8u)s=8u;return (s*s)>>1;}
    return s*s*storage_bytes(t->storage);
}
static uint32_t level_offset(const gl2_texture_obj *t,uint32_t level){uint32_t off=0;for(uint32_t l=0;l<level;++l)off+=level_bytes(t,l);return off;}
static uint32_t defined_bytes(const gl2_texture_obj *t){uint32_t bytes=0;for(uint32_t l=0;l<=t->log2size;++l)if(t->level_mask&(1u<<l))bytes=level_offset(t,l)+level_bytes(t,l);if(!bytes)bytes=level_bytes(t,0);return bytes;}
/* Source buffers may alias texture storage. Stage only overlapping uploads;
 * normal uploads have no temporary copy. API/compiler scratch is serialized. */
static uint8_t alias_source[GL2_TEXTURE_SLOT_BYTES];
static const void *stage_alias(gl2_ctx *c, gl2_texture_obj *t, uint32_t base, const void *pixels, uint32_t bytes, uint32_t output_bytes) {
    (void)t;
    uintptr_t src = (uintptr_t)pixels, dst = (uintptr_t)(c->texture_cpu + base);
    if (pixels && src < dst + output_bytes && dst < src + bytes) {
        const uint8_t *p = pixels;
        for (uint32_t i=0;i<bytes;++i) alias_source[i]=p[i];
        return alias_source;
    }
    return pixels;
}
typedef uint32_t texture_word __attribute__((__may_alias__));
static uint32_t packed16(const uint8_t *p) { return p ? (uint32_t)p[0] | (uint32_t)p[1]<<8 : 0; }
/* Correct nearest conversion for normalized 5/6-bit channels, no division call. */
static uint32_t unorm5(uint32_t v) { return (v * 527u + 23u) >> 6; }
static uint32_t unorm6(uint32_t v) { return (v * 259u + 33u) >> 6; }
static uint32_t storage_for(uint32_t format,uint32_t type) {
    if (type==GL_UNSIGNED_SHORT_5_6_5) return GL2_TEX_RGB565;
    if (type==GL_UNSIGNED_SHORT_4_4_4_4) return GL2_TEX_ARGB4444;
    if (type==GL_UNSIGNED_SHORT_5_5_5_1) return GL2_TEX_ARGB1555;
    return format==GL_LUMINANCE_ALPHA ? GL2_TEX_AL88 : GL2_TEX_ARGB8888;
}
static uint32_t storage_bytes(uint32_t storage) { return storage==GL2_TEX_ARGB8888 ? 4u : 2u; }
static uint32_t rgba_pixel(const uint8_t *p,uint32_t format,uint32_t type) {
    uint32_t v=packed16(p);
    if (type==GL_UNSIGNED_SHORT_5_6_5) return 0xff000000u|unorm5(v>>11)<<16|unorm6((v>>5)&63)<<8|unorm5(v&31);
    if (type==GL_UNSIGNED_SHORT_4_4_4_4) return (v&15)*17<<24|(v>>12)*17<<16|((v>>8)&15)*17<<8|((v>>4)&15)*17;
    if (type==GL_UNSIGNED_SHORT_5_5_5_1) return (0u-(v&1))<<24|unorm5(v>>11)<<16|unorm5((v>>6)&31)<<8|unorm5((v>>1)&31);
    if (format==GL_RGB) return 0xff000000u|(p?(uint32_t)p[0]<<16|(uint32_t)p[1]<<8|p[2]:0);
    return p?(uint32_t)p[3]<<24|(uint32_t)p[0]<<16|(uint32_t)p[1]<<8|p[2]:0;
}
static uint32_t native_pixel(uint32_t rgba,uint32_t storage) {
    uint32_t r=rgba>>16&255,g=rgba>>8&255,b=rgba&255,a=rgba>>24;
    if (storage==GL2_TEX_RGB565) return ((r*31+127)/255)<<11|((g*63+127)/255)<<5|(b*31+127)/255;
    if (storage==GL2_TEX_ARGB4444) return ((a*15+127)/255)<<12|((r*15+127)/255)<<8|((g*15+127)/255)<<4|(b*15+127)/255;
    if (storage==GL2_TEX_AL88) return (a<<8)|((r*77+g*150+b*29+128)>>8);
    return (a>=128u)<<15|((r*31+127)/255)<<10|((g*31+127)/255)<<5|(b*31+127)/255;
}
static uint32_t native_to_rgba(uint32_t v,uint32_t storage) {
    if (storage==GL2_TEX_RGB565) { uint32_t r=(v>>11)*255/31,g=((v>>5)&63)*255/63,b=(v&31)*255/31; return 0xff000000u|(r<<16)|(g<<8)|b; }
    if (storage==GL2_TEX_ARGB4444) { uint32_t a=(v>>12)*17,r=(v>>8&15)*17,g=(v>>4&15)*17,b=(v&15)*17; return (a<<24)|(r<<16)|(g<<8)|b; }
    if (storage==GL2_TEX_AL88) { uint32_t l=v&255; return ((v>>8)<<24)|(l<<16)|(l<<8)|l; }
    return ((v>>15)?0xff000000u:0)|((v>>10&31)*255/31<<16)|(((v>>5)&31)*255/31<<8)|((v&31)*255/31);
}
typedef uint16_t texture_half __attribute__((__may_alias__));
/* Aligned full tiles: two source word loads and two destination word stores
 * move four native texels. Format reordering operates on both halfwords at once. */
static void upload16_tiles(gl2_ctx *c,gl2_texture_obj *t,uint32_t base,uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t pitch,const void *pixels){
 texture_word *dst=(texture_word *)(c->texture_cpu+base);const uint8_t *src=pixels;uint32_t first_x=spread(x);
#define TILES(expression) do { \
 for(uint32_t j=0;j<h;j+=2){const texture_word *r0=(const texture_word *)(src+j*pitch),*r1=(const texture_word *)(src+(j+1)*pitch);uint32_t mx=first_x,my=spread(y+j)>>1; \
  for(uint32_t i=0;i<w/2;++i){uint32_t v=r0[i],a=(expression);v=r1[i];uint32_t b=(expression),index=mx|my; \
   dst[index]=(a&65535u)|(b<<16);dst[index+1]=(a>>16)|(b&0xffff0000u);mx=(mx-0x5554u)&0x5554u; \
  } \
 } \
}while(0)
 if(t->storage==GL2_TEX_ARGB4444){TILES((v>>4&0x0fff0fffu)|(v<<12&0xf000f000u));}
 else if(t->storage==GL2_TEX_ARGB1555){TILES((v>>1&0x7fff7fffu)|(v<<15&0x80008000u));}
 else{TILES(v);}
#undef TILES
}
static void upload16(gl2_ctx *c,gl2_texture_obj *t,uint32_t base,uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t format,uint32_t type,uint32_t bpp,const void *pixels) {
    uint8_t *basep=c->texture_cpu+base;
    texture_half *dst=(texture_half *)basep;texture_word *words=(texture_word *)basep;
    const uint8_t *src=pixels;
    uint32_t pitch=(w*bpp+c->unpack_alignment-1)&~(c->unpack_alignment-1),first_x=spread(x);
    int direct=(t->storage==GL2_TEX_RGB565 && type==GL_UNSIGNED_SHORT_5_6_5) ||
        (t->storage==GL2_TEX_ARGB4444 && type==GL_UNSIGNED_SHORT_4_4_4_4) ||
        (t->storage==GL2_TEX_ARGB1555 && type==GL_UNSIGNED_SHORT_5_5_5_1) || t->storage==GL2_TEX_AL88;
    if(direct && src && !((uintptr_t)src&3u) && !(pitch&3u) && !((x|y|w|h)&1u)) {
        upload16_tiles(c,t,base,x,y,w,h,pitch,src);return;
    }
    /* Adjacent Morton halfwords differ in y, not x. Pair source rows and emit
     * full bus words; isolated top/bottom rows retain halfword writes. */
#define HALF_ROW(expression) do { \
    const uint8_t *row=src?src+j*pitch:0;uint32_t mx=first_x<<1,my=spread(y+j); \
    for(uint32_t i=0;i<w;++i) { const uint8_t *p=row?row+i*bpp:0;uint32_t v=packed16(p);(void)v; \
        dst[mx|my]=(uint16_t)(expression);mx=(mx-0xaaaau)&0xaaaau; } \
} while(0)
#define WRITE16(expression) do { \
    uint32_t j=0; \
    if((y&1u) && h) { HALF_ROW(expression);++j; } \
    for(;j+1<h;j+=2) { \
        const uint8_t *r0=src?src+j*pitch:0,*r1=r0?r0+pitch:0; \
        uint32_t mx=first_x,my=spread(y+j)>>1; \
        for(uint32_t i=0;i<w;++i) { \
            const uint8_t *p=r0?r0+i*bpp:0;uint32_t v=packed16(p);(void)v; \
            uint32_t lo=(expression);p=r1?r1+i*bpp:0;v=packed16(p); \
            uint32_t hi=(expression);words[mx|my]=lo|(hi<<16);mx=(mx-0x5555u)&0x5555u; \
        } \
    } \
    if(j<h) { HALF_ROW(expression); } \
} while(0)
    if ((t->storage==GL2_TEX_RGB565 && type==GL_UNSIGNED_SHORT_5_6_5) || t->storage==GL2_TEX_AL88) { WRITE16(v); }
    else if(t->storage==GL2_TEX_ARGB4444 && type==GL_UNSIGNED_SHORT_4_4_4_4) { WRITE16((v>>4)|(v&15)<<12); }
    else if(t->storage==GL2_TEX_ARGB1555 && type==GL_UNSIGNED_SHORT_5_5_5_1) { WRITE16((v>>1)|(v&1)<<15); }
    else { WRITE16(native_pixel(rgba_pixel(p,format,type),t->storage)); }
#undef WRITE16
#undef HALF_ROW
}
static void upload(gl2_ctx *c, gl2_texture_obj *t, uint32_t base, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t format, uint32_t type, uint32_t bpp, const void *pixels) {
    if (t->storage!=GL2_TEX_ARGB8888) { upload16(c,t,base,x,y,w,h,format,type,bpp,pixels);return; }
    texture_word *dst = (texture_word *)(c->texture_cpu + base);
    const uint8_t *src = pixels;
    uint32_t pitch = (w*bpp + c->unpack_alignment-1) & ~(c->unpack_alignment-1);
    uint32_t first_x = spread(x)<<1;
    if (format==GL_RGBA && type==GL_UNSIGNED_BYTE && src && !((uintptr_t)src&3u)) {
        for (uint32_t j=0;j<h;++j) {
            uint32_t mx=first_x, my=spread(y+j);
            const texture_word *row=(const texture_word *)(src+j*pitch);
            for (uint32_t i=0;i<w;++i) {
                uint32_t v=row[i];
                dst[mx|my]=(v&0xff00ff00u)|((v&255u)<<16)|((v>>16)&255u);
                mx=(mx-0xaaaau)&0xaaaau;
            }
        }
        return;
    }
    /* Each format selects a specialized loop once. No per-pixel format branch,
     * function pointer call or generic intermediate RGBA conversion buffer. */
#define WRITE_LOOP(expression) do { \
    for (uint32_t j=0;j<h;++j) { \
        uint32_t mx=first_x, my=spread(y+j); \
        const uint8_t *row=src ? src+j*pitch : 0; \
        for (uint32_t i=0;i<w;++i) { \
            const uint8_t *p=row ? row+i*bpp : 0; \
            uint32_t v=type==GL_UNSIGNED_BYTE ? 0 : packed16(p); (void)v; \
            dst[mx|my]=(expression); \
            mx=(mx-0xaaaau)&0xaaaau; \
        } \
    } \
} while (0)
    if (type==GL_UNSIGNED_SHORT_5_6_5) {
        WRITE_LOOP(0xff000000u | unorm5(v>>11)<<16 | unorm6((v>>5)&63u)<<8 | unorm5(v&31u));
    } else if (type==GL_UNSIGNED_SHORT_4_4_4_4) {
        WRITE_LOOP(((v&15u)*17u)<<24 | ((v>>12)*17u)<<16 | (((v>>8)&15u)*17u)<<8 | ((v>>4)&15u)*17u);
    } else if (type==GL_UNSIGNED_SHORT_5_5_5_1) {
        WRITE_LOOP((0u-(v&1u))<<24 | unorm5(v>>11)<<16 | unorm5((v>>6)&31u)<<8 | unorm5((v>>1)&31u));
    } else switch (format) {
    case GL_ALPHA: WRITE_LOOP(0x00ffffffu | (uint32_t)(p?p[0]:0)<<24); break;
    case GL_LUMINANCE: WRITE_LOOP(0xff000000u | (uint32_t)(p?p[0]:0)*0x010101u); break;
    case GL_LUMINANCE_ALPHA: WRITE_LOOP((uint32_t)(p?p[1]:0)<<24 | (uint32_t)(p?p[0]:0)*0x010101u); break;
    case GL_RGB: WRITE_LOOP(0xff000000u | (p?((uint32_t)p[0]<<16 | (uint32_t)p[1]<<8 | p[2]):0)); break;
    default: WRITE_LOOP(p?((uint32_t)p[3]<<24 | (uint32_t)p[0]<<16 | (uint32_t)p[1]<<8 | p[2]):0); break;
    }
#undef WRITE_LOOP
}
static int sync_image(gl2_ctx *c, gl2_texture_obj *t) {
    uint32_t off = slot_offset(c, t);
    if (c->texture_sync && c->texture_sync(c->texture_sync_user, c->texture_cpu + off, c->texture_va + off, defined_bytes(t))) {
        t->defined = 0; terr(c, GL_INVALID_OPERATION); return -1;
    }
    return 0;
}
static int valid_format(uint32_t format) {
    return format==GL_ALPHA || format==GL_RGB || format==GL_RGBA || format==GL_LUMINANCE || format==GL_LUMINANCE_ALPHA;
}
static uint32_t image_format(gl2_ctx *c, int level, uint32_t format, uint32_t type) {
    if (level<0 || (1u<<(unsigned)level)>GL2_MAX_TEXTURE_SIZE) { terr(c, GL_INVALID_VALUE); return 0; }
    if (!valid_format(format)) { terr(c, GL_INVALID_ENUM); return 0; }
    if (type==GL_UNSIGNED_BYTE) return format==GL_RGBA ? 4u : format==GL_RGB ? 3u : format==GL_LUMINANCE_ALPHA ? 2u : 1u;
    if (type!=GL_UNSIGNED_SHORT_5_6_5 && type!=GL_UNSIGNED_SHORT_4_4_4_4 && type!=GL_UNSIGNED_SHORT_5_5_5_1) { terr(c, GL_INVALID_ENUM); return 0; }
    if ((type==GL_UNSIGNED_SHORT_5_6_5 && format!=GL_RGB) || (type!=GL_UNSIGNED_SHORT_5_6_5 && format!=GL_RGBA)) { terr(c, GL_INVALID_OPERATION); return 0; }
    return 2;
}
static int source_range(gl2_ctx *c, uint32_t w, uint32_t h, uint32_t bpp, const void *pixels, uint32_t *bytes) {
    uint32_t pitch=(w*bpp+c->unpack_alignment-1)&~(c->unpack_alignment-1);
    *bytes=h ? (h-1)*pitch+w*bpp : 0;
    if (pixels && (uintptr_t)pixels>UINTPTR_MAX-*bytes) { terr(c, GL_INVALID_VALUE); return -1; }
    return 0;
}
void glTexImage2D(gl2_ctx *c, uint32_t target, int level, int internalformat, int w, int h, int border, uint32_t format, uint32_t type, const void *pixels) {
    gl2_texture_obj *t=bound(c,target); if (!t) return;
    if (!valid_format((uint32_t)internalformat)) { terr(c, GL_INVALID_VALUE); return; }
    uint32_t bpp=image_format(c,level,format,type); if (!bpp) return;
    if ((uint32_t)internalformat!=format) { terr(c, GL_INVALID_OPERATION); return; }
    if (border || !c->texture_cpu) { terr(c, border ? GL_INVALID_VALUE : GL_INVALID_OPERATION); return; }
    uint32_t bytes;
    if (source_range(c,(uint32_t)w,(uint32_t)h,bpp,pixels,&bytes)) return;
    if (c->texture_write_guard && c->texture_write_guard(c->texture_write_guard_user)) { terr(c, GL_INVALID_OPERATION); return; }
    if (level == 0) {
        if (w<8 || w>GL2_MAX_TEXTURE_SIZE || w!=h || (w&(w-1))) { terr(c, GL_INVALID_VALUE); return; }
        gl2_invalidate_state_cache(c);
        uint32_t storage=storage_for(format,type),base=slot_offset(c,t);
        pixels=stage_alias(c,t,base,pixels,bytes,(uint32_t)w*(uint32_t)h*storage_bytes(storage));
        t->size=(uint32_t)w; t->log2size=0; t->format=format; t->storage=storage; t->level_mask=1; t->mip_complete=0;
        while ((1u<<t->log2size)<t->size) ++t->log2size;
        t->defined=0;
        upload(c,t,base,0,0,(uint32_t)w,(uint32_t)h,format,type,bpp,pixels);
        if (!sync_image(c,t)) t->defined=1;
        return;
    }
    if (!t->defined || level > (int)t->log2size) { terr(c, GL_INVALID_VALUE); return; }
    uint32_t expected=t->size>>level;
    if (!expected || w != (int)expected || h != (int)expected) { terr(c, GL_INVALID_VALUE); return; }
    if (storage_for(format,type) != t->storage) { terr(c, GL_INVALID_OPERATION); return; }
    gl2_invalidate_state_cache(c);
    uint32_t base=slot_offset(c,t)+level_offset(t,(uint32_t)level);
    pixels=stage_alias(c,t,base,pixels,bytes,level_bytes(t,(uint32_t)level));
    upload(c,t,base,0,0,(uint32_t)w,(uint32_t)h,format,type,bpp,pixels);
    t->level_mask |= 1u<<level;
    uint32_t all=(1u<<(t->log2size+1))-1u;
    t->mip_complete=(t->level_mask&all)==all;
    (void)sync_image(c,t);
}
void glCompressedTexImage2D(gl2_ctx *c, uint32_t target, int level, uint32_t internalformat, int w, int h, int border, int imageSize, const void *data) {
    gl2_texture_obj *t=bound(c,target); if (!t) return;
    if (internalformat!=GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG && internalformat!=GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG) { terr(c, GL_INVALID_ENUM); return; }
    if (level<0 || w<1 || h<1 || border || imageSize<0 || !data) { terr(c, (!data||imageSize<0)?GL_INVALID_OPERATION:GL_INVALID_VALUE); return; }
    if (!c->texture_cpu || (c->texture_write_guard && c->texture_write_guard(c->texture_write_guard_user))) { terr(c, GL_INVALID_OPERATION); return; }
    uint32_t storage = (internalformat==GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG) ? GL2_TEX_PVRTC4_RGB : GL2_TEX_PVRTC4_RGBA;
    uint32_t bw=(uint32_t)w<8u?8u:(uint32_t)w, bh=(uint32_t)h<8u?8u:(uint32_t)h;
    uint32_t expected_bytes=(bw*bh)>>1;
    if ((uint32_t)imageSize != expected_bytes) { terr(c, GL_INVALID_VALUE); return; }
    if (level == 0) {
        if (w<8 || w>GL2_MAX_TEXTURE_SIZE || w!=h || (w&(w-1))) { terr(c, GL_INVALID_VALUE); return; }
        gl2_invalidate_state_cache(c);
        uint32_t base=slot_offset(c,t);
        data=stage_alias(c,t,base,data,expected_bytes,expected_bytes);
        t->size=(uint32_t)w; t->log2size=0; t->format=internalformat; t->storage=storage; t->level_mask=1; t->mip_complete=0;
        while ((1u<<t->log2size)<t->size) ++t->log2size;
        t->defined=0;
        uint8_t *dst=c->texture_cpu+base; const uint8_t *src=(const uint8_t *)data;
        for (uint32_t i=0;i<expected_bytes;++i) dst[i]=src[i];
        if (!sync_image(c,t)) t->defined=1;
        return;
    }
    if (!t->defined || level > (int)t->log2size) { terr(c, GL_INVALID_VALUE); return; }
    uint32_t expected=t->size>>level;
    if (!expected || w!=(int)expected || h!=(int)expected) { terr(c, GL_INVALID_VALUE); return; }
    if (storage != t->storage) { terr(c, GL_INVALID_OPERATION); return; }
    gl2_invalidate_state_cache(c);
    uint32_t base=slot_offset(c,t)+level_offset(t,(uint32_t)level);
    data=stage_alias(c,t,base,data,expected_bytes,expected_bytes);
    uint8_t *dst=c->texture_cpu+base; const uint8_t *src=(const uint8_t *)data;
    for (uint32_t i=0;i<expected_bytes;++i) dst[i]=src[i];
    t->level_mask |= 1u<<level;
    uint32_t all=(1u<<(t->log2size+1))-1u;
    t->mip_complete=(t->level_mask&all)==all;
    (void)sync_image(c,t);
}
void glTexSubImage2D(gl2_ctx *c, uint32_t target, int level, int x, int y, int w, int h, uint32_t format, uint32_t type, const void *pixels) {
    gl2_texture_obj *t=bound(c,target); if (!t) return;
    uint32_t bpp=image_format(c,level,format,type); if (!bpp) return;
    if (!t->defined || !c->texture_cpu || format!=t->format || level<0 || level>(int)t->log2size || !(t->level_mask&(1u<<level))) { terr(c, GL_INVALID_OPERATION); return; }
    uint32_t lsize=t->size>>level;
    if (x<0 || y<0 || w<0 || h<0 || (uint32_t)x>lsize || (uint32_t)y>lsize ||
        (uint32_t)w>lsize-(uint32_t)x || (uint32_t)h>lsize-(uint32_t)y || (w && h && !pixels)) { terr(c, GL_INVALID_VALUE); return; }
    if (!w || !h) return;
    uint32_t bytes;
    if (source_range(c,(uint32_t)w,(uint32_t)h,bpp,pixels,&bytes)) return;
    if (c->texture_write_guard && c->texture_write_guard(c->texture_write_guard_user)) { terr(c, GL_INVALID_OPERATION); return; }
    gl2_invalidate_state_cache(c);
    uint32_t base=slot_offset(c,t)+level_offset(t,(uint32_t)level);
    pixels=stage_alias(c,t,base,pixels,bytes,level_bytes(t,(uint32_t)level));
    upload(c,t,base,(uint32_t)x,(uint32_t)y,(uint32_t)w,(uint32_t)h,format,type,bpp,pixels);
    (void)sync_image(c,t);
}
void glGenerateMipmap(gl2_ctx *c, uint32_t target) {
    gl2_texture_obj *t=bound(c,target); if (!t) return;
    if (!t->defined || (t->storage&(GL2_TEX_PVRTC4_RGB|GL2_TEX_PVRTC4_RGBA))) { terr(c, GL_INVALID_OPERATION); return; }
    if (c->texture_write_guard && c->texture_write_guard(c->texture_write_guard_user)) { terr(c, GL_INVALID_OPERATION); return; }
    gl2_invalidate_state_cache(c); gl2_soft_cache_flush();
    uint32_t slot=slot_offset(c,t);
    for (uint32_t level=1; level<=t->log2size; ++level) {
        uint32_t dstn=t->size>>level;
        uint32_t src=slot+level_offset(t,level-1), dst=slot+level_offset(t,level);
        for (uint32_t y=0;y<dstn;++y) for (uint32_t x=0;x<dstn;++x) {
            uint32_t rgba[4]; uint32_t sum[4]={0,0,0,0};
            for (uint32_t sy=0;sy<2;++sy) for (uint32_t sx=0;sx<2;++sx) {
                uint32_t ix=(spread(x*2+sx)<<1)|spread(y*2+sy);
                uint32_t v;
                if (t->storage==GL2_TEX_ARGB8888) v=((const uint32_t *)(c->texture_cpu+src))[ix];
                else v=((const texture_half *)(c->texture_cpu+src))[ix];
                rgba[0]=t->storage==GL2_TEX_ARGB8888?v:native_to_rgba(v,t->storage);
                rgba[1]=rgba[2]=rgba[3]=0;
                sum[0]+=rgba[0]>>16&255; sum[1]+=rgba[0]>>8&255; sum[2]+=rgba[0]&255; sum[3]+=rgba[0]>>24;
            }
            uint32_t r=(sum[0]+2)>>2,g=(sum[1]+2)>>2,b=(sum[2]+2)>>2,a=(sum[3]+2)>>2;
            uint32_t oix=(spread(x)<<1)|spread(y);
            if (t->storage==GL2_TEX_ARGB8888) ((uint32_t *)(c->texture_cpu+dst))[oix]=(a<<24)|(r<<16)|(g<<8)|b;
            else ((texture_half *)(c->texture_cpu+dst))[oix]=(uint16_t)native_pixel((a<<24)|(r<<16)|(g<<8)|b,t->storage);
        }
        t->level_mask |= 1u<<level;
    }
    t->mip_complete=1;
    (void)sync_image(c,t);
}
int gl2_resolve_texture_units(gl2_ctx *c, uint32_t layers, uint32_t samplers, const uint8_t units[2]) {
    if (!samplers) return 0;
    if (layers > 2 || samplers > 2 || !units) { terr(c, GL_INVALID_OPERATION); return -1; }
    gl2_tex resolved[2] = {c->tex[0], c->tex[1]};
    for (uint32_t layer = 0; layer < layers; ++layer) {
        uint32_t unit = units[samplers == 1 ? 0 : layer];
        if (unit >= 2) { terr(c, GL_INVALID_OPERATION); return -1; }
        if (!c->texture_managed[unit]) { resolved[layer] = c->raw_unit_tex[unit]; continue; }
        gl2_texture_obj *t = named(c, c->texture_binding[unit]);
        if (!t || !t->used || !t->defined) { terr(c, GL_INVALID_OPERATION); return -1; }
        uint32_t mf=t->min_filter,gf=t->mag_filter;
        int mip = mf>=GL_NEAREST_MIPMAP_NEAREST && mf<=GL_LINEAR_MIPMAP_LINEAR;
        uint32_t linear=0,mipflag=0;
        if (mip) {
            if (!t->mip_complete) { terr(c, GL_INVALID_OPERATION); return -1; }
            int base_linear = mf==GL_LINEAR_MIPMAP_NEAREST || mf==GL_LINEAR_MIPMAP_LINEAR;
            if (gf != (base_linear ? GL_LINEAR : GL_NEAREST)) { terr(c, GL_INVALID_OPERATION); return -1; }
            linear = base_linear ? GL2_TEX_LINEAR : 0;
            mipflag = (mf==GL_NEAREST_MIPMAP_NEAREST || mf==GL_LINEAR_MIPMAP_NEAREST) ? GL2_TEX_MIP_NEAREST : GL2_TEX_MIP_LINEAR;
        } else {
            if ((mf != GL_NEAREST && mf != GL_LINEAR) || gf != mf) { terr(c, GL_INVALID_OPERATION); return -1; }
            linear = mf==GL_LINEAR ? GL2_TEX_LINEAR : 0;
        }
        if ((t->wrap_s != GL_REPEAT && t->wrap_s != GL_CLAMP_TO_EDGE) ||
            (t->wrap_t != GL_REPEAT && t->wrap_t != GL_CLAMP_TO_EDGE)) { terr(c, GL_INVALID_OPERATION); return -1; }
        resolved[layer].va = c->texture_va + slot_offset(c, t); resolved[layer].log2size = t->log2size;
        resolved[layer].storage=t->storage|linear|mipflag|
            (t->wrap_s==GL_CLAMP_TO_EDGE?GL2_TEX_CLAMP_S:0u)|(t->wrap_t==GL_CLAMP_TO_EDGE?GL2_TEX_CLAMP_T:0u);
    }
    c->tex[0] = resolved[0]; c->tex[1] = resolved[1]; return 0;
}
int gl2_resolve_textures(gl2_ctx *c, uint32_t layers, uint32_t samplers) {
    static const uint8_t implicit[2] = {0,1};
    return gl2_resolve_texture_units(c, layers, samplers, implicit);
}
/* Backend copy from a completed ARGB8888 surface, bottom-left source rectangle.
 * One pass into tiled storage, without a full-image RGBA staging buffer. */
int gl2_texture_copy_argb(gl2_ctx *c,int dx,int dy,int sx,int sy,int w,int h,const uint32_t *surface,uint32_t sw,uint32_t sh,uint32_t stride){
 gl2_texture_obj *t=bound(c,GL_TEXTURE_2D);if(!t)return -1;
 if(!t->defined||!c->texture_cpu||(t->format!=GL_RGBA&&t->format!=GL_RGB)||t->storage!=GL2_TEX_ARGB8888){terr(c,GL_INVALID_OPERATION);return -1;}
 if(dx<0||dy<0||sx<0||sy<0||w<0||h<0||(uint32_t)dx>t->size||(uint32_t)dy>t->size||(uint32_t)w>t->size-(uint32_t)dx||(uint32_t)h>t->size-(uint32_t)dy||(uint32_t)sx>sw||(uint32_t)sy>sh||(uint32_t)w>sw-(uint32_t)sx||(uint32_t)h>sh-(uint32_t)sy||stride<sw){terr(c,GL_INVALID_VALUE);return -1;}
 if(!w||!h)return 0;
 if(!surface||((uintptr_t)surface&3u)||!sh||stride>UINT32_MAX/4/sh){terr(c,GL_INVALID_VALUE);return -1;}
 uintptr_t src=(uintptr_t)surface,dst=(uintptr_t)(c->texture_cpu+slot_offset(c,t));uint32_t bytes=stride*sh*4,output=t->size*t->size*4;
 if(src>UINTPTR_MAX-bytes||(src<dst+output&&dst<src+bytes)){terr(c,GL_INVALID_OPERATION);return -1;}
 if(c->texture_write_guard&&c->texture_write_guard(c->texture_write_guard_user)){terr(c,GL_INVALID_OPERATION);return -1;}
 gl2_invalidate_state_cache(c);
 texture_word *out=(texture_word *)dst;uint32_t alpha=t->format==GL_RGB?0xff000000u:0;
 if(!((dx|dy|w|h)&1)){
  /* Four contiguous destination words per2x2 tile; reverse source rows while
   * loading. Padded source pitch does not require a staging image. */
  uint32_t first=spread((uint32_t)dx)<<1;
  for(uint32_t y=0;y<(uint32_t)h;y+=2){const uint32_t *r0=surface+(sh-1-(uint32_t)sy-y)*stride+(uint32_t)sx,*r1=r0-stride;uint32_t mx=first,my=spread((uint32_t)dy+y);
   for(uint32_t x=0;x<(uint32_t)w;x+=2){uint32_t index=mx|my;out[index]=r0[x]|alpha;out[index+1]=r1[x]|alpha;out[index+2]=r0[x+1]|alpha;out[index+3]=r1[x+1]|alpha;mx=(mx-0xaaa8u)&0xaaa8u;}
  }
 }else for(uint32_t y=0;y<(uint32_t)h;++y){const uint32_t *row=surface+(sh-1-(uint32_t)sy-y)*stride+(uint32_t)sx;uint32_t mx=spread((uint32_t)dx)<<1,my=spread((uint32_t)dy+y);
  for(uint32_t x=0;x<(uint32_t)w;++x){out[mx|my]=row[x]|alpha;mx=(mx-0xaaaau)&0xaaaau;}
 }
 return sync_image(c,t);
}
/* Store a linear ARGB8888 surface (top-left origin) into a texture object,
 * reversing rows to the texture's bottom-left convention. Used by FBO
 * completion; the destination texture need not be bound. */
int gl2_texture_store_argb(gl2_ctx *c, gl2_texture_obj *t, int dx, int dy, int w, int h, const uint32_t *surface, uint32_t sw, uint32_t sh, uint32_t stride) {
    if (!t || !t->defined || !c->texture_cpu || (t->format != GL_RGBA && t->format != GL_RGB) || t->storage != GL2_TEX_ARGB8888) { terr(c, GL_INVALID_OPERATION); return -1; }
    if (dx < 0 || dy < 0 || w < 0 || h < 0 || (uint32_t)dx > t->size || (uint32_t)dy > t->size ||
        (uint32_t)w > t->size - (uint32_t)dx || (uint32_t)h > t->size - (uint32_t)dy ||
        !sw || !sh || (uint32_t)w > sw || (uint32_t)h > sh || stride < sw || stride > UINT32_MAX / 4 / sh) { terr(c, GL_INVALID_VALUE); return -1; }
    if (!w || !h) return 0;
    if (!surface || ((uintptr_t)surface & 3u)) { terr(c, GL_INVALID_VALUE); return -1; }
    uintptr_t src = (uintptr_t)surface, dst = (uintptr_t)(c->texture_cpu + slot_offset(c, t));
    uint32_t bytes = stride * sh * 4, output = t->size * t->size * 4;
    if (src > UINTPTR_MAX - bytes || (src < dst + output && dst < src + bytes)) { terr(c, GL_INVALID_OPERATION); return -1; }
    if (c->texture_write_guard && c->texture_write_guard(c->texture_write_guard_user)) { terr(c, GL_INVALID_OPERATION); return -1; }
    gl2_invalidate_state_cache(c);
    texture_word *out = (texture_word *)dst; uint32_t alpha = t->format == GL_RGB ? 0xff000000u : 0;
    if (!((dx | dy | w | h) & 1)) {
        uint32_t first = spread((uint32_t)dx) << 1;
        for (uint32_t y = 0; y < (uint32_t)h; y += 2) {
            const uint32_t *r0 = surface + (sh - 1 - y) * stride, *r1 = r0 - stride;
            uint32_t mx = first, my = spread((uint32_t)dy + y);
            for (uint32_t x = 0; x < (uint32_t)w; x += 2) {
                uint32_t index = mx | my;
                out[index] = r0[x] | alpha; out[index + 1] = r1[x] | alpha;
                out[index + 2] = r0[x + 1] | alpha; out[index + 3] = r1[x + 1] | alpha;
                mx = (mx - 0xaaa8u) & 0xaaa8u;
            }
        }
    } else for (uint32_t y = 0; y < (uint32_t)h; ++y) {
        const uint32_t *row = surface + (sh - 1 - y) * stride;
        uint32_t mx = spread((uint32_t)dx) << 1, my = spread((uint32_t)dy + y);
        for (uint32_t x = 0; x < (uint32_t)w; ++x) { out[mx | my] = row[x] | alpha; mx = (mx - 0xaaaau) & 0xaaaau; }
    }
    return sync_image(c, t);
}
