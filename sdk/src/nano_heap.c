#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "math.h"
#include "ctype.h"

/* ---- Free-list heap across cached CPU-only regions (24 MiB total):
 *   1. MM_GAME_HEAP upper: 0x0a600000..0x0affffff (10 MiB, clear of g_ns_game at 0x0a400000)
 *   2. MM_GAME_HEAP lower: 0x0a000000..0x0a3fffff (4 MiB)
 *   3. MM_GAME tail:       __bss_end..0x0b9fffff  (~5.9 MiB)
 *   4. MM_GAME_HEAP2:      0x0ba20000..0x0be40000 (4.1 MiB, clear of 0x0ba00000/0x0be60000)
 * Per NanoOS/src/memmap.h, report mirrors in MM_GAME_HEAP are only live from
 * rpt_mirror_write() at game exit until the next reset. ---- */
#define HEAP_BYTES (10u * 1024u * 1024u)
typedef struct blk_hdr {
    size_t size;
    struct blk_hdr *next;
    struct blk_hdr *prev;
    uint32_t free_flag;
} blk_hdr_t;

static uint8_t *const heap_mem = (uint8_t *)0x0a600000u;
static blk_hdr_t *heap_head = 0;
static blk_hdr_t *heap_rover = 0;
extern char __bss_end[];

static void heap_init(void) {
    if (heap_head) return;
    heap_head = (blk_hdr_t *)heap_mem;
    heap_head->size = HEAP_BYTES - sizeof(blk_hdr_t);
    heap_head->prev = 0;
    heap_head->free_flag = 1;
    blk_hdr_t *tail = heap_head;

    blk_hdr_t *lo = (blk_hdr_t *)0x0a000000u;
    lo->size = (0x0a400000u - 0x0a000000u) - sizeof(blk_hdr_t);
    lo->prev = tail;
    lo->free_flag = 1;
    tail->next = lo;
    tail = lo;

    uintptr_t hi_start = ((uintptr_t)__bss_end + 4095u) & ~(uintptr_t)4095u;
    uintptr_t hi_end = 0x0ba00000u;
    if (hi_start + sizeof(blk_hdr_t) + 1024u < hi_end) {
        blk_hdr_t *hi = (blk_hdr_t *)hi_start;
        hi->size = (size_t)(hi_end - hi_start) - sizeof(blk_hdr_t);
        hi->prev = tail;
        hi->free_flag = 1;
        tail->next = hi;
        tail = hi;
    }

    blk_hdr_t *h2 = (blk_hdr_t *)0x0ba20000u;
    h2->size = (0x0be40000u - 0x0ba20000u) - sizeof(blk_hdr_t);
    h2->prev = tail;
    h2->free_flag = 1;
    h2->next = 0;
    tail->next = h2;
    heap_rover = heap_head;
}

static void *alloc_from(blk_hdr_t *cur, size_t size) {
    if (cur->size >= size + sizeof(blk_hdr_t) + 16u) {
        blk_hdr_t *nxt = (blk_hdr_t *)((uint8_t *)(cur + 1) + size);
        nxt->size = cur->size - size - sizeof(blk_hdr_t);
        nxt->next = cur->next;
        nxt->prev = cur;
        nxt->free_flag = 1;
        if (cur->next) cur->next->prev = nxt;
        cur->size = size;
        cur->next = nxt;
        heap_rover = nxt;
    } else {
        heap_rover = cur->next ? cur->next : heap_head;
    }
    cur->free_flag = 0;
    return (void *)(cur + 1);
}

