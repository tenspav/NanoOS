/* freestanding memcpy/memset/memmove/memcmp (gcc may emit calls to these) */
#include <stddef.h>
void *memset(void *d, int c, size_t n){ unsigned char *p=d; while(n--) *p++=(unsigned char)c; return d; }
void *memcpy(void *d, const void *s, size_t n){ unsigned char *a=d; const unsigned char *b=s; while(n--) *a++=*b++; return d; }
void *memmove(void *d, const void *s, size_t n){ unsigned char *a=d; const unsigned char *b=s; if(a<b) while(n--) *a++=*b++; else { a+=n; b+=n; while(n--) *--a=*--b; } return d; }
int memcmp(const void *x, const void *y, size_t n){ const unsigned char *a=x,*b=y; for(;n;n--,a++,b++) if(*a!=*b) return *a-*b; return 0; }
size_t strlen(const char *s){ size_t n=0; while(s[n]) n++; return n; }
