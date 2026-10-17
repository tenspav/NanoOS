#include "fmt.h"
#include "lcd.h"
static char buf[64];
static unsigned len;
char *fmt_out, *fmt_end;
void fmt_str(const char *s) {
    while (*s && len < sizeof buf - 1) buf[len++] = *s++;
    buf[len] = 0;
}
void fmt_dec(uint32_t v) {
    char t[11];
    unsigned n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n && len < sizeof buf - 1) buf[len++] = t[--n];
    buf[len] = 0;
}
void fmt_sdec(int32_t v) {
    if (v < 0) { fmt_str("-"); v = -v; }
    fmt_dec((uint32_t)v);
}
void fmt_hex(uint32_t v, unsigned digits) {
    while (digits-- && len < sizeof buf - 1)
        buf[len++] = "0123456789ABCDEF"[(v >> (4 * digits)) & 15];
    buf[len] = 0;
}
void fmt_line(int x, int y, uint32_t color) {
    if (!fmt_out) lcd_text(x, y, buf, color);
    else {
        for (unsigned i = 0; i < len && fmt_out < fmt_end; ++i) *fmt_out++ = buf[i];
        if (fmt_out < fmt_end) *fmt_out++ = '\n';
    }
    len = 0;
    buf[0] = 0;
}