void *malloc(size_t size) {
    if (!size) size = 8;
    size = (size + 15u) & ~(size_t)15u;
    heap_init();
    blk_hdr_t *start = heap_rover ? heap_rover : heap_head;
    blk_hdr_t *cur = start;
    while (cur) {
        if (cur->free_flag && cur->size >= size) return alloc_from(cur, size);
        cur = cur->next;
    }
    cur = heap_head;
    while (cur && cur != start) {
        if (cur->free_flag && cur->size >= size) return alloc_from(cur, size);
        cur = cur->next;
    }
    return 0;
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void free(void *ptr) {
    if (!ptr) return;
    blk_hdr_t *b = ((blk_hdr_t *)ptr) - 1;
    b->free_flag = 1;
    if (b->next && b->next->free_flag &&
        (uint8_t *)(b + 1) + b->size == (uint8_t *)b->next) {
        if (heap_rover == b->next) heap_rover = b;
        b->size += sizeof(blk_hdr_t) + b->next->size;
        b->next = b->next->next;
        if (b->next) b->next->prev = b;
    }
    if (b->prev && b->prev->free_flag &&
        (uint8_t *)(b->prev + 1) + b->prev->size == (uint8_t *)b) {
        blk_hdr_t *p = b->prev;
        if (heap_rover == b) heap_rover = p;
        p->size += sizeof(blk_hdr_t) + b->size;
        p->next = b->next;
        if (b->next) b->next->prev = p;
    }
}

#include "nano_heap.h"
void nano_heap_stats(uint32_t *used_bytes, uint32_t *free_bytes, uint32_t *largest_free, uint32_t *num_blocks) {
    heap_init();
    uint32_t u = 0, f = 0, lg = 0, nb = 0;
    for (blk_hdr_t *c = heap_head; c; c = c->next) {
        ++nb;
        if (c->free_flag) {
            f += (uint32_t)c->size;
            if ((uint32_t)c->size > lg) lg = (uint32_t)c->size;
        } else {
            u += (uint32_t)c->size;
        }
    }
    if (used_bytes) *used_bytes = u;
    if (free_bytes) *free_bytes = f;
    if (largest_free) *largest_free = lg;
    if (num_blocks) *num_blocks = nb;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (!size) { free(ptr); return 0; }
    blk_hdr_t *b = ((blk_hdr_t *)ptr) - 1;
    if (b->size >= size) return ptr;
    void *np = malloc(size);
    if (!np) return 0;
    memcpy(np, ptr, b->size);
    free(ptr);
    return np;
}

/* ---- String / ctype ---- */
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c; }
int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c; }

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = dst; const uint8_t *s = src;
    if (d < s) { for (size_t i = 0; i < n; ++i) d[i] = s[i]; }
    else if (d > s) { for (size_t i = n; i > 0; --i) d[i - 1] = s[i - 1]; }
    return dst;
}
int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *a = s1, *b = s2;
    for (size_t i = 0; i < n; ++i) if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { ++a; ++b; } return (unsigned char)*a - (unsigned char)*b; }
int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i] || !a[i]) return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0; for (; i < n && s[i]; ++i) d[i] = s[i];
    for (; i < n; ++i) d[i] = 0;
    return d;
}
char *strcat(char *d, const char *s) { char *r = d; while (*d) ++d; while ((*d++ = *s++)) {} return r; }
char *strchr(const char *s, int c) { for (; *s; ++s) if (*s == (char)c) return (char *)s; return c == 0 ? (char *)s : 0; }
char *strrchr(const char *s, int c) {
    const char *last = 0;
    for (; *s; ++s) if (*s == (char)c) last = s;
    return c == 0 ? (char *)s : (char *)last;
}
char *strstr(const char *h, const char *n) {
    if (!*n) return (char *)h;
    for (; *h; ++h) {
        const char *a = h, *b = n;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (!*b) return (char *)h;
    }
    return 0;
}

int abs(int j) { return j < 0 ? -j : j; }
static unsigned int rng_state = 1;
int rand(void) { rng_state = rng_state * 1103515245u + 12345u; return (int)((rng_state >> 16) & 0x7fffu); }
void srand(unsigned int seed) { rng_state = seed ? seed : 1; }

