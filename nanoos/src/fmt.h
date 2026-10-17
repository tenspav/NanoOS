#ifndef NANOOS_FMT_H
#define NANOOS_FMT_H
#include <stdint.h>
/* One read-out line at a time (no libc): append, then fmt_line() draws it
 * and starts the next. Divisions are by constants only (no libgcc). */
void fmt_str(const char *s);
void fmt_dec(uint32_t v);
void fmt_sdec(int32_t v);
void fmt_hex(uint32_t v, unsigned digits);
void fmt_line(int x, int y, uint32_t color);
/* set: fmt_line appends the line + '\n' at fmt_out (up to fmt_end) instead */
extern char *fmt_out, *fmt_end;
#endif
