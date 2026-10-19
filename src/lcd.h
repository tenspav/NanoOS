#ifndef NANOOS_LCD_H
#define NANOOS_LCD_H
#include <stdint.h>
#define LCD_WIDTH 432
#define LCD_HEIGHT 240
#define LCD_PANEL_WIDTH 240
#define LCD_PANEL_HEIGHT 432
#define WHITE 0xffffffffu
void lcd_init(void);
void lcd_pixel(int x, int y, uint32_t color);
void lcd_clear(uint32_t color);
/* logical rows [y0, y1) in color (0 <= y0 <= y1 <= LCD_HEIGHT) */
void lcd_fill_rows(int y0, int y1, uint32_t color);
void lcd_circle(int x, int y, int radius, uint32_t color);
void lcd_text(int x, int y, const char *text, uint32_t color);
void lcd_center(int y, const char *text, uint32_t color);
/* the buffer being drawn (panel layout: logical (x, y) at [x * 240 + 239 - y],
 * ARGB8888): a GPU may render into it; it changes at each flip */
uint32_t *lcd_back(void);
void lcd_present(void);   /* marks the frame; lcd_service() pushes it */
void lcd_service(void);   /* call every main-loop pass; never blocks long */
int lcd_busy(void);       /* a frame is queued, in flight or to be re-sent */
int lcd_ready(void);      /* the back buffer is free: no frame queued, so
                           * the next one may be drawn (while the last one
                           * may still be in flight: the buffer it is sent
                           * from is never the back buffer; scanout done =
                           * !lcd_busy()) */
void lcd_flush(void);     /* present and wait until it is on the glass
                           * (at most FLUSH_US) */
/* completed pushes; expired waits by deadline / by poll backstop (each
 * aborts its push, round 10), the state the last one expired in; pushes whose busy bits never rose; pushes
 * whose busy bits were still up after BUSY_HOLD_US (ended anyway) */
struct lcd_stats { uint32_t frames, timeouts, spins, last, norise, held,
                   push_us, /* flip to end of the last push */
                   rise_us, end_us, /* 0x2c to busy seen / to the end */
                   flipped, /* USEC of the last flip */
                   recovered; /* window re-armed after RECOVER_AFTER
                               * expired waits in a row */ };
extern struct lcd_stats lcd_stats;
/* NANOOS_FONT32 (the kernel build, build_payload.py): glyphs 32..127 only,
 * 256 B less; control characters draw as a space. NanoOSLoader links
 * font.c without it and indexes the full 128 entries. */
#ifdef NANOOS_FONT32
#define FONT_BASE 32u
#define FONT_GLYPH(ch) font8x8[((ch) & 127u) < FONT_BASE ? 0 : ((ch) & 127u) - FONT_BASE]
#else
#define FONT_BASE 0u
#define FONT_GLYPH(ch) font8x8[(ch) & 127u]
#endif
extern const uint8_t font8x8[128 - FONT_BASE][8];
#endif
