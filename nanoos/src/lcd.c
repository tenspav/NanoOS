#include "lcd.h"
#define REG(o) (*(volatile uint32_t *)(0x38900000u + (o)))
/* Private buffer beyond the kernel/stack; never reuse a RetailOS buffer
 * which might overlap the relocated kernel. Caches remain disabled. */
static volatile uint32_t *const panel = (volatile uint32_t *)0x08200000u;
static uint32_t fb[LCD_WIDTH * LCD_HEIGHT];
void lcd_init(void) {
    REG(0x24) = 0x08200000u;
    REG(0x20) = 0x002107ffu;
    REG(0x28) = LCD_PANEL_WIDTH;
    REG(0x2c) = 0;
    REG(0x30) = (LCD_PANEL_WIDTH << 16) | LCD_PANEL_HEIGHT;
    REG(0x34) = 0;
    REG(0x04) |= 1u << 4;
}
void lcd_pixel(int x, int y, uint32_t c) {
    if ((unsigned)x < LCD_WIDTH && (unsigned)y < LCD_HEIGHT)
        fb[y * LCD_WIDTH + x] = c;
}
void lcd_clear(uint32_t c) {
    for (unsigned i = 0; i < LCD_WIDTH * LCD_HEIGHT; ++i) fb[i] = c;
}
void lcd_circle(int x, int y, int r, uint32_t c) {
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx)
            if (dx*dx + dy*dy <= r*r) lcd_pixel(x+dx, y+dy, c);
}
void lcd_text(int x, int y, const char *s, uint32_t c) {
    for (; *s; ++s, x += 8)
        for (int row = 0; row < 8; ++row)
            for (int col = 0; col < 8; ++col)
                if (font8x8[(unsigned char)*s & 127][row] & (1u << col))
                    lcd_pixel(x+col, y+row, c);
}
void lcd_center(int y, const char *s, uint32_t c) {
    int n = 0;
    while (s[n]) ++n;
    lcd_text((LCD_WIDTH - n*8)/2, y, s, c);
}
void lcd_present(void) {
    /* No rotation bit is confirmed in the existing LCD register definitions.
     * Keep native scanout geometry; rotate the logical frame clockwise:
     * (x, y) -> (239 - y, x). View with the panel turned counterclockwise. */
    for (unsigned x = 0; x < LCD_WIDTH; ++x)
        for (unsigned y = 0; y < LCD_HEIGHT; ++y)
            panel[x * LCD_PANEL_WIDTH + LCD_PANEL_WIDTH - 1 - y] =
                fb[y * LCD_WIDTH + x];
    __asm__ volatile("dsb sy" ::: "memory");
}
