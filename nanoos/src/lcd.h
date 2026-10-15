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
void lcd_present(void);
extern const uint8_t font8x8[128][8];
#endif
