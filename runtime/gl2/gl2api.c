#include "gl2api.h"
static int scmp(const char *a, const char *b) { while (*a && *a == *b) { ++a; ++b; } return *a - *b; }
static void scpy(char *d, const char *s, int n) { int i = 0; for (; i < n - 1 && s[i]; ++i) d[i] = s[i]; d[i] = 0; }
uint32_t gl2_hash(const char *s) { uint32_t h = 2166136261u; while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; } return h; }
#include "gl2fsc.inc"
static void fs_copy(gl2_fs_owned *dst,const gl2_fs_entry *src){
 dst->e=*src;if(src->software){dst->software=*src->software;dst->e.software=&dst->software;}for(unsigned i=0;i<src->nsampler;++i){scpy(dst->samplers[i],src->sampler_names[i],32);dst->e.sampler_names[i]=dst->samplers[i];}
 for(unsigned i=0;i<src->nuniform;++i){scpy(dst->uniforms[i],src->uniform_names[i],32);dst->e.uniform_names[i]=dst->uniforms[i];}
 for(unsigned i=0;i<src->nvary;++i){scpy(dst->varyings[i],src->vary_names[i],32);dst->e.vary_names[i]=dst->varyings[i];}
}
static void fs_rebase(gl2_prog_obj *p){p->fs=&p->fs_owned.e;if(p->fs->software)p->fs_owned.e.software=&p->fs_owned.software;for(unsigned i=0;i<p->fs->nuniform;++i)p->fs_owned.e.uniform_names[i]=p->fs_owned.uniforms[i];for(unsigned i=0;i<p->fs->nsampler;++i)p->fs_owned.e.sampler_names[i]=p->fs_owned.samplers[i];for(unsigned i=0;i<p->fs->nvary;++i)p->fs_owned.e.vary_names[i]=p->fs_owned.varyings[i];p->prog.plan=&p->fs->plan[0][0];}
static void err(gl2_ctx *c, uint32_t e) { if (!c->err) c->err = e; }
void gl2_init(gl2_ctx *c, gl2_prog_obj *progs, uint32_t *segbuf, uint32_t seg_cap, gl2_submit_fn fn, void *user) {
    uint8_t *p = (uint8_t *)c; for (uint32_t i = 0; i < sizeof *c; ++i) p[i] = 0;
    c->pr = progs; for (int i = 0; i < GL2_MAX_PROGRAMS; ++i) { progs[i].used = 0; progs[i].ok = 0; }
    c->seg = segbuf; c->seg_cap = seg_cap; c->submit = fn; c->submit_user = user;
    c->st.depth_func = 0x201; c->st.depth_write = 1; c->st.src = 1; c->st.dst = 0; c->vp_w = 240; c->vp_h = 432;
    c->cull_face = GL_BACK; c->front_face = GL_CCW; c->clear_depth = 1.f; c->color_mask = 0xfu; c->scissor_box[0]=0;c->scissor_box[1]=0;c->scissor_box[2]=240;c->scissor_box[3]=432;
    c->next_buffer = 1; c->next_texture = 1; c->unpack_alignment = 4; c->pack_alignment = 4;
    for (uint32_t i = 0; i < 5; ++i) { c->attr[i].size = 4; c->attr[i].stride = 16; c->attr[i].type = GL_FLOAT; c->attr[i].current[3] = 1; }
}
void gl2_invalidate_state_cache(gl2_ctx *c) {
    if (!c) return;
    c->cached_hdr_valid = 0; c->cached_hdr_words = 0;
}
void gl2_set_state_cache(gl2_ctx *c, int enabled) {
    if (!c) return;
    c->state_cache_enabled = enabled != 0;
    gl2_invalidate_state_cache(c);
}
static int words_equal(const uint32_t *a, const uint32_t *b, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) if (a[i] != b[i]) return 0;
    return 1;
}
static void copy_words(uint32_t *dst, const uint32_t *src, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) dst[i] = src[i];
}
static int submit_draw_packet(gl2_ctx *c, uint32_t *seg, uint32_t n, uint32_t nh, const void *parts_src, uint32_t parts_words) {
    if ((!parts_src && n < nh) || nh < 2u) return -1;
    /* Stock driver 0x0813f810 selects bits 19:18. The current viewport
     * flips window Y; CCW/BACK uses mode 1. Never send undocumented mode 3. */
    if (c->cull_enabled && c->cull_face == GL_FRONT_AND_BACK) return 0;
    uint32_t cull = 0;
    if (c->cull_enabled) cull = ((c->cull_face == GL_BACK) == (c->front_face == GL_CCW)) ? 1u : 2u;
    seg[nh - 2u] = (seg[nh - 2u] & ~0x000c0000u) | (cull << 18);
    uint32_t prefix = nh - 2u;
    int hit = c->state_cache_enabled && c->cached_hdr_valid &&
              c->cached_hdr_words == prefix && words_equal(c->cached_hdr, seg, prefix);
    const uint32_t *sub = hit ? seg + prefix : seg;
    int fail = 0;
    if (parts_src) fail = c->submit_parts(sub, hit ? 2u : nh, parts_src, parts_words, c->submit_user);
    else if (c->submit) fail = c->submit(sub, n - (hit ? prefix : 0u), c->submit_user);
    if (fail) { gl2_invalidate_state_cache(c); return -1; }
    if (c->state_cache_enabled && !hit && (parts_src || c->submit) && prefix <= GL2_MAX_HEADER_WORDS) {
        copy_words(c->cached_hdr, seg, prefix);
        c->cached_hdr_words = prefix; c->cached_hdr_valid = 1u;
    }
    return 0;
}
static int submit_draw_seg(gl2_ctx *c, int n, const gl2_program *prog, int count) {
    if (n < 0) return -1;
    uint32_t vwords = 4u + prog->normal_input + (prog->omit_colour ? 0u : 1u) + 2u + (prog->nplan > 1 ? 2u : 0u);
    uint32_t total = (uint32_t)n, vtotal = (uint32_t)count * vwords;
    if (total < vtotal) return -1;
    return submit_draw_packet(c, c->seg, total, total - vtotal, 0, 0);
}
void gl2_set_fs_cache(gl2_ctx *c, const gl2_fs_entry *e, uint32_t n) { c->cache = e; c->ncache = n; }
uint32_t glGetError(gl2_ctx *c) { uint32_t e = c->err; c->err = 0; return e; }
uint32_t glCreateShader(gl2_ctx *c, uint32_t type) {
    if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER) { err(c, GL_INVALID_ENUM); return 0; }
    for (uint32_t i = 1; i < GL2_MAX_SHADERS; ++i) if (!c->sh[i].used) { gl2_shader empty = {0}; c->sh[i] = empty; c->sh[i].used = 1; c->sh[i].type = type; c->sh[i].ok = 0; c->sh[i].log[0] = 0; c->sh[i].src = 0; c->sh[i].src_hash = 0; c->sh[i].vs = 0; c->sh[i].fs = 0; return i; }
    err(c, GL_OUT_OF_MEMORY); return 0;
}
static gl2_shader *shp(gl2_ctx *c, uint32_t sh) { if (sh == 0 || sh >= GL2_MAX_SHADERS || !c->sh[sh].used) { err(c, GL_INVALID_VALUE); return 0; } return &c->sh[sh]; }
void glShaderSourceStrings(gl2_ctx *c,uint32_t sh,int count,const char *const *strings,const int *lengths){
 gl2_shader *s=shp(c,sh);if(!s)return;
 if(count<0||(count&&!strings)){err(c,GL_INVALID_VALUE);return;}
 static char stage[GL2_SHADER_SOURCE_BYTES];uint32_t total=0;
 for(int i=0;i<count;++i){if(!strings[i]){err(c,GL_INVALID_VALUE);return;}
  uint32_t n=0;
  if(lengths&&lengths[i]>=0)n=(uint32_t)lengths[i];
  else{while(n<GL2_SHADER_SOURCE_BYTES&&strings[i][n])++n;}
  if(n>=GL2_SHADER_SOURCE_BYTES-total){err(c,GL_OUT_OF_MEMORY);return;}
  for(uint32_t k=0;k<n;++k)stage[total+k]=strings[i][k];
  total+=n;
 }
 stage[total]=0;
 for(uint32_t i=0;i<=total;++i)c->shader_source[sh][i]=stage[i];
 s->src=c->shader_source[sh];s->source_length=total;s->src_hash=gl2_hash(s->src);
}
void glShaderSource(gl2_ctx *c,uint32_t sh,const char *src){if(!src){if(shp(c,sh))err(c,GL_INVALID_VALUE);return;}glShaderSourceStrings(c,sh,1,&src,0);}
const char *glGetShaderSource(gl2_ctx *c,uint32_t sh){gl2_shader *s=shp(c,sh);return s&&s->src?s->src:"";}
int glIsShader(gl2_ctx *c,uint32_t sh){return sh>0&&sh<GL2_MAX_SHADERS&&c->sh[sh].used;}
static void shader_release(gl2_ctx *c,uint32_t sh){gl2_shader *s=&c->sh[sh];if(s->delete_pending&&!s->attachments){s->used=s->ok=0;s->src=0;s->vs=0;s->fs=0;}}
void glDeleteShader(gl2_ctx *c,uint32_t sh){if(!sh)return;gl2_shader *s=shp(c,sh);if(!s)return;s->delete_pending=1;shader_release(c,sh);}
static int cache_valid(const gl2_fs_entry *e) {
    if(e->software&&gl2_soft_validate(e->software))return 0;
    if(e->has_alpha>3||((e->has_alpha&1u)&&((e->alpha[0]|e->alpha[1])&0x8000u)))return 0;
    if((e->has_alpha&2u)&&(!(e->has_alpha&1u)||e->nplan!=1))return 0;
    if (!e->source || e->nplan < 1 || e->nplan > 2 || e->nsampler > 2 || e->nvary > 3 || e->nuniform>16) return 0;
    for(unsigned i=0;i<e->nuniform;++i){if(!e->uniform_names[i]||!e->uniform_names[i][0]||e->uniform_widths[i]<1||e->uniform_widths[i]>4)return 0;for(unsigned j=0;j<i;++j)if(!scmp(e->uniform_names[i],e->uniform_names[j]))return 0;for(unsigned j=0;j<e->nsampler;++j)if(e->sampler_names[j]&&!scmp(e->uniform_names[i],e->sampler_names[j]))return 0;}
    for (uint32_t i = 0; i < e->nsampler; ++i) {
        if (!e->sampler_names[i] || !e->sampler_names[i][0]) return 0;
        for (uint32_t j = 0; j < i; ++j) if (!scmp(e->sampler_names[i], e->sampler_names[j])) return 0;
    }
    for (uint32_t i = 0; i < e->nvary; ++i) {
        uint32_t s = e->vary_slots[i], w = e->vary_widths[i];
        if (!e->vary_names[i] || !e->vary_names[i][0] ||
            !((s == 1 && w == 4) || (s == 2 && w == 1) || ((s == 3 || s == 4) && w == 2))) return 0;
        for (uint32_t j = 0; j < i; ++j)
            if (s == e->vary_slots[j] || !scmp(e->vary_names[i], e->vary_names[j])) return 0;
    }
    return 1;
}
static int pp_isid(char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_';}
static int pp_isid2(char c){return pp_isid(c)||(c>='0'&&c<='9');}
static const char *gl2_preprocess(const char *src, char *out, uint32_t cap, char *log){
    int has_hash=0; for(const char *p=src;*p;++p)if(*p=='#'){has_hash=1;break;}
    if(!has_hash)return src;
    struct { char name[32], val[64]; uint8_t used; } defs[32];
    for(unsigned i=0;i<32;++i)defs[i].used=0;
    int act[16], seen_else[16], depth=0; act[0]=1; seen_else[0]=0;
    uint32_t outp=0; const char *s=src;
    while(*s){
        const char *ls=s; while(*s&&*s!='\n')++s;
        const char *le=s; if(*s=='\n')++s;
        const char *p=ls; while(p<le&&(*p==' '||*p=='\t'||*p=='\r'))++p;
        if(p<le&&*p=='#'){
            ++p; while(p<le&&(*p==' '||*p=='\t'))++p;
            char dir[16]; unsigned dn=0; while(p<le&&pp_isid(*p)&&dn<15)dir[dn++]=*p++; dir[dn]=0;
            while(p<le&&(*p==' '||*p=='\t'))++p;
            char name[32]; unsigned nn=0; while(p<le&&pp_isid2(*p)&&nn<31)name[nn++]=*p++; name[nn]=0;
            int active=act[depth];
            if(!scmp(dir,"ifdef")||!scmp(dir,"ifndef")){
                if(!nn||depth>=15){scpy(log,"preprocessor conditional error",64);return 0;}
                int found=0; for(unsigned i=0;i<32;++i)if(defs[i].used&&!scmp(defs[i].name,name)){found=1;break;}
                int cond=(!scmp(dir,"ifdef"))?found:!found;
                ++depth; act[depth]=active&&cond; seen_else[depth]=0;
            }else if(!scmp(dir,"else")){
                if(depth==0||seen_else[depth]){scpy(log,"unexpected #else",64);return 0;}
                seen_else[depth]=1; act[depth]=act[depth-1]&&!act[depth];
            }else if(!scmp(dir,"endif")){
                if(depth==0){scpy(log,"unexpected #endif",64);return 0;}
                --depth;
            }else if(!scmp(dir,"define")){
                if(active){
                    if(!nn){scpy(log,"invalid #define",64);return 0;}
                    while(p<le&&(*p==' '||*p=='\t'))++p;
                    const char *ve=le; while(ve>p&&(ve[-1]==' '||ve[-1]=='\t'||ve[-1]=='\r'))--ve;
                    int slot=-1; for(unsigned i=0;i<32;++i)if(defs[i].used&&!scmp(defs[i].name,name)){slot=(int)i;break;}
                    if(slot<0)for(unsigned i=0;i<32;++i)if(!defs[i].used){slot=(int)i;break;}
                    if(slot<0){scpy(log,"too many #define macros",64);return 0;}
                    defs[slot].used=1; scpy(defs[slot].name,name,32);
                    uint32_t vn=(uint32_t)(ve-p); if(vn>=63)vn=63;
                    for(uint32_t k=0;k<vn;++k)defs[slot].val[k]=p[k];
                    defs[slot].val[vn]=0;
                }
            }else if(!scmp(dir,"undef")){
                if(active){ for(unsigned i=0;i<32;++i)if(defs[i].used&&!scmp(defs[i].name,name))defs[i].used=0; }
            }else if(!scmp(dir,"pragma")||!scmp(dir,"version")){
                /* ignore */
            }else{
                scpy(log,"unsupported preprocessor directive",64);return 0;
            }
            if(outp+1>=cap){scpy(log,"preprocessed shader too large",64);return 0;}
            out[outp++]='\n';
            continue;
        }
        if(!act[depth]){if(outp+1>=cap){scpy(log,"preprocessed shader too large",64);return 0;}out[outp++]='\n';continue;}
        const char *q=ls;
        while(q<le){
            if(pp_isid(*q)){
                char id[32]; unsigned in=0; const char *qs=q;
                while(q<le&&pp_isid2(*q)){if(in<31)id[in++]=*q; ++q;}
                id[in]=0;
                const char *rep=0;
                if(in<31&&(uint32_t)(q-qs)==in){for(unsigned i=0;i<32;++i)if(defs[i].used&&!scmp(defs[i].name,id)){rep=defs[i].val;break;}}
                if(rep){for(const char *r=rep;*r;++r){if(outp+1>=cap){scpy(log,"preprocessed shader too large",64);return 0;}out[outp++]=*r;}}
                else{for(const char *r=qs;r<q;++r){if(outp+1>=cap){scpy(log,"preprocessed shader too large",64);return 0;}out[outp++]=*r;}}
            }else{
                if(outp+1>=cap){scpy(log,"preprocessed shader too large",64);return 0;}
                out[outp++]=*q++;
            }
        }
        if(outp+1>=cap){scpy(log,"preprocessed shader too large",64);return 0;}
        out[outp++]='\n';
    }
    if(depth!=0){scpy(log,"unterminated #ifdef",64);return 0;}
    out[outp]=0;
    return out;
}
void glCompileShader(gl2_ctx *c, uint32_t sh) {
    gl2_shader *s = shp(c, sh); if (!s) return; s->ok = 0; s->log[0] = 0; s->vs = 0; s->fs = 0;
    if (!s->src) { scpy(s->log, "shader has no source", 64); return; }
    for(uint32_t i=0;i<s->source_length;++i)if(!s->src[i]){scpy(s->log,"embedded NUL in shader source",64);return;}
    static char pp_buf[GL2_SHADER_SOURCE_BYTES];
    const char *src = gl2_preprocess(s->src, pp_buf, sizeof pp_buf, s->log);
    if (!src) return;
    if (s->type == GL_VERTEX_SHADER) {
        vsc_result *r = &c->sh_vs[sh];          /* shader IDs must never alias compiled storage */
        if (gl2vsc_compile(src, r) == 0) { s->ok = 1; s->vs = r; } else scpy(s->log, r->err, 64);
    } else {
        uint32_t h = gl2_hash(src);
        for (uint32_t i = 0; i < c->ncache; ++i) if (c->cache[i].hash == h &&
            c->cache[i].source && !scmp(c->cache[i].source, src)) {
            if (!cache_valid(&c->cache[i])) { scpy(s->log, "invalid cached fragment metadata", 64); return; }
            s->fs = &c->cache[i]; s->ok = 1; return;
        }
        if(!compile_fragment(src,&c->sh_fs[sh],s->log)){s->fs=&c->sh_fs[sh].e;s->ok=1;}
    }
}
static uint32_t text_length(const char *s){uint32_t n=0;while(s[n])++n;return n;}
int glGetShaderiv(gl2_ctx *c,uint32_t sh,uint32_t pname){gl2_shader *s=shp(c,sh);if(!s)return 0;
 if(pname==GL_COMPILE_STATUS)return (int)s->ok;
 if(pname==GL_SHADER_TYPE)return (int)s->type;
 if(pname==GL_DELETE_STATUS)return (int)s->delete_pending;
 if(pname==GL_SHADER_SOURCE_LENGTH)return s->src?(int)s->source_length+1:0;
 if(pname==GL_INFO_LOG_LENGTH)return s->log[0]?(int)text_length(s->log)+1:0;
 err(c,GL_INVALID_ENUM);return 0;
}
const char *glGetShaderInfoLog(gl2_ctx *c, uint32_t sh) { gl2_shader *s = shp(c, sh); return s ? s->log : ""; }
uint32_t glCreateProgram(gl2_ctx *c) {
    for (uint32_t i = 1; i < GL2_MAX_PROGRAMS; ++i) if (!c->pr[i].used) { gl2_prog_obj empty = {0}; c->pr[i] = empty; c->pr[i].used = 1; c->pr[i].ok = 0; c->pr[i].vsh = c->pr[i].fsh = 0; c->pr[i].log[0] = 0; return i; }
    err(c, GL_OUT_OF_MEMORY); return 0;
}
static gl2_prog_obj *prp(gl2_ctx *c, uint32_t p) { if (p == 0 || p >= GL2_MAX_PROGRAMS || !c->pr[p].used) { err(c, GL_INVALID_VALUE); return 0; } return &c->pr[p]; }
void glAttachShader(gl2_ctx *c, uint32_t p, uint32_t sh) {
    gl2_prog_obj *P = prp(c, p); gl2_shader *S = shp(c, sh); if (!P || !S) return;
    uint32_t *slot = S->type == GL_VERTEX_SHADER ? &P->vsh : &P->fsh;
    if (*slot) { err(c, GL_INVALID_OPERATION); return; }
    *slot = sh; ++S->attachments;
}
static void link_build(gl2_ctx *c,gl2_prog_obj *P) {
    P->ok = 0;
    if (!P->vsh || !P->fsh || !c->sh[P->vsh].ok || !c->sh[P->fsh].ok) { scpy(P->log, "missing or uncompiled shader", 64); return; }
    P->vs = *c->sh[P->vsh].vs; fs_copy(&P->fs_owned,c->sh[P->fsh].fs);P->fs=&P->fs_owned.e;
    uint32_t has_col = 0, has_uv = 0; for (uint32_t i = 0; i < P->vs.nvary; ++i) { if (P->vs.vary_slot[i] == 1) has_col = 1; if (P->vs.vary_slot[i] == 3) has_uv = 1; }
    if (P->fs->needs_colour && !has_col) { scpy(P->log, "fragment shader reads a colour varying the vertex shader does not write", 64); return; }
    if (P->fs->nsampler && !has_uv) { scpy(P->log, "fragment shader samples a texture but the vertex shader writes no texcoord", 64); return; }
    if (P->fs->has_constp && !has_col) { scpy(P->log, "vertex shader needs a vec4 colour varying for this fragment shader", 64); return; }
    for (uint32_t i = 0; i < P->fs->nvary; ++i) {
        uint32_t j;
        for (j = 0; j < P->vs.nvary; ++j) if (!scmp(P->fs->vary_names[i], P->vs.vary_name[j])) break;
        if (j == P->vs.nvary || P->vs.vary_slot[j] != P->fs->vary_slots[i] || P->vs.vary_w[j] != P->fs->vary_widths[i]) {
            scpy(P->log, "fragment varying name/type/slot does not match vertex output", 64); return;
        }
    }
    for (uint32_t i = 0; i < P->fs->nsampler; ++i) for (uint32_t j = 0; j < P->vs.nuni; ++j)
        if (!scmp(P->fs->sampler_names[i], P->vs.uni_name[j])) {
            scpy(P->log, "uniform type conflict between shader stages", 64); return;
        }
    for(unsigned i=0;i<16;++i){P->fragment_vertex_alias[i]=0;for(unsigned k=0;k<4;++k)P->fragment_values[i][k]=0;}
    for(unsigned i=0;i<P->fs->nuniform;++i)for(unsigned j=0;j<P->vs.nuni;++j)if(!scmp(P->fs->uniform_names[i],P->vs.uni_name[j])){
        if(P->fs->uniform_widths[i]!=P->vs.uni_width[j]){scpy(P->log,"uniform type conflict between shader stages",64);return;}
        P->fragment_vertex_alias[i]=(uint8_t)(j+1);
    }
    /* assemble a gl2_program directly from the compiled pieces (no serialised blob on the device) */
    gl2_program *g = &P->prog; g->nstage = P->vs.ncode; g->nplan = P->fs->nplan; g->code = &P->vs.code[0][0];
    g->nlit = P->vs.nlit; g->lit_idx = P->vs.lit_idx; g->lit_val = &P->vs.lit_val[0][0]; g->nuni = P->vs.nuni;
    /* All vertex tables belong to this linked program snapshot, not a global program-ID slot. */
    g->dot3_mask=(P->fs->has_alpha>>1)&1u;
    g->repeat_uv0=P->fs->software&&g->nplan==2&&P->fs->nsampler==1;g->custom_alpha=P->fs->has_alpha&1u;g->alpha[0]=P->fs->alpha[0];g->alpha[1]=P->fs->alpha[1];g->normal_input=0;g->omit_colour=!P->fs->has_constp;for(uint32_t i=0;i<P->vs.nattr;++i){if(P->vs.attr_slot[i]==1)g->normal_input=P->vs.attr_w[i];if(P->vs.attr_slot[i]==2)g->omit_colour=0;}
    g->uni_idx = P->vs.uni_idx; g->uni_cnt = P->vs.uni_cnt; g->uni_names = 0; g->plan = &P->fs->plan[0][0]; g->has_constp = P->fs->has_constp;
    for (int i = 0; i < 4; ++i) g->constp[i] = P->fs->constp[i];
    for (uint32_t i = 0; i < GL2_MAX_UNI; ++i) { g->uset[i] = i < g->nuni;
        for(unsigned r=0;r<4;++r)for(unsigned k=0;k<4;++k)g->uval[i][r][k]=0;
    }
    g->rounding_bias=0;
    if(g->repeat_uv0&&gl2_soft_is_texture_cube(P->fs->software)){
        uint8_t occupied[112]={0};uint32_t stages=g->nstage+1;
        for(unsigned i=0;i<g->nstage;++i)stages+=(g->code[2*i+1]>>26)==3;
        for(unsigned i=0;i<g->nlit;++i)if(g->lit_idx[i]<112)occupied[g->lit_idx[i]]=1;
        for(unsigned u=0;u<g->nuni;++u)for(unsigned row=0;row<g->uni_cnt[u];++row)if(g->uni_idx[u]+row<112)occupied[g->uni_idx[u]+row]=1;
        if(stages<=256)for(unsigned i=0;i<112;++i)if(!occupied[i]){g->rounding_bias=i+1;break;}
        if(g->rounding_bias)for(unsigned u=0;u<2;++u){uint8_t *plan=P->fs_owned.e.plan[u];plan[5]=0;plan[6]=3;plan[7]=0;plan[8]=1;plan[9]=0;g->alpha[u]=(uint16_t)((g->alpha[u]&~((3u<<9)|(3u<<7)|8u|16u))|(3u<<7)|8u);}
    }
    P->sampler_unit[0] = P->sampler_unit[1] = 0; /* GLES sampler-uniform defaults */
    P->ok = 1; P->executable = 1; P->validate_status = 0; scpy(P->log, P->fs->nplan?"linked:native combiner; state-dependent route":"linked:software-only fragment (slow)", 64);
}
void glLinkProgram(gl2_ctx *c,uint32_t p){
 gl2_prog_obj *P=prp(c,p);if(!P)return;
 static gl2_prog_obj candidate;candidate=*P;link_build(c,&candidate);
 if(!candidate.ok){P->ok=0;scpy(P->log,candidate.log,64);return;}
 *P=candidate;fs_rebase(P);gl2_program *g=&P->prog;
 g->code=&P->vs.code[0][0];g->lit_idx=P->vs.lit_idx;g->lit_val=&P->vs.lit_val[0][0];g->uni_idx=P->vs.uni_idx;g->uni_cnt=P->vs.uni_cnt;
 gl2_invalidate_state_cache(c);
}
int glIsProgram(gl2_ctx *c,uint32_t p){return p>0&&p<GL2_MAX_PROGRAMS&&c->pr[p].used;}
void glDetachShader(gl2_ctx *c,uint32_t p,uint32_t sh){
 gl2_prog_obj *P=prp(c,p);gl2_shader *S=shp(c,sh);if(!P||!S)return;
 uint32_t *slot=P->vsh==sh?&P->vsh:P->fsh==sh?&P->fsh:0;
 if(!slot){err(c,GL_INVALID_OPERATION);return;}*slot=0;--S->attachments;shader_release(c,sh);
}
static void program_release(gl2_ctx *c,uint32_t p){
 gl2_prog_obj *P=&c->pr[p];if(P->vsh)glDetachShader(c,p,P->vsh);if(P->fsh)glDetachShader(c,p,P->fsh);
 P->used=P->ok=P->executable=0;
}
void glDeleteProgram(gl2_ctx *c,uint32_t p){if(!p)return;gl2_prog_obj *P=prp(c,p);if(!P)return;P->delete_pending=1;gl2_invalidate_state_cache(c);if(c->cur!=p)program_release(c,p);}
void glGetAttachedShaders(gl2_ctx *c,uint32_t p,int capacity,int *count,uint32_t *out){
 if(capacity<0||(capacity&&!out)){err(c,GL_INVALID_VALUE);return;}gl2_prog_obj *P=prp(c,p);if(!P)return;int n=0;
 if(P->vsh&&n<capacity)out[n++]=P->vsh;
 if(P->fsh&&n<capacity)out[n++]=P->fsh;
 if(count)*count=n;
}
int glGetProgramiv(gl2_ctx *c,uint32_t p,uint32_t pname){gl2_prog_obj *P=prp(c,p);if(!P)return 0;
 if(pname==GL_LINK_STATUS)return (int)P->ok;
 if(pname==GL_DELETE_STATUS)return (int)P->delete_pending;
 if(pname==GL_ATTACHED_SHADERS)return (P->vsh!=0)+(P->fsh!=0);
 if(pname==GL_INFO_LOG_LENGTH)return P->log[0]?(int)text_length(P->log)+1:0;
 if(pname==GL_VALIDATE_STATUS)return (int)P->validate_status;
 err(c,GL_INVALID_ENUM);return 0;
}
const char *glGetProgramInfoLog(gl2_ctx *c, uint32_t p) { gl2_prog_obj *P = prp(c, p); return P ? P->log : ""; }
void glUseProgram(gl2_ctx *c,uint32_t p){
 if(p&&(p>=GL2_MAX_PROGRAMS||!c->pr[p].used||!c->pr[p].ok)){err(c,GL_INVALID_OPERATION);return;}
 uint32_t old=c->cur;c->cur=p;gl2_invalidate_state_cache(c);if(old&&old!=p&&c->pr[old].delete_pending)program_release(c,old);
}
int glGetUniformLocation(gl2_ctx *c, uint32_t p, const char *name) {
    gl2_prog_obj *P = prp(c, p); if (!P || !P->ok) { err(c, GL_INVALID_OPERATION); return -1; }
    for (uint32_t i = 0; i < P->vs.nuni; ++i) { if (!scmp(P->vs.uni_name[i], name)) return (int)i; }
    for (uint32_t i = 0; i < P->fs->nsampler; ++i) if (!scmp(P->fs->sampler_names[i], name)) return GL2_MAX_UNI + (int)i;
    for(unsigned i=0;i<P->fs->nuniform;++i)if(!scmp(P->fs->uniform_names[i],name))return P->fragment_vertex_alias[i]?(int)P->fragment_vertex_alias[i]-1:GL2_FRAGMENT_UNIFORM_BASE+(int)i;
    return -1;
}
int glGetAttribLocation(gl2_ctx *c, uint32_t p, const char *name) {
    gl2_prog_obj *P = prp(c, p); if (!P || !P->ok) { err(c, GL_INVALID_OPERATION); return -1; }
    for (uint32_t i = 0; i < P->vs.nattr; ++i) { if (!scmp(P->vs.attr_name[i], name)) return P->vs.attr_slot[i]; }
    return -1;
}
static gl2_prog_obj *cur(gl2_ctx *c) {
    if (!c->cur || c->cur >= GL2_MAX_PROGRAMS || !c->pr[c->cur].used || !c->pr[c->cur].executable) { err(c, GL_INVALID_OPERATION); return 0; }
    return &c->pr[c->cur];
}
void glUniform1i(gl2_ctx *c, int loc, int unit) {
    gl2_prog_obj *P = cur(c); if (!P || loc == -1) return;
    if (loc < GL2_MAX_UNI || loc >= GL2_MAX_UNI + (int)P->fs->nsampler) { err(c, GL_INVALID_OPERATION); return; }
    if (unit < 0 || unit >= 2) { err(c, GL_INVALID_VALUE); return; }
    P->sampler_unit[loc - GL2_MAX_UNI] = (uint8_t)unit;
    gl2_invalidate_state_cache(c);
}
static void uniform_float(gl2_ctx *c,int loc,unsigned width,const float values[4]){
 gl2_prog_obj *P=cur(c);if(!P||loc==-1)return;
 if(loc>=0&&(unsigned)loc<P->vs.nuni){
  if(P->vs.uni_cnt[loc]!=1||P->vs.uni_width[loc]!=width){err(c,GL_INVALID_OPERATION);return;}
  gl2_uniform4f(&P->prog,loc,values[0],values[1],values[2],values[3]);
  for(unsigned i=0;i<P->fs->nuniform;++i)if(P->fragment_vertex_alias[i]==loc+1)for(unsigned k=0;k<4;++k)P->fragment_values[i][k]=values[k];
  gl2_invalidate_state_cache(c);return;
 }
 if(loc>=GL2_FRAGMENT_UNIFORM_BASE&&(unsigned)(loc-GL2_FRAGMENT_UNIFORM_BASE)<P->fs->nuniform){unsigned i=(unsigned)(loc-GL2_FRAGMENT_UNIFORM_BASE);
  if(P->fragment_vertex_alias[i]||P->fs->uniform_widths[i]!=width){err(c,GL_INVALID_OPERATION);return;}for(unsigned k=0;k<4;++k)P->fragment_values[i][k]=values[k];gl2_invalidate_state_cache(c);return;
 }
 err(c,GL_INVALID_OPERATION);
}
void glUniform1f(gl2_ctx *c,int loc,float x){float v[4]={x,0,0,0};uniform_float(c,loc,1,v);}
void glUniform2f(gl2_ctx *c,int loc,float x,float y){float v[4]={x,y,0,0};uniform_float(c,loc,2,v);}
void glUniform3f(gl2_ctx *c,int loc,float x,float y,float z){float v[4]={x,y,z,0};uniform_float(c,loc,3,v);}
void glUniform4f(gl2_ctx *c,int loc,float x,float y,float z,float w){float v[4]={x,y,z,w};uniform_float(c,loc,4,v);}
void glUniformMatrix4fv(gl2_ctx *c, int loc, int count, int transpose, const float *m) {
    gl2_prog_obj *P = cur(c); if (!P || loc == -1) return;
    if (transpose) { err(c, GL_INVALID_VALUE); return; }
    if (loc < 0 || (uint32_t)loc >= P->vs.nuni || P->vs.uni_cnt[loc] != 4 || count != 1) { err(c, GL_INVALID_OPERATION); return; }
    gl2_uniform_matrix4(&P->prog, loc, m);
    gl2_invalidate_state_cache(c);
}
void glUniformMatrix3fv(gl2_ctx *c, int loc, int count, int transpose, const float *m) {
    gl2_prog_obj *P = cur(c); if (!P || loc == -1) return;
    if (transpose) { err(c, GL_INVALID_VALUE); return; }
    if (loc < 0 || (uint32_t)loc >= P->vs.nuni || P->vs.uni_cnt[loc] != 3 || count != 1) { err(c, GL_INVALID_OPERATION); return; }
    gl2_uniform_matrix3(&P->prog, loc, m);
    gl2_invalidate_state_cache(c);
}
void glUniformMatrix2fv(gl2_ctx *c, int loc, int count, int transpose, const float *m) {
    gl2_prog_obj *P = cur(c); if (!P || loc == -1) return;
    if (transpose) { err(c, GL_INVALID_VALUE); return; }
    if (loc < 0 || (uint32_t)loc >= P->vs.nuni || P->vs.uni_cnt[loc] != 2 || count != 1) { err(c, GL_INVALID_OPERATION); return; }
    gl2_uniform_matrix2(&P->prog, loc, m);
    gl2_invalidate_state_cache(c);
}
void glEnableVertexAttribArray(gl2_ctx *c, uint32_t loc) { if (loc > 4) { err(c, GL_INVALID_VALUE); return; } c->attr[loc].enabled = 1; }
void glDisableVertexAttribArray(gl2_ctx *c, uint32_t loc) { if (loc > 4) { err(c, GL_INVALID_VALUE); return; } c->attr[loc].enabled = 0; }
static uint32_t attribute_bytes(uint32_t type){
 return type==GL_BYTE||type==GL_UNSIGNED_BYTE?1:type==GL_SHORT||type==GL_UNSIGNED_SHORT?2:type==GL_FLOAT||type==GL_FIXED?4:0;
}
void glVertexAttrib4f(gl2_ctx *c,uint32_t loc,float x,float y,float z,float w){if(loc>4){err(c,GL_INVALID_VALUE);return;}float *v=c->attr[loc].current;v[0]=x;v[1]=y;v[2]=z;v[3]=w;}
void glVertexAttrib1f(gl2_ctx *c,uint32_t loc,float x){glVertexAttrib4f(c,loc,x,0,0,1);}
void glVertexAttrib2f(gl2_ctx *c,uint32_t loc,float x,float y){glVertexAttrib4f(c,loc,x,y,0,1);}
void glVertexAttrib3f(gl2_ctx *c,uint32_t loc,float x,float y,float z){glVertexAttrib4f(c,loc,x,y,z,1);}
void glVertexAttribPointer(gl2_ctx *c,uint32_t loc,int size,uint32_t type,int normalized,int stride,const void *ptr){
 if(loc>4||size<1||size>4||stride<0){err(c,GL_INVALID_VALUE);return;}
 uint32_t bytes=attribute_bytes(type);if(!bytes){err(c,GL_INVALID_ENUM);return;}
 gl2_attribute *a=&c->attr[loc];a->ptr=ptr;a->size=(uint32_t)size;a->type=type;a->normalized=normalized!=0;
 a->stride=stride?(uint32_t)stride:(uint32_t)size*bytes;a->buffer=c->array_buffer;
}
void glBlendFunc(gl2_ctx *c,uint32_t s,uint32_t d){if(!((s<=1)||(s>=0x300&&s<=0x308))||!((d<=1)||(d>=0x300&&d<=0x307))){err(c,GL_INVALID_ENUM);return;}c->st.src=s;c->st.dst=d;gl2_invalidate_state_cache(c);}
void glDepthFunc(gl2_ctx *c, uint32_t f) { if (f < 0x200 || f > 0x207) { err(c, GL_INVALID_ENUM); return; } c->st.depth_func = f; gl2_invalidate_state_cache(c); }
void glDepthMask(gl2_ctx *c, int flag) { c->st.depth_write = flag != 0; gl2_invalidate_state_cache(c); }
void glEnable(gl2_ctx *c, uint32_t cap) { if (cap == 0xBE2) c->st.blend = 1; else if (cap == 0xB71) c->st.depth_test = 1; else if (cap == GL_CULL_FACE) c->cull_enabled = 1; else if (cap == GL_SCISSOR_TEST) c->scissor_enabled = 1; else { err(c, GL_INVALID_ENUM); return; } gl2_invalidate_state_cache(c); }
void glDisable(gl2_ctx *c, uint32_t cap) { if (cap == 0xBE2) c->st.blend = 0; else if (cap == 0xB71) c->st.depth_test = 0; else if (cap == GL_CULL_FACE) c->cull_enabled = 0; else if (cap == GL_SCISSOR_TEST) c->scissor_enabled = 0; else { err(c, GL_INVALID_ENUM); return; } gl2_invalidate_state_cache(c); }
void glCullFace(gl2_ctx *c, uint32_t mode) {
    if (mode != GL_FRONT && mode != GL_BACK && mode != GL_FRONT_AND_BACK) { err(c, GL_INVALID_ENUM); return; }
    c->cull_face = mode;
}
void glFrontFace(gl2_ctx *c, uint32_t mode) {
    if (mode != GL_CW && mode != GL_CCW) { err(c, GL_INVALID_ENUM); return; }
    c->front_face = mode;
}
int glIsEnabled(gl2_ctx *c, uint32_t cap) {
    if (cap == GL_CULL_FACE) return c->cull_enabled != 0;
    if (cap == 0xBE2) return c->st.blend != 0;
    if (cap == 0xB71) return c->st.depth_test != 0;
    err(c, GL_INVALID_ENUM); return 0;
}
void glViewport(gl2_ctx *c, int x, int y, int w, int h) { (void)x; (void)y; if (w < 0 || h < 0) { err(c, GL_INVALID_VALUE); return; } c->vp_w = (uint32_t)w; c->vp_h = (uint32_t)h; gl2_invalidate_state_cache(c); }
void gl2_bind_texture_native(gl2_ctx *c,uint32_t unit,uint32_t va,uint32_t l2,uint32_t storage) {
    if (unit>=2) { err(c,GL_INVALID_VALUE);return; }
    uint32_t format=storage&~GL2_TEX_LINEAR;
    if (format!=GL2_TEX_ARGB8888 && format!=GL2_TEX_RGB565 && format!=GL2_TEX_ARGB4444 && format!=GL2_TEX_ARGB1555 && format!=GL2_TEX_AL88) { err(c,GL_INVALID_ENUM);return; }
    c->raw_unit_tex[unit].va=va;c->raw_unit_tex[unit].log2size=l2;c->raw_unit_tex[unit].storage=storage;
    c->tex[unit]=c->raw_unit_tex[unit];c->texture_managed[unit]=0;c->texture_binding[unit]=0;
    gl2_invalidate_state_cache(c);
}
void gl2_bind_texture(gl2_ctx *c,uint32_t unit,uint32_t va,uint32_t l2) {
    gl2_bind_texture_native(c,unit,va,l2,GL2_TEX_ARGB8888);
}
/* Bounded CPU-side buffer storage. MBX vertex execution remains on the VGP;
 * gather/deindex here only packs the non-indexed TA FIFO vertex payload. */
static gl2_buffer *buffer_named(gl2_ctx *c, uint32_t name) {
    if (!name) return 0;
    for (uint32_t i = 0; i < GL2_MAX_BUFFERS; ++i)
        if (c->buffers[i].reserved && c->buffers[i].name == name) return &c->buffers[i];
    return 0;
}
static uint32_t *buffer_binding(gl2_ctx *c, uint32_t target) {
    if (target == GL_ARRAY_BUFFER) return &c->array_buffer;
    if (target == GL_ELEMENT_ARRAY_BUFFER) return &c->element_buffer;
    err(c, GL_INVALID_ENUM); return 0;
}
static gl2_buffer *bound_buffer(gl2_ctx *c, uint32_t target) {
    uint32_t *binding = buffer_binding(c, target); if (!binding) return 0;
    gl2_buffer *b = buffer_named(c, *binding);
    if (!b || !b->used) { err(c, GL_INVALID_OPERATION); return 0; }
    return b;
}
void glGenBuffers(gl2_ctx *c, int n, uint32_t *names) {
    if (n < 0 || (n && !names)) { err(c, GL_INVALID_VALUE); return; }
    uint32_t free_slots = 0;
    for (uint32_t i = 0; i < GL2_MAX_BUFFERS; ++i) free_slots += !c->buffers[i].reserved;
    if ((uint32_t)n > free_slots) { err(c, GL_OUT_OF_MEMORY); return; }
    for (int k = 0; k < n; ++k) {
        while (!c->next_buffer || buffer_named(c, c->next_buffer)) ++c->next_buffer;
        for (uint32_t i = 0; i < GL2_MAX_BUFFERS; ++i) if (!c->buffers[i].reserved) {
            gl2_buffer *b = &c->buffers[i]; b->reserved = 1; b->used = 0;
            b->name = c->next_buffer++; b->size = 0; b->usage = GL_STATIC_DRAW;
            names[k] = b->name; break;
        }
    }
}
void glBindBuffer(gl2_ctx *c, uint32_t target, uint32_t name) {
    uint32_t *binding = buffer_binding(c, target); if (!binding) return;
    if (!name) { *binding = 0; return; }
    gl2_buffer *b = buffer_named(c, name);
    if (!b) for (uint32_t i = 0; i < GL2_MAX_BUFFERS; ++i) if (!c->buffers[i].reserved) {
        b = &c->buffers[i]; b->reserved = 1; b->name = name; b->size = 0; b->usage = GL_STATIC_DRAW; break;
    }
    if (!b) { err(c, GL_OUT_OF_MEMORY); return; }
    b->used = 1; *binding = name;
}
void glDeleteBuffers(gl2_ctx *c, int n, const uint32_t *names) {
    if (n < 0 || (n && !names)) { err(c, GL_INVALID_VALUE); return; }
    for (int k = 0; k < n; ++k) {
        gl2_buffer *b = buffer_named(c, names[k]); if (!b) continue;
        if (c->array_buffer == b->name) c->array_buffer = 0;
        if (c->element_buffer == b->name) c->element_buffer = 0;
        for (uint32_t a = 0; a < 5; ++a) if (c->attr[a].buffer == b->name) {
            c->attr[a].buffer = 0; c->attr[a].ptr = 0;
        }
        b->reserved = b->used = b->size = 0;
    }
}
int glIsBuffer(gl2_ctx *c, uint32_t name) { gl2_buffer *b = buffer_named(c, name); return b && b->used; }
void glBufferData(gl2_ctx *c, uint32_t target, intptr_t size, const void *data, uint32_t usage) {
    if (size < 0) { err(c, GL_INVALID_VALUE); return; }
    if (usage != GL_STATIC_DRAW && usage != GL_DYNAMIC_DRAW && usage != GL_STREAM_DRAW) { err(c, GL_INVALID_ENUM); return; }
    gl2_buffer *b = bound_buffer(c, target); if (!b) return;
    if ((uintptr_t)size > GL2_BUFFER_BYTES) { err(c, GL_OUT_OF_MEMORY); return; }
    const uint8_t *p = data;
    for (uint32_t i = 0; i < (uint32_t)size; ++i) b->data[i] = p ? p[i] : 0;
    b->size = (uint32_t)size; b->usage = usage;
}
void glBufferSubData(gl2_ctx *c, uint32_t target, intptr_t offset, intptr_t size, const void *data) {
    if (offset < 0 || size < 0 || (size && !data)) { err(c, GL_INVALID_VALUE); return; }
    gl2_buffer *b = bound_buffer(c, target); if (!b) return;
    if ((uintptr_t)offset > b->size || (uintptr_t)size > b->size - (uintptr_t)offset) { err(c, GL_INVALID_VALUE); return; }
    const uint8_t *p = data;
    for (uint32_t i = 0; i < (uint32_t)size; ++i) b->data[(uint32_t)offset + i] = p[i];
}
int glGetBufferParameteriv(gl2_ctx *c, uint32_t target, uint32_t pname) {
    gl2_buffer *b = bound_buffer(c, target); if (!b) return 0;
    if (pname == GL_BUFFER_SIZE) return (int)b->size;
    if (pname == GL_BUFFER_USAGE) return (int)b->usage;
    err(c, GL_INVALID_ENUM); return 0;
}
/* Load via bytes: arbitrary legal byte strides and unaligned VBO/EBO offsets
 * must not emit unaligned ARM float/halfword accesses with the MMU off. */
static float load_float(const uint8_t *p) {
    union { uint32_t u; float f; } v;
    v.u = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    return v.f;
}
static const uint8_t *attribute_at(gl2_ctx *c, uint32_t slot, uint32_t index) {
    const gl2_attribute *a = &c->attr[slot];
    uintptr_t base = (uintptr_t)a->ptr; uint32_t bytes = a->size * attribute_bytes(a->type);
    if (a->buffer) {
        gl2_buffer *b = buffer_named(c, a->buffer);
        if (!b || !b->used || base > b->size || bytes > b->size - base ||
            index > (b->size - base - bytes) / a->stride) { err(c, GL_INVALID_OPERATION); return 0; }
        return b->data + base + (uintptr_t)index * a->stride;
    }
    if (!base || base > UINTPTR_MAX - bytes || index > (UINTPTR_MAX - base - bytes) / a->stride) {
        err(c, GL_INVALID_OPERATION); return 0;
    }
    return (const uint8_t *)(base + (uintptr_t)index * a->stride);
}
static float component(const gl2_attribute *a,const uint8_t *p,uint32_t k){
 if(!p)return a->current[k];
 if(k>=a->size)return k==3?1.0f:0.0f;
 p+=k*attribute_bytes(a->type);
 if(a->type==GL_FLOAT)return load_float(p);
 if(a->type==GL_FIXED){uint32_t v=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;return (v&0x80000000u?-(float)(~v+1u):(float)v)/65536.0f;}
 uint32_t u=p[0];if(a->type==GL_SHORT||a->type==GL_UNSIGNED_SHORT)u|=(uint32_t)p[1]<<8;
 if(a->type==GL_BYTE||a->type==GL_SHORT){int v=(int)u;uint32_t bits=a->type==GL_BYTE?8:16;if(u&(1u<<(bits-1)))v-=(int)(1u<<bits);
  return a->normalized?(2.0f*(float)v+1.0f)/(bits==8?255.0f:65535.0f):(float)v;
 }
 return a->normalized?(float)u/(a->type==GL_UNSIGNED_BYTE?255.0f:65535.0f):(float)u;
}
/* Validate one attribute range once, then gather it without repeating buffer
 * lookup/type checks for every vertex component. memcpy preserves exact float
 * bits; only aligned float vectors use the wider loads on this ARM target. */
static int gather_attribute(gl2_ctx *c,unsigned slot,unsigned active,const uint32_t *indices,unsigned count,unsigned width,unsigned stride,float *dst,uint32_t maximum){
 const gl2_attribute *a=&c->attr[slot];const uint8_t *base=0;
 if((active&(1u<<slot))&&a->enabled){if(!attribute_at(c,slot,maximum))return -1;base=attribute_at(c,slot,0);if(!base)return -1;}
 if(!base){for(unsigned i=0;i<count;++i)for(unsigned k=0;k<width;++k)dst[i*stride+k]=a->current[k];return 0;}
#if defined(__GNUC__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
 if(a->type==GL_FLOAT&&a->size>=width&&!(((uintptr_t)base|a->stride)&3u)){
#define GATHER_FLOAT_BYTES(bytes) do{for(unsigned i=0;i<count;++i){const void *src=__builtin_assume_aligned(base+(uintptr_t)indices[i]*a->stride,4);__builtin_memcpy(dst+i*stride,src,bytes);}return 0;}while(0)
  if(width==4){GATHER_FLOAT_BYTES(16);}
  if(width==3){GATHER_FLOAT_BYTES(12);}
  if(width==2){GATHER_FLOAT_BYTES(8);}
  if(width==1){GATHER_FLOAT_BYTES(4);}
#undef GATHER_FLOAT_BYTES
 }
#endif
 for(unsigned i=0;i<count;++i){const uint8_t *src=base+(uintptr_t)indices[i]*a->stride;for(unsigned k=0;k<width;++k)dst[i*stride+k]=component(a,src,k);}return 0;
}
#include "gl2soft.inc"
static int draw_gather(gl2_ctx *c, gl2_prog_obj *P, uint32_t mode, int count, const uint32_t *indices) {
    static float pos[1024], normals[1024], col[1024], uv0[512], uv1[512];
    if(!c->soft_target&&(!P->fs->nplan||(!c->diagnostic_gpu&&gl2_program_needs_software(c,P)))){err(c,GL_INVALID_OPERATION);return -1;}
    if (gl2_resolve_texture_units(c, P->prog.nplan?P->prog.nplan:1, P->fs->nsampler, P->sampler_unit)) return -1;
    uint32_t active=0;for(uint32_t i=0;i<P->vs.nattr;++i)active|=1u<<P->vs.attr_slot[i];
    uint32_t maximum=0;for(int i=0;i<count;++i)if(indices[i]>maximum)maximum=indices[i];
    /* Consecutive aligned float arrays can be packed straight from their
     * validated source, avoiding a second copy through the gather scratch.
     * Each other attribute retains the existing typed/default conversion. */
    if(!c->soft_target){
        unsigned sequential=1;for(int i=1;i<count;++i)if(indices[i]!=indices[0]+(uint32_t)i){sequential=0;break;}
        if(sequential){
            gl2_vertex_view views[5]={{pos,4},{normals,4},{col,4},{uv0,2},{uv1,2}};
            unsigned widths[5]={4,P->prog.normal_input,(!P->prog.omit_colour&&!P->prog.has_constp)?4u:0u,2,P->prog.nplan>1?2u:0u};
            for(unsigned slot=0;slot<5;++slot){
                unsigned width=widths[slot];if(!width)continue;const gl2_attribute *a=&c->attr[slot];
                if((active&(1u<<slot))&&a->enabled&&a->type==GL_FLOAT&&a->size>=width&&!(a->stride&3u)){
                    const uint8_t *last=attribute_at(c,slot,maximum);if(!last)return -1;
                    const uint8_t *first=attribute_at(c,slot,indices[0]);if(!first)return -1;
                    if(!((uintptr_t)first&3u)){views[slot].data=(const float *)first;views[slot].stride=a->stride/4u;continue;}
                }
                if(gather_attribute(c,slot,active,indices,(unsigned)count,width,views[slot].stride,(float *)views[slot].data,maximum))return -1;
            }
            const void *source=0;uint32_t words=c->submit_parts?gl2_native_vertex_words(&P->prog,views,&source):0;
            if(words){
                int header=gl2_build_draw_header_mask(&P->prog,&c->st,(int)mode,c->tex,c->vp_w,c->vp_h,c->seg,c->seg_cap,c->color_mask);
                if(header<2||submit_draw_packet(c,c->seg,(uint32_t)header,(uint32_t)header,source,words*(uint32_t)count)){err(c,GL_INVALID_OPERATION);return -1;}
                return 0;
            }
            int n=gl2_build_draw_views_mask(&P->prog,&c->st,(int)mode,(uint32_t)count,views,c->tex,c->vp_w,c->vp_h,c->seg,c->seg_cap,c->color_mask);
            if(submit_draw_seg(c,n,&P->prog,count)){err(c,GL_INVALID_OPERATION);return -1;}
            return 0;
        }
    }
    if(gather_attribute(c,0,active,indices,(unsigned)count,4,4,pos,maximum))return -1;
    if(P->prog.normal_input&&gather_attribute(c,1,active,indices,(unsigned)count,P->prog.normal_input,4,normals,maximum))return -1;
    if(!P->prog.omit_colour&&gather_attribute(c,2,active,indices,(unsigned)count,4,4,col,maximum))return -1;
    if(gather_attribute(c,3,active,indices,(unsigned)count,2,2,uv0,maximum)||gather_attribute(c,4,active,indices,(unsigned)count,2,2,uv1,maximum))return -1;
    if(c->soft_target){gl2_invalidate_state_cache(c);if(s_draw(c,P,mode,(unsigned)count,pos,normals,col,uv0,uv1)){err(c,GL_INVALID_OPERATION);return -1;}++c->soft_draws;return 0;}
    int n = gl2_build_draw_inputs_mask(&P->prog, &c->st, (int)mode, (uint32_t)count, pos, normals, col, uv0,
                         uv1, c->tex, c->vp_w, c->vp_h, c->seg, c->seg_cap, c->color_mask);
    if (submit_draw_seg(c, n, &P->prog, count)) {err(c, GL_INVALID_OPERATION);return -1;}
    return 0;
}
static gl2_prog_obj *draw_check(gl2_ctx *c, uint32_t mode, int count) {
    if (c->fbo_active && glCheckFramebufferStatus(c, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { err(c, GL_INVALID_FRAMEBUFFER_OPERATION); return 0; }
    if (mode < GL_TRIANGLES || mode > GL_TRIANGLE_FAN) { err(c, GL_INVALID_ENUM); return 0; }
    if (count < 0) { err(c, GL_INVALID_VALUE); return 0; }
    gl2_prog_obj *P = cur(c); if (!P || !count) return 0;
    return P;
}
/* Large GL draws keep bounded gather scratch. Validate the complete range
 * before any packet is submitted, then split only on topology-safe boundaries.
 * The native float path consumes client memory immediately in larger packets. */
static uint32_t draw_index(const uint8_t *src,uint32_t type,uint32_t first,uint32_t i){
    if(!src)return first+i;
    if(type==GL_UNSIGNED_BYTE)return src[i];
    return (uint32_t)src[2u*i]|(uint32_t)src[2u*i+1]<<8;
}
#define GL2_NATIVE_BATCH_VERTICES 4096u
static int draw_direct_range(gl2_ctx *c,gl2_prog_obj *p,uint32_t mode,uint32_t first,uint32_t count,uint32_t active){
    if(c->soft_target||!c->submit_parts||!p->prog.omit_colour||(mode==GL_TRIANGLE_FAN&&count>GL2_NATIVE_BATCH_VERTICES))return 0;
    gl2_vertex_view views[5]={{0}};
    unsigned widths[5]={4,p->prog.normal_input,0,2,p->prog.nplan>1?2u:0u};
    for(unsigned slot=0;slot<5;++slot){
        if(!widths[slot])continue;
        const gl2_attribute *a=&c->attr[slot];
        if(!(active&(1u<<slot))||!a->enabled||a->type!=GL_FLOAT||a->size<widths[slot]||(a->stride&3u))return 0;
        const uint8_t *ptr=attribute_at(c,slot,first);if(!ptr)return -1;
        if((uintptr_t)ptr&3u)return 0;
        views[slot].data=(const float *)ptr;views[slot].stride=a->stride/4u;
    }
    const void *source=0;uint32_t words=gl2_native_vertex_words(&p->prog,views,&source);if(!words)return 0;
    int header=gl2_build_draw_header_mask(&p->prog,&c->st,(int)mode,c->tex,c->vp_w,c->vp_h,c->seg,c->seg_cap,c->color_mask);
    if(header<2){err(c,GL_INVALID_OPERATION);return -1;}
    for(uint32_t offset=0;offset<count;){
        uint32_t n=count-offset;if(n>GL2_NATIVE_BATCH_VERTICES)n=mode==GL_TRIANGLES?GL2_NATIVE_BATCH_VERTICES/3u*3u:GL2_NATIVE_BATCH_VERTICES;
        if(submit_draw_packet(c,c->seg,(uint32_t)header,(uint32_t)header,(const uint32_t *)source+(uintptr_t)offset*words,words*n)){err(c,GL_INVALID_OPERATION);return -1;}
        if(n==count-offset)break;
        offset+=mode==GL_TRIANGLE_STRIP?n-2u:n;
    }
    return 1;
}
static void draw_stream(gl2_ctx *c,gl2_prog_obj *p,uint32_t mode,uint32_t count,const uint8_t *src,uint32_t type,uint32_t first){
    uint32_t gathered[256];
    if(count<=256u){for(uint32_t i=0;i<count;++i)gathered[i]=draw_index(src,type,first,i);(void)draw_gather(c,p,mode,(int)count,gathered);return;}
    if(!c->soft_target&&(!p->fs->nplan||(!c->diagnostic_gpu&&gl2_program_needs_software(c,p)))){err(c,GL_INVALID_OPERATION);return;}
    if(gl2_resolve_texture_units(c,p->prog.nplan?p->prog.nplan:1,p->fs->nsampler,p->sampler_unit))return;
    uint32_t maximum=first+count-1u,active=0,sequential=1,base=first;
    if(src){maximum=0;base=draw_index(src,type,0,0);for(uint32_t i=0;i<count;++i){uint32_t ix=draw_index(src,type,0,i);if(ix>maximum)maximum=ix;if(ix!=base+i)sequential=0;}}
    for(uint32_t i=0;i<p->vs.nattr;++i)active|=1u<<p->vs.attr_slot[i];
    for(unsigned slot=0;slot<5;++slot)if((active&(1u<<slot))&&c->attr[slot].enabled&&!attribute_at(c,slot,maximum))return;
    if(sequential&&draw_direct_range(c,p,mode,base,count,active))return;
    uint32_t limit=256u;
    if(!c->soft_target){
        int header=gl2_build_draw_header_mask(&p->prog,&c->st,(int)mode,c->tex,c->vp_w,c->vp_h,c->seg,c->seg_cap,c->color_mask);
        uint32_t words=6u+p->prog.normal_input+(p->prog.omit_colour?0u:1u)+(p->prog.nplan>1?2u:0u);
        if(header<2||c->seg_cap-(uint32_t)header<3u*words){err(c,GL_INVALID_OPERATION);return;}
        uint32_t fit=(c->seg_cap-(uint32_t)header)/words;if(fit<limit)limit=fit;
    }
    if(limit<4u&&mode!=GL_TRIANGLES){
        for(uint32_t tri=0;tri+2u<count;++tri){
            uint32_t a=mode==GL_TRIANGLE_FAN?0u:tri,b=tri+1u;
            if(mode==GL_TRIANGLE_STRIP&&(tri&1u)){uint32_t t=a;a=b;b=t;}
            gathered[0]=draw_index(src,type,first,a);gathered[1]=draw_index(src,type,first,b);gathered[2]=draw_index(src,type,first,tri+2u);
            if(draw_gather(c,p,GL_TRIANGLES,3,gathered))return;
        }
        return;
    }
    if(mode==GL_TRIANGLES)limit=limit/3u*3u;
    if(mode==GL_TRIANGLE_STRIP)limit&=~1u;
    for(uint32_t offset=0;offset<count;){
        uint32_t n=count-offset;if(n>limit)n=limit;
        if(mode==GL_TRIANGLE_FAN){gathered[0]=draw_index(src,type,first,0);for(uint32_t i=1;i<n;++i)gathered[i]=draw_index(src,type,first,offset+i);}
        else for(uint32_t i=0;i<n;++i)gathered[i]=draw_index(src,type,first,offset+i);
        if(draw_gather(c,p,mode,(int)n,gathered))return;
        if(n==count-offset)break;
        offset+=mode==GL_TRIANGLES?n:n-2u;
    }
}
void glDrawArrays(gl2_ctx *c, uint32_t mode, int first, int count) {
    if (first < 0) { err(c, GL_INVALID_VALUE); return; }
    gl2_prog_obj *p = draw_check(c, mode, count); if (!p) return;
    draw_stream(c,p,mode,(uint32_t)count,0,0,(uint32_t)first);
}
void glDrawElements(gl2_ctx *c, uint32_t mode, int count, uint32_t type, const void *indices) {
    if (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT) { err(c, GL_INVALID_ENUM); return; }
    gl2_prog_obj *p = draw_check(c, mode, count); if (!p) return;
    uint32_t bytes = (uint32_t)count * (type == GL_UNSIGNED_BYTE ? 1u : 2u);
    uintptr_t off = (uintptr_t)indices; const uint8_t *src;
    if (c->element_buffer) {
        gl2_buffer *b = buffer_named(c, c->element_buffer);
        if (!b || !b->used || off > b->size || bytes > b->size - off) { err(c, GL_INVALID_OPERATION); return; }
        src = b->data + off;
    } else {
        if (!indices || off > UINTPTR_MAX - bytes) { err(c, GL_INVALID_OPERATION); return; }
        src = indices;
    }
    draw_stream(c,p,mode,(uint32_t)count,src,type,0);
}
/* ---------------------------------------------------------------- queries */
static unsigned query_int(gl2_ctx *c, uint32_t pname, int *v) {
    switch (pname) {
    case GL_MAX_TEXTURE_SIZE: v[0] = GL2_MAX_TEXTURE_SIZE; return 1;
    case GL_MAX_CUBE_MAP_TEXTURE_SIZE: v[0] = 0; return 1;
    case GL_MAX_TEXTURE_IMAGE_UNITS: v[0] = 2; return 1;
    case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: v[0] = 2; return 1;
    case GL_MAX_VERTEX_ATTRIBS: v[0] = 5; return 1;
    case GL_MAX_VERTEX_UNIFORM_VECTORS: v[0] = VSC_MAX_UNI; return 1;
    case GL_MAX_VARYING_VECTORS: v[0] = 4; return 1;
    case GL_MAX_FRAGMENT_UNIFORM_VECTORS: v[0] = 16; return 1;
    case GL_MAX_RENDERBUFFER_SIZE: v[0] = 0; return 1;
    case GL_SUBPIXEL_BITS: v[0] = 4; return 1;
    case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: case GL_ALPHA_BITS: v[0] = 8; return 1;
    case GL_DEPTH_BITS: v[0] = 16; return 1;
    case GL_STENCIL_BITS: v[0] = 0; return 1;
    case GL_MAX_VIEWPORT_DIMS: v[0] = 240; v[1] = 432; return 2;
    case GL_VIEWPORT: v[0] = 0; v[1] = 0; v[2] = (int)c->vp_w; v[3] = (int)c->vp_h; return 4;
    case GL_ACTIVE_TEXTURE: v[0] = GL_TEXTURE0 + (int)c->active_texture; return 1;
    case GL_CURRENT_PROGRAM: v[0] = (int)c->cur; return 1;
    case GL_ARRAY_BUFFER_BINDING: v[0] = (int)c->array_buffer; return 1;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING: v[0] = (int)c->element_buffer; return 1;
    case GL_TEXTURE_BINDING_2D: v[0] = (int)c->texture_binding[c->active_texture]; return 1;
    case GL_BLEND: v[0] = c->st.blend != 0; return 1;
    case GL_DEPTH_TEST: v[0] = c->st.depth_test != 0; return 1;
    case GL_CULL_FACE: v[0] = c->cull_enabled != 0; return 1;
    case GL_CULL_FACE_MODE: v[0] = (int)c->cull_face; return 1;
    case GL_FRONT_FACE: v[0] = (int)c->front_face; return 1;
    case GL_DEPTH_FUNC: v[0] = (int)c->st.depth_func; return 1;
    case GL_DEPTH_WRITEMASK: v[0] = c->st.depth_write != 0; return 1;
    case GL_UNPACK_ALIGNMENT: v[0] = (int)c->unpack_alignment; return 1;
    case GL_PACK_ALIGNMENT: v[0] = (int)c->pack_alignment; return 1;
    case GL_SCISSOR_BOX: v[0]=c->scissor_box[0];v[1]=c->scissor_box[1];v[2]=c->scissor_box[2];v[3]=c->scissor_box[3];return 4;
    case GL_SCISSOR_TEST: v[0]=c->scissor_enabled!=0;return 1;
    case GL_COLOR_WRITEMASK: for(unsigned i=0;i<4;++i)v[i]=(c->color_mask>>i)&1u;return 4;
    case GL_FRAMEBUFFER_BINDING: v[0] = (int)c->framebuffer_binding; return 1;
    case GL_RENDERBUFFER_BINDING: v[0] = (int)c->renderbuffer_binding; return 1;
    default: err(c, GL_INVALID_ENUM); return 0;
    }
}
const char *glGetString(gl2_ctx *c, uint32_t name) {
    switch (name) {
    case GL_VENDOR: return "NanoOS";
    case GL_RENDERER: return "PowerVR MBX Lite (NanoOS GL2 prototype)";
    case GL_VERSION: return "OpenGL ES 2.0 (NanoOS prototype subset)";
    case GL_EXTENSIONS: return "";
    case GL_SHADING_LANGUAGE_VERSION: return "OpenGL ES GLSL ES 1.00 (bounded subset)";
    default: err(c, GL_INVALID_ENUM); return 0;
    }
}
void glGetIntegerv(gl2_ctx *c, uint32_t pname, int *params) {
    if (!params) { err(c, GL_INVALID_VALUE); return; }
    int v[4]; unsigned n = query_int(c, pname, v);
    for (unsigned i = 0; i < n; ++i) params[i] = v[i];
}
void glGetBooleanv(gl2_ctx *c, uint32_t pname, unsigned char *params) {
    if (!params) { err(c, GL_INVALID_VALUE); return; }
    int v[4]; unsigned n = query_int(c, pname, v);
    for (unsigned i = 0; i < n; ++i) params[i] = v[i] != 0;
}
void glGetFloatv(gl2_ctx *c, uint32_t pname, float *params) {
    if (!params) { err(c, GL_INVALID_VALUE); return; }
    if (pname == GL_COLOR_CLEAR_VALUE) { for (unsigned i=0;i<4;++i) params[i]=c->clear_color[i]; return; }
    if (pname == GL_DEPTH_CLEAR_VALUE) { params[0]=c->clear_depth; return; }
    int v[4]; unsigned n = query_int(c, pname, v);
    for (unsigned i = 0; i < n; ++i) params[i] = (float)v[i];
}
void glGetShaderPrecisionFormat(gl2_ctx *c, uint32_t shadertype, uint32_t precisiontype, int *range, int *precision) {
    if (!range || !precision) { err(c, GL_INVALID_VALUE); return; }
    if (shadertype != GL_VERTEX_SHADER && shadertype != GL_FRAGMENT_SHADER) { err(c, GL_INVALID_ENUM); return; }
    if (precisiontype < GL_LOW_FLOAT || precisiontype > GL_HIGH_INT) { err(c, GL_INVALID_ENUM); return; }
    int frag = shadertype == GL_FRAGMENT_SHADER;
    int high = precisiontype == GL_HIGH_FLOAT || precisiontype == GL_HIGH_INT;
    int integer = precisiontype >= GL_LOW_INT;
    range[0] = range[1] = 0; precision[0] = 0;
    if (frag && high) return;                 /* fragment highp is not advertised */
    if (integer) { range[0] = range[1] = 31; return; }
    if (precisiontype == GL_MEDIUM_FLOAT) { range[0] = range[1] = 15; precision[0] = 10; }
    else if (precisiontype == GL_LOW_FLOAT) { range[0] = range[1] = 7; precision[0] = 8; }
    else { range[0] = range[1] = 127; precision[0] = 23; }
}
void glValidateProgram(gl2_ctx *c, uint32_t program) {
    gl2_prog_obj *P = prp(c, program); if (!P) return;
    P->validate_status = (P->ok && P->executable && P->vsh && P->fsh) ? 1u : 0u;
    scpy(P->log, P->validate_status ? "validate: ok" : "validate: program is not linked", 64);
}
void glFlush(gl2_ctx *c) { (void)c; }
void glFinish(gl2_ctx *c) { (void)c; }

/* ---------------------------------------------------- framebuffer objects */
static gl2_framebuffer *fb_named(gl2_ctx *c, uint32_t name) {
    if (!name) return 0;
    for (uint32_t i = 0; i < GL2_MAX_FRAMEBUFFERS; ++i)
        if (c->framebuffers[i].reserved && c->framebuffers[i].name == name) return &c->framebuffers[i];
    return 0;
}
static gl2_renderbuffer *rb_named(gl2_ctx *c, uint32_t name) {
    if (!name) return 0;
    for (uint32_t i = 0; i < GL2_MAX_RENDERBUFFERS; ++i)
        if (c->renderbuffers[i].reserved && c->renderbuffers[i].name == name) return &c->renderbuffers[i];
    return 0;
}
static gl2_texture_obj *fb_texture(gl2_ctx *c, uint32_t name) {
    if (!name) return 0;
    for (uint32_t i = 0; i < GL2_MAX_TEXTURES; ++i)
        if (c->textures[i].reserved && c->textures[i].name == name) return &c->textures[i];
    return 0;
}
static uint32_t fb_complete(gl2_ctx *c, gl2_framebuffer *f, int *w, int *h) {
    if (!f) return GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
    gl2_texture_obj *t = fb_texture(c, f->color_texture);
    if (!f->color_texture) return GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
    if (!t || !t->used || !t->defined) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
    uint32_t n = t->size;
    if (n < 16 || n > GL2_MAX_TEXTURE_SIZE || (n & (n - 1u)) || !c->texture_cpu ||
        (t->format != GL_RGBA && t->format != GL_RGB) || t->storage != GL2_TEX_ARGB8888)
        return GL_FRAMEBUFFER_UNSUPPORTED;
    if (f->stencil_renderbuffer) return GL_FRAMEBUFFER_UNSUPPORTED;
    if (f->depth_renderbuffer) {
        gl2_renderbuffer *r = rb_named(c, f->depth_renderbuffer);
        if (!r || !r->used || !r->defined) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        if (r->width != n || r->height != n) return GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS;
        if (r->format != GL_DEPTH_COMPONENT16) return GL_FRAMEBUFFER_UNSUPPORTED;
    }
    if (w) *w = (int)n;
    if (h) *h = (int)n;
    return GL_FRAMEBUFFER_COMPLETE;
}
static void fb_unbind(gl2_ctx *c) {
    if (!c->fbo_active) return;
    gl2_framebuffer *f = fb_named(c, c->fbo_active);
    gl2_texture_obj *t = f ? fb_texture(c, f->color_texture) : 0;
    /* Only copy the software scratch if it was actually the active raster
     * target. A native adapter begins its own GPU target and leaves
     * soft_target clear; that path updates the texture itself. */
    int software_target = c->soft_target == c->fbo_color;
    if (software_target && t && t->defined && c->texture_cpu && t->size >= 16 && t->size <= GL2_MAX_TEXTURE_SIZE)
        (void)gl2_texture_store_argb(c, t, 0, 0, (int)t->size, (int)t->size, c->fbo_color, t->size, t->size, t->size);
    c->soft_target = c->fbo_saved.target; c->soft_depth = c->fbo_saved.depth;
    c->soft_width = c->fbo_saved.width; c->soft_height = c->fbo_saved.height;
    c->soft_stride = c->fbo_saved.stride; c->soft_depth_initialized = c->fbo_saved.depth_initialized;
    c->fbo_active = 0;
}
void glGenFramebuffers(gl2_ctx *c, int n, uint32_t *names) {
    if (n < 0 || (n && !names)) { err(c, GL_INVALID_VALUE); return; }
    uint32_t free_slots = 0;
    for (uint32_t i = 0; i < GL2_MAX_FRAMEBUFFERS; ++i) free_slots += !c->framebuffers[i].reserved;
    if ((uint32_t)n > free_slots) { err(c, GL_OUT_OF_MEMORY); return; }
    for (int k = 0; k < n; ++k) {
        while (!c->next_framebuffer || fb_named(c, c->next_framebuffer)) ++c->next_framebuffer;
        for (uint32_t i = 0; i < GL2_MAX_FRAMEBUFFERS; ++i) if (!c->framebuffers[i].reserved) {
            gl2_framebuffer z = {0}; z.name = c->next_framebuffer++; z.reserved = 1; z.used = 1;
            c->framebuffers[i] = z; names[k] = z.name; break;
        }
    }
}
void glBindFramebuffer(gl2_ctx *c, uint32_t target, uint32_t name) {
    if (target != GL_FRAMEBUFFER) { err(c, GL_INVALID_ENUM); return; }
    if (name) {
        gl2_framebuffer *f = fb_named(c, name);
        if (!f) for (uint32_t i = 0; i < GL2_MAX_FRAMEBUFFERS; ++i) if (!c->framebuffers[i].reserved) {
            gl2_framebuffer z = {0}; z.name = name; z.reserved = 1; c->framebuffers[i] = z; f = &c->framebuffers[i]; break;
        }
        if (!f) { err(c, GL_OUT_OF_MEMORY); return; }
        f->used = 1;
        if (name == c->framebuffer_binding) return;
    }
    fb_unbind(c);
    c->framebuffer_binding = name;
    if (!name) return;
    gl2_framebuffer *f = fb_named(c, name);
    int w = 0, h = 0; (void)fb_complete(c, f, &w, &h);
    c->fbo_saved.target = c->soft_target; c->fbo_saved.depth = c->soft_depth;
    c->fbo_saved.width = c->soft_width; c->fbo_saved.height = c->soft_height;
    c->fbo_saved.stride = c->soft_stride; c->fbo_saved.depth_initialized = c->soft_depth_initialized;
    c->fbo_active = name;
    c->soft_target = c->fbo_color; c->soft_depth = c->fbo_depth;
    c->soft_width = (uint32_t)w; c->soft_height = (uint32_t)h; c->soft_stride = (uint32_t)w;
    c->soft_depth_initialized = 0;
    if (w > 0) {
        for (int i = 0; i < w * h; ++i) c->fbo_color[i] = 0;
        for (int i = 0; i < w * h; ++i) c->fbo_depth[i] = 1.f;
    }
}
void glDeleteFramebuffers(gl2_ctx *c, int n, const uint32_t *names) {
    if (n < 0 || (n && !names)) { err(c, GL_INVALID_VALUE); return; }
    for (int k = 0; k < n; ++k) {
        gl2_framebuffer *f = fb_named(c, names[k]); if (!f) continue;
        if (c->framebuffer_binding == names[k]) { fb_unbind(c); c->framebuffer_binding = 0; }
        f->reserved = f->used = f->color_texture = f->depth_renderbuffer = f->stencil_renderbuffer = 0;
    }
}
int glIsFramebuffer(gl2_ctx *c, uint32_t name) { gl2_framebuffer *f = fb_named(c, name); return f && f->used; }
uint32_t glCheckFramebufferStatus(gl2_ctx *c, uint32_t target) {
    if (target != GL_FRAMEBUFFER) { err(c, GL_INVALID_ENUM); return 0; }
    if (!c->framebuffer_binding) return GL_FRAMEBUFFER_COMPLETE;
    return fb_complete(c, fb_named(c, c->framebuffer_binding), 0, 0);
}
void glFramebufferTexture2D(gl2_ctx *c, uint32_t target, uint32_t attachment, uint32_t textarget, uint32_t texture, int level) {
    if (target != GL_FRAMEBUFFER || textarget != GL_TEXTURE_2D) { err(c, GL_INVALID_ENUM); return; }
    if (attachment != GL_COLOR_ATTACHMENT0) { err(c, GL_INVALID_OPERATION); return; }
    if (level != 0 || level < 0) { err(c, GL_INVALID_VALUE); return; }
    gl2_framebuffer *f = fb_named(c, c->framebuffer_binding);
    if (!f) { err(c, GL_INVALID_OPERATION); return; }
    if (texture) {
        gl2_texture_obj *t = fb_texture(c, texture);
        if (!t || !t->used) { err(c, GL_INVALID_OPERATION); return; }
    }
    int active = c->fbo_active == f->name;
    if (active) { fb_unbind(c); c->framebuffer_binding = 0; }
    f->color_texture = texture;
    if (active) glBindFramebuffer(c, GL_FRAMEBUFFER, f->name);
}
void glFramebufferRenderbuffer(gl2_ctx *c, uint32_t target, uint32_t attachment, uint32_t rbtarget, uint32_t renderbuffer) {
    if (target != GL_FRAMEBUFFER || rbtarget != GL_RENDERBUFFER) { err(c, GL_INVALID_ENUM); return; }
    if (attachment != GL_DEPTH_ATTACHMENT && attachment != GL_STENCIL_ATTACHMENT) { err(c, GL_INVALID_ENUM); return; }
    gl2_framebuffer *f = fb_named(c, c->framebuffer_binding);
    if (!f) { err(c, GL_INVALID_OPERATION); return; }
    if (renderbuffer) {
        gl2_renderbuffer *r = rb_named(c, renderbuffer);
        if (!r || !r->used) { err(c, GL_INVALID_OPERATION); return; }
    }
    int active = c->fbo_active == f->name;
    if (active) { fb_unbind(c); c->framebuffer_binding = 0; }
    if (attachment == GL_DEPTH_ATTACHMENT) f->depth_renderbuffer = renderbuffer;
    else f->stencil_renderbuffer = renderbuffer;
    if (active) glBindFramebuffer(c, GL_FRAMEBUFFER, f->name);
}
void glGetFramebufferAttachmentParameteriv(gl2_ctx *c, uint32_t target, uint32_t attachment, uint32_t pname, int *params) {
    if (!params) { err(c, GL_INVALID_VALUE); return; }
    if (target != GL_FRAMEBUFFER) { err(c, GL_INVALID_ENUM); return; }
    if (attachment != GL_COLOR_ATTACHMENT0 && attachment != GL_DEPTH_ATTACHMENT && attachment != GL_STENCIL_ATTACHMENT) { err(c, GL_INVALID_ENUM); return; }
    gl2_framebuffer *f = fb_named(c, c->framebuffer_binding);
    uint32_t object = attachment == GL_COLOR_ATTACHMENT0 ? (f ? f->color_texture : 0) :
                      attachment == GL_DEPTH_ATTACHMENT ? (f ? f->depth_renderbuffer : 0) : (f ? f->stencil_renderbuffer : 0);
    uint32_t type = !object ? GL_NONE : (attachment == GL_COLOR_ATTACHMENT0 ? GL_TEXTURE : GL_RENDERBUFFER);
    if (pname == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE) *params = (int)type;
    else if (pname == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME) *params = (int)object;
    else if (pname == GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL) *params = 0;
    else if (pname == GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE) *params = 0;
    else err(c, GL_INVALID_ENUM);
}
void glGenRenderbuffers(gl2_ctx *c, int n, uint32_t *names) {
    if (n < 0 || (n && !names)) { err(c, GL_INVALID_VALUE); return; }
    uint32_t free_slots = 0;
    for (uint32_t i = 0; i < GL2_MAX_RENDERBUFFERS; ++i) free_slots += !c->renderbuffers[i].reserved;
    if ((uint32_t)n > free_slots) { err(c, GL_OUT_OF_MEMORY); return; }
    for (int k = 0; k < n; ++k) {
        while (!c->next_renderbuffer || rb_named(c, c->next_renderbuffer)) ++c->next_renderbuffer;
        for (uint32_t i = 0; i < GL2_MAX_RENDERBUFFERS; ++i) if (!c->renderbuffers[i].reserved) {
            gl2_renderbuffer z = {0}; z.name = c->next_renderbuffer++; z.reserved = 1; z.used = 1;
            c->renderbuffers[i] = z; names[k] = z.name; break;
        }
    }
}
void glBindRenderbuffer(gl2_ctx *c, uint32_t target, uint32_t name) {
    if (target != GL_RENDERBUFFER) { err(c, GL_INVALID_ENUM); return; }
    if (name) {
        gl2_renderbuffer *r = rb_named(c, name);
        if (!r) for (uint32_t i = 0; i < GL2_MAX_RENDERBUFFERS; ++i) if (!c->renderbuffers[i].reserved) {
            gl2_renderbuffer z = {0}; z.name = name; z.reserved = 1; c->renderbuffers[i] = z; r = &c->renderbuffers[i]; break;
        }
        if (!r) { err(c, GL_OUT_OF_MEMORY); return; }
        r->used = 1;
    }
    c->renderbuffer_binding = name;
}
void glDeleteRenderbuffers(gl2_ctx *c, int n, const uint32_t *names) {
    if (n < 0 || (n && !names)) { err(c, GL_INVALID_VALUE); return; }
    for (int k = 0; k < n; ++k) {
        gl2_renderbuffer *r = rb_named(c, names[k]); if (!r) continue;
        if (c->renderbuffer_binding == names[k]) c->renderbuffer_binding = 0;
        r->reserved = r->used = r->defined = r->width = r->height = r->format = 0;
    }
}
int glIsRenderbuffer(gl2_ctx *c, uint32_t name) { gl2_renderbuffer *r = rb_named(c, name); return r && r->used; }
void glRenderbufferStorage(gl2_ctx *c, uint32_t target, uint32_t internalformat, int width, int height) {
    if (target != GL_RENDERBUFFER) { err(c, GL_INVALID_ENUM); return; }
    if (internalformat != GL_DEPTH_COMPONENT16) { err(c, GL_INVALID_ENUM); return; }
    if (width <= 0 || height <= 0 || width > GL2_MAX_TEXTURE_SIZE || height > GL2_MAX_TEXTURE_SIZE) { err(c, GL_INVALID_VALUE); return; }
    gl2_renderbuffer *r = rb_named(c, c->renderbuffer_binding);
    if (!r) { err(c, GL_INVALID_OPERATION); return; }
    r->width = (uint32_t)width; r->height = (uint32_t)height; r->format = internalformat; r->defined = 1;
}
void glGetRenderbufferParameteriv(gl2_ctx *c, uint32_t target, uint32_t pname, int *params) {
    if (!params) { err(c, GL_INVALID_VALUE); return; }
    if (target != GL_RENDERBUFFER) { err(c, GL_INVALID_ENUM); return; }
    gl2_renderbuffer *r = rb_named(c, c->renderbuffer_binding);
    if (!r || !r->defined) { err(c, GL_INVALID_OPERATION); return; }
    if (pname == GL_RENDERBUFFER_WIDTH) *params = (int)r->width;
    else if (pname == GL_RENDERBUFFER_HEIGHT) *params = (int)r->height;
    else if (pname == GL_RENDERBUFFER_INTERNAL_FORMAT) *params = (int)r->format;
    else err(c, GL_INVALID_ENUM);
}
/* ---------------------------------------------------------- clear state */
static uint32_t clear_byte(float v){if(!(v>0))return 0;if(v>=1)return 255;return(uint32_t)(v*255.f+.5f);}
void glClearColor(gl2_ctx *c,float r,float g,float b,float a){c->clear_color[0]=r;c->clear_color[1]=g;c->clear_color[2]=b;c->clear_color[3]=a;}
void glClearDepthf(gl2_ctx *c,float depth){c->clear_depth=depth<0?0:depth>1?1:depth;}
void glClearStencil(gl2_ctx *c,int stencil){c->clear_stencil=(uint32_t)stencil;}
void glColorMask(gl2_ctx *c,int r,int g,int b,int a){c->color_mask=(r?1u:0u)|(g?2u:0u)|(b?4u:0u)|(a?8u:0u);gl2_invalidate_state_cache(c);}
void glScissor(gl2_ctx *c,int x,int y,int width,int height){if(width<0||height<0){err(c,GL_INVALID_VALUE);return;}c->scissor_box[0]=x;c->scissor_box[1]=y;c->scissor_box[2]=width;c->scissor_box[3]=height;gl2_invalidate_state_cache(c);}
void glClear(gl2_ctx *c,uint32_t mask){
    if(mask&~(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT)){err(c,GL_INVALID_VALUE);return;}
    mask&=~(uint32_t)GL_STENCIL_BUFFER_BIT;               /* no stencil buffer */
    uint32_t bits=mask&(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    if(!bits)return;
    uint32_t argb=(clear_byte(c->clear_color[3])<<24)|(clear_byte(c->clear_color[0])<<16)|
                   (clear_byte(c->clear_color[1])<<8)|clear_byte(c->clear_color[2]);
    if(c->soft_target){
        uint32_t w=c->soft_width,h=c->soft_height,stride=c->soft_stride;
        int x0=0,y0=0,x1=(int)w,y1=(int)h;
        if(c->scissor_enabled){
            int gx0=c->scissor_box[0],gy0=c->scissor_box[1],gx1=gx0+c->scissor_box[2],gy1=gy0+c->scissor_box[3];
            if(gx0<0)gx0=0;
            if(gy0<0)gy0=0;
            if(gx1>(int)w)gx1=(int)w;
            if(gy1>(int)h)gy1=(int)h;
            x0=gx0;x1=gx1;y0=(int)h-gy1;y1=(int)h-gy0;
            if(x1<=x0||y1<=y0)return;
        }
        uint32_t cmask=0;
        if(c->color_mask&1)cmask|=0x00ff0000u;
        if(c->color_mask&2)cmask|=0x0000ff00u;
        if(c->color_mask&4)cmask|=0x000000ffu;
        if(c->color_mask&8)cmask|=0xff000000u;
        for(int y=y0;y<y1;++y){
            uint32_t *row=c->soft_target+(uint32_t)y*stride;
            if(mask&GL_COLOR_BUFFER_BIT)
                for(int x=x0;x<x1;++x)row[x]=(row[x]&~cmask)|(argb&cmask);
            if((mask&GL_DEPTH_BUFFER_BIT)&&c->st.depth_write)
                for(int x=x0;x<x1;++x)c->soft_depth[(uint32_t)y*w+(uint32_t)x]=c->clear_depth;
        }
        gl2_soft_cache_flush();
        return;
    }
    if(!c->clear){err(c,GL_INVALID_OPERATION);return;}
    if(bits!=(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT)||c->color_mask!=0xfu||c->scissor_enabled||c->clear_depth!=1.f){
        err(c,GL_INVALID_OPERATION);return;
    }
    if(c->clear(c,mask,argb,c->clear_depth,c->color_mask,c->scissor_enabled,c->scissor_box,c->clear_user))err(c,GL_INVALID_OPERATION);
}
int gl2_texture_store_name(gl2_ctx *c,uint32_t name,int dx,int dy,int w,int h,const uint32_t *surface,uint32_t sw,uint32_t sh,uint32_t stride){
    gl2_texture_obj *t=fb_texture(c,name);if(!t)return -1;
    return gl2_texture_store_argb(c,t,dx,dy,w,h,surface,sw,sh,stride);
}
int gl2_fbo_target(gl2_ctx *c,uint32_t *texture,uint32_t *width,uint32_t *height){
    if(!c||!texture||!width||!height)return -1;
    gl2_framebuffer *f=fb_named(c,c->fbo_active);if(!f)return -2;
    int w=0,h=0;if(fb_complete(c,f,&w,&h)!=GL_FRAMEBUFFER_COMPLETE)return -3;
    gl2_texture_obj *t=fb_texture(c,f->color_texture);if(!t)return -4;
    *texture=f->color_texture;*width=(uint32_t)w;*height=(uint32_t)h;return 0;
}
void glReadPixels(gl2_ctx *c,int x,int y,int width,int height,uint32_t format,uint32_t type,void *pixels){
    if(width<0||height<0||(!pixels&&width&&height)){err(c,GL_INVALID_VALUE);return;}
    if(format!=GL_RGBA&&format!=GL_RGB){err(c,GL_INVALID_ENUM);return;}
    if(type!=GL_UNSIGNED_BYTE){err(c,GL_INVALID_ENUM);return;}
    if(!width||!height)return;
    uint32_t bpp=format==GL_RGBA?4u:3u,pack=c->pack_alignment?c->pack_alignment:4u;
    uint32_t pitch=(uint32_t)width*bpp;if(pitch&(pack-1u))pitch=(pitch+pack-1u)&~(pack-1u);
    if(c->soft_target){
        if(x<0||y<0||(uint32_t)x>c->soft_width||(uint32_t)y>c->soft_height||
           (uint32_t)width>c->soft_width-(uint32_t)x||(uint32_t)height>c->soft_height-(uint32_t)y){err(c,GL_INVALID_VALUE);return;}
        uint8_t *out=(uint8_t *)pixels;
        for(int gy=0;gy<height;++gy){
            const uint32_t *src=c->soft_target+(uint32_t)(c->soft_height-1u-(uint32_t)(y+gy))*c->soft_stride+(uint32_t)x;
            for(int gx=0;gx<width;++gx){uint32_t v=src[gx];out[0]=(uint8_t)(v>>16);out[1]=(uint8_t)(v>>8);out[2]=(uint8_t)v;if(bpp==4)out[3]=(uint8_t)(v>>24);out+=bpp;}
            if(pitch>(uint32_t)width*bpp)out+=(pitch-(uint32_t)width*bpp);
        }
        return;
    }
    if(c->read_pixels){if(c->read_pixels(c,x,y,width,height,format,type,pack,pixels,c->read_pixels_user))err(c,GL_INVALID_OPERATION);return;}
    err(c,GL_INVALID_OPERATION);
}
void glCopyTexImage2D(gl2_ctx *c,uint32_t target,int level,uint32_t internalformat,int x,int y,int width,int height,int border){
    if(border||width<=0||height<=0||width>GL2_MAX_TEXTURE_SIZE||height>GL2_MAX_TEXTURE_SIZE){err(c,GL_INVALID_VALUE);return;}
    if(internalformat!=GL_RGBA&&internalformat!=GL_RGB){err(c,GL_INVALID_ENUM);return;}
    uint32_t pack=c->pack_alignment,unpack=c->unpack_alignment;
    c->pack_alignment=c->unpack_alignment=4;
    uint32_t before=c->err;
    glReadPixels(c,x,y,width,height,internalformat,GL_UNSIGNED_BYTE,c->copy_pixels);
    if(c->err==before)glTexImage2D(c,target,level,(int)internalformat,width,height,0,internalformat,GL_UNSIGNED_BYTE,c->copy_pixels);
    c->pack_alignment=pack;c->unpack_alignment=unpack;
}
void glCopyTexSubImage2D(gl2_ctx *c,uint32_t target,int level,int xoffset,int yoffset,int x,int y,int width,int height){
    if(width<=0||height<=0||width>GL2_MAX_TEXTURE_SIZE||height>GL2_MAX_TEXTURE_SIZE){err(c,GL_INVALID_VALUE);return;}
    if(target!=GL_TEXTURE_2D){err(c,GL_INVALID_ENUM);return;}
    gl2_texture_obj *t=fb_texture(c,c->texture_binding[c->active_texture]);if(!t||!t->defined){err(c,GL_INVALID_OPERATION);return;}
    if(t->format!=GL_RGBA&&t->format!=GL_RGB){err(c,GL_INVALID_OPERATION);return;}
    uint32_t pack=c->pack_alignment,unpack=c->unpack_alignment;
    c->pack_alignment=c->unpack_alignment=4;
    uint32_t before=c->err;
    glReadPixels(c,x,y,width,height,t->format,GL_UNSIGNED_BYTE,c->copy_pixels);
    if(c->err==before)glTexSubImage2D(c,target,level,xoffset,yoffset,width,height,t->format,GL_UNSIGNED_BYTE,c->copy_pixels);
    c->pack_alignment=pack;c->unpack_alignment=unpack;
}
