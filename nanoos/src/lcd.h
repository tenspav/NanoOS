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
void lcd_circle(int x, int y, int radius, uint32_t color);
void lcd_text(int x, int y, const char *text, uint32_t color);
void lcd_center(int y, const char *text, uint32_t color);
void lcd_present(void);   /* marks the frame; lcd_service() pushes it */
void lcd_service(void);   /* call every main-loop pass; never blocks long */
int lcd_busy(void);       /* a frame is queued or in flight */
int lcd_ready(void);      /* no frame queued: the next one may be drawn
                           * (while the last one is still being pushed) */
void lcd_flush(void);     /* present and wait until it is on the glass
                           * (at most FLUSH_US) */
/* completed pushes; expired waits by deadline / by poll backstop, the
 * state the last one expired in; pushes whose busy bits never rose; pushes
 * whose busy bits were still up after BUSY_HOLD_US (ended anyway) */
struct lcd_stats { uint32_t frames, timeouts, spins, last, norise, held,
                   push_us, /* flip to end of the last push */
                   rise_us, end_us, /* 0x2c to busy seen / to the end */
                   flipped, /* USEC of the last flip */
                   recovered; /* window re-armed after RECOVER_AFTER
                               * pushes in a row with an expired wait */ };
extern struct lcd_stats lcd_stats;
extern const uint8_t font8x8[128][8];
#endif