double strtod(const char *s, char **endptr) {
    while (isspace((unsigned char)*s)) ++s;
    int neg = 0;
    if (*s == '-') { neg = 1; ++s; }
    else if (*s == '+') ++s;
    double v = 0.0;
    while (isdigit((unsigned char)*s)) { v = v * 10.0 + (*s - '0'); ++s; }
    if (*s == '.') {
        ++s;
        double f = 0.1;
        while (isdigit((unsigned char)*s)) { v += (*s - '0') * f; f *= 0.1; ++s; }
    }
    if (*s == 'e' || *s == 'E') {
        ++s;
        int eneg = 0, exp = 0;
        if (*s == '-') { eneg = 1; ++s; }
        else if (*s == '+') ++s;
        while (isdigit((unsigned char)*s)) { exp = exp * 10 + (*s - '0'); ++s; }
        while (exp-- > 0) v = eneg ? v * 0.1 : v * 10.0;
    }
    if (endptr) *endptr = (char *)s;
    return neg ? -v : v;
}
double atof(const char *s) { return strtod(s, 0); }
int atoi(const char *s) {
    while (isspace((unsigned char)*s)) ++s;
    int neg = 0, v = 0;
    if (*s == '-') { neg = 1; ++s; } else if (*s == '+') ++s;
    while (isdigit((unsigned char)*s)) v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

void exit(int status) { (void)status; for (;;) {} }

/* ---- Minimal vsnprintf / sprintf ---- */
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    size_t pos = 0;
    #define PUTC(ch) do { if (size && pos + 1 < size) buf[pos] = (ch); ++pos; } while (0)
    while (*fmt) {
        if (*fmt != '%') { PUTC(*fmt++); continue; }
        ++fmt;
        int zero_pad = 0, width = 0, prec = -1;
        if (*fmt == '0') { zero_pad = 1; ++fmt; }
        while (isdigit((unsigned char)*fmt)) width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') { ++fmt; prec = 0; while (isdigit((unsigned char)*fmt)) prec = prec * 10 + (*fmt++ - '0'); }
        if (*fmt == 'l') ++fmt;
        char spec = *fmt ? *fmt++ : 0;
        if (spec == '%') { PUTC('%'); }
        else if (spec == 'c') { PUTC((char)va_arg(ap, int)); }
        else if (spec == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = (int)strlen(s);
            while (width > len) { PUTC(' '); --width; }
            while (*s) PUTC(*s++);
        } else if (spec == 'd' || spec == 'i' || spec == 'u' || spec == 'x' || spec == 'X' || spec == 'p') {
            unsigned long v; int neg = 0;
            if (spec == 'd' || spec == 'i') {
                long sv = va_arg(ap, int);
                if (sv < 0) { neg = 1; v = (unsigned long)(-sv); } else v = (unsigned long)sv;
            } else if (spec == 'p') {
                v = (unsigned long)(uintptr_t)va_arg(ap, void *);
            } else {
                v = va_arg(ap, unsigned int);
            }
            unsigned base = (spec == 'x' || spec == 'X' || spec == 'p') ? 16u : 10u;
            const char *digs = (spec == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
            char tmp[32]; int n = 0;
            if (v == 0) tmp[n++] = '0';
            else while (v) { tmp[n++] = digs[v % base]; v /= base; }
            int pad = width - n - (neg ? 1 : 0);
            if (neg && zero_pad) PUTC('-');
            while (pad-- > 0) PUTC(zero_pad ? '0' : ' ');
            if (neg && !zero_pad) PUTC('-');
            while (n--) PUTC(tmp[n]);
        } else if (spec == 'f') {
            double dv = va_arg(ap, double);
            if (dv < 0) { PUTC('-'); dv = -dv; }
            if (prec < 0) prec = 3;
            long ip = (long)dv;
            double fp = dv - (double)ip;
            char tmp[32]; int n = 0;
            if (ip == 0) tmp[n++] = '0';
            else while (ip) { tmp[n++] = (char)('0' + ip % 10); ip /= 10; }
            while (n--) PUTC(tmp[n]);
            if (prec > 0) {
                PUTC('.');
                for (int k = 0; k < prec; ++k) {
                    fp *= 10.0;
                    int d = (int)fp;
                    if (d > 9) d = 9;
                    PUTC((char)('0' + d));
                    fp -= d;
                }
            }
        }
    }
    if (size) buf[pos < size ? pos : size - 1] = 0;
    return (int)pos;
    #undef PUTC
}
int sprintf(char *buf, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, 0x7fffffffu, fmt, ap); va_end(ap); return r;
}
int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, size, fmt, ap); va_end(ap); return r;
}
FILE *stdout = 0;
int fflush(FILE *s) { (void)s; return 0; }
int printf(const char *fmt, ...) { (void)fmt; return 0; }
static char log_ring[1536];
static unsigned log_pos = 0;
const char *shmup_get_log(void) { log_ring[log_pos] = 0; return log_ring; }
int vprintf(const char *fmt, va_list ap) {
    char tmp[256];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    for (int i = 0; i < n && tmp[i]; ++i) {
        if (log_pos + 1 >= sizeof log_ring) {
            memmove(log_ring, log_ring + 256, sizeof log_ring - 256);
            log_pos -= 256;
        }
        log_ring[log_pos++] = tmp[i];
    }
    log_ring[log_pos] = 0;
    return n;
}
void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {
    uint8_t *b = (uint8_t *)base;
    uint8_t tmp[256];
    if (!base || !compar || nmemb < 2 || size > sizeof tmp) return;
    for (size_t i = 1; i < nmemb; ++i) {
        memcpy(tmp, b + i * size, size);
        size_t j = i;
        while (j > 0 && compar(b + (j - 1) * size, tmp) > 0) {
            memcpy(b + j * size, b + (j - 1) * size, size);
            --j;
        }
        memcpy(b + j * size, tmp, size);
    }
}

