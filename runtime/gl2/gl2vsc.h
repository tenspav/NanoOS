/* gl2vsc: GLSL ES 1.00 vertex shader subset -> VGP microcode, freestanding C (no libc, no malloc). Port of glsl2vgp/vsc.py (same hardware rules:
 * one distinct constant register per instruction, BGRA colour output, 14 verified scratch registers).
 * Writes native near/far distances to VGP output5.xy on every position update.
 * Physical runs78/79 validate generated lighting code; run77 isolates unsafe scratch indices14/15. */
#ifndef GL2VSC_H
#define GL2VSC_H
#include <stdint.h>
#define VSC_MAX_INSTR 256
#define VSC_MAX_LIT 64
#define VSC_MAX_UNI 16
typedef struct {
    uint32_t ncode; uint32_t code[VSC_MAX_INSTR][2];
    uint32_t nlit; uint16_t lit_idx[VSC_MAX_LIT]; float lit_val[VSC_MAX_LIT][4];
    uint32_t nuni; char uni_name[VSC_MAX_UNI][32]; uint8_t uni_idx[VSC_MAX_UNI], uni_cnt[VSC_MAX_UNI],uni_width[VSC_MAX_UNI];
    uint32_t nattr; char attr_name[8][32]; uint8_t attr_slot[8], attr_w[8];     /* attribute name -> VGP input slot (0 pos, 1 normal, 2 colour, 3 uv0, 4 uv1) */
    uint32_t nvary; char vary_name[8][32]; uint8_t vary_slot[8], vary_w[8];     /* varying name -> output slot (1 colour, 2 fog, 3 uv0, 4 uv1) */
    uint32_t nconst, ntemps;
    char err[64];
} vsc_result;
int gl2vsc_compile(const char *src, vsc_result *out);   /* 0 ok, <0 error (message in out->err) */
#endif
