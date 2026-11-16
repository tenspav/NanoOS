/* Freestanding SHA-256 for bounded (<4 GiB) file verification. */
#ifndef N16_SHA256_H
#define N16_SHA256_H
#include <stdint.h>
struct sha256 { uint32_t h[8], bytes, used; unsigned char b[64]; };
static uint32_t rr(uint32_t x,unsigned n){return (x>>n)|(x<<(32-n));}
static void sha_block(struct sha256 *s){
 static const uint32_t k[64]={0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
 uint32_t w[64];
 for(unsigned i=0;i<16;++i)w[i]=(uint32_t)s->b[i*4]<<24|(uint32_t)s->b[i*4+1]<<16|(uint32_t)s->b[i*4+2]<<8|s->b[i*4+3];
 for(unsigned i=16;i<64;++i){uint32_t x=w[i-15],y=w[i-2];w[i]=w[i-16]+(rr(x,7)^rr(x,18)^(x>>3))+w[i-7]+(rr(y,17)^rr(y,19)^(y>>10));}
 uint32_t a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4],f=s->h[5],g=s->h[6],h=s->h[7];
 for(unsigned i=0;i<64;++i){uint32_t t=h+(rr(e,6)^rr(e,11)^rr(e,25))+((e&f)^(~e&g))+k[i]+w[i];uint32_t u=(rr(a,2)^rr(a,13)^rr(a,22))+((a&b)^(a&c)^(b&c));h=g;g=f;f=e;e=d+t;d=c;c=b;b=a;a=t+u;}
 s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}
static void sha_init(struct sha256 *s){static const uint32_t h[8]={0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};for(unsigned i=0;i<8;++i)s->h[i]=h[i];s->bytes=s->used=0;}
static void sha_update(struct sha256 *s,const void *v,uint32_t n){const unsigned char *p=v;s->bytes+=n;while(n--){s->b[s->used++]=*p++;if(s->used==64){sha_block(s);s->used=0;}}}
static void sha_final(struct sha256 *s,unsigned char out[32]){
 uint32_t hi=s->bytes>>29,lo=s->bytes<<3;s->b[s->used++]=128;
 if(s->used>56){while(s->used<64)s->b[s->used++]=0;sha_block(s);s->used=0;}
 while(s->used<56)s->b[s->used++]=0;
 for(unsigned i=0;i<4;++i){s->b[56+i]=(unsigned char)(hi>>(24-i*8));s->b[60+i]=(unsigned char)(lo>>(24-i*8));}
 sha_block(s);for(unsigned i=0;i<32;++i)out[i]=(unsigned char)(s->h[i/4]>>(24-(i%4)*8));
}
#endif