/* ---- Math ---- */
double fabs(double x) { return x < 0 ? -x : x; }
float fabsf(float x) { return x < 0 ? -x : x; }
double floor(double x) { long i = (long)x; return (double)(x < (double)i ? i - 1 : i); }
float floorf(float x) { return (float)floor(x); }
double ceil(double x) { long i = (long)x; return (double)(x > (double)i ? i + 1 : i); }
float ceilf(float x) { return (float)ceil(x); }

double sqrt(double x) {
    if (x <= 0.0) return 0.0;
    double g = x > 1.0 ? x * 0.5 : 1.0;
    for (int i = 0; i < 24; ++i) g = 0.5 * (g + x / g);
    return g;
}
float sqrtf(float x) { return (float)sqrt(x); }

double sin(double x) {
    const double two_pi = 6.28318530717958647692;
    x = x - floor(x / two_pi) * two_pi;
    if (x > M_PI) x -= two_pi;
    if (x > M_PI * 0.5) x = M_PI - x;
    else if (x < -M_PI * 0.5) x = -M_PI - x;
    double x2 = x * x;
    return x * (1.0 - x2 / 6.0 * (1.0 - x2 / 20.0 * (1.0 - x2 / 42.0 * (1.0 - x2 / 72.0))));
}
float sinf(float x) { return (float)sin(x); }
double cos(double x) { return sin(x + M_PI * 0.5); }
float cosf(float x) { return (float)cos(x); }
double tan(double x) { double c = cos(x); return fabs(c) < 1e-12 ? 0.0 : sin(x) / c; }
float tanf(float x) { return (float)tan(x); }

double atan2(double y, double x) {
    if (x == 0.0 && y == 0.0) return 0.0;
    double ax = fabs(x), ay = fabs(y);
    double a = ax < ay ? ax / ay : ay / ax;
    double s = a * a;
    double r = ((-0.0464964749 * s + 0.15931422) * s - 0.327622764) * s * a + a;
    if (ay > ax) r = 1.57079637 - r;
    if (x < 0.0) r = M_PI - r;
    if (y < 0.0) r = -r;
    return r;
}
float atan2f(float y, float x) { return (float)atan2(y, x); }
double acos(double x) {
    if (x >= 1.0) return 0.0;
    if (x <= -1.0) return M_PI;
    return atan2(sqrt(1.0 - x * x), x);
}
float acosf(float x) { return (float)acos(x); }
double pow(double x, double y) {
    if (y == 0.0) return 1.0;
    if (x <= 0.0) return 0.0;
    int iy = (int)y;
    if ((double)iy == y && iy >= 0 && iy <= 16) {
        double r = 1.0; while (iy--) r *= x; return r;
    }
    return 1.0;
}
float powf(float x, float y) { return (float)pow(x, y); }
