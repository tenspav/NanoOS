#ifndef GL2SOFT_H
#define GL2SOFT_H
#include <stdint.h>
#define GL2_SOFT_INSNS 256
#define GL2_SOFT_UNIFORM_BASE 8
#define GL2_SOFT_INPUTS (GL2_SOFT_UNIFORM_BASE+16*4)
/* Scalar SSA: each instruction defines its own index. Sources precede it. */
enum {GL2_SCONST=1,GL2_SINPUT,GL2_SADD,GL2_SMUL,GL2_SSUB,GL2_SCLAMP};
typedef struct {uint16_t a,b;uint8_t op;float value;} gl2_soft_insn;
typedef struct {uint16_t count,output[4];gl2_soft_insn code[GL2_SOFT_INSNS];} gl2_soft_program;
int gl2_soft_is_texture_cube(const gl2_soft_program *);
int gl2_soft_validate(const gl2_soft_program *);
int gl2_soft_fragment_uniforms(const gl2_soft_program *,const float primary[4],const float texel[4],const float uniforms[16][4],float rgba[4]);
int gl2_soft_fragment(const gl2_soft_program *,const float primary[4],const float texel[4],float rgba[4]);
void gl2_soft_cache_flush(void);
#endif
