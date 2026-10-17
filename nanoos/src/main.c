#include "lcd.h"
#include "input.h"
#include "devmode.h"
void reset_stockos(void) __attribute__((noreturn));
#define APPS 5
#define DEVMODE 4
static const char *const names[APPS] = {"StockOS", "ViceNano", "PS1Emu", "Settings", "DevMode"};
static const char *const labels[APPS] = {"iP", "VC", "P1", "ST", "DV"};
static const uint32_t colors[APPS] = {0xff2684ff, 0xffff8800, 0xff28b848, 0xff808080, 0xffa04ce0};
static const struct {
    const char *name, *title, *speeds;
    uint32_t cpu, gpu;
} profiles[] = {
    {"STOCK", "NanoOS v0.0.1 beta by Tenspav | STOCK", "342 MHz CPU / 133 MHz GPU", 0x40009001u, 0xe0010000u},
    {"SAFE", "NanoOS v0.0.1 beta by Tenspav | SAFE", "684 MHz CPU / 171 MHz GPU", 0x40009000u, 0xd0030000u},
    {"MAX", "NanoOS v0.0.1 beta by Tenspav | MAX", "684 MHz CPU / 266 MHz GPU", 0x40009000u, 0xe0000000u},
};
static unsigned active_profile;
static void clock_selector(unsigned selected) {
    lcd_clear(0xff1a1a1a);
    lcd_center(16, "NanoOS v0.0.1 beta by Tenspav", WHITE);
    lcd_center(56, "Choose clock profile", WHITE);
    for (unsigned i = 0; i < 3; ++i) {
        int x = 48 + (int)i * 144;
        lcd_text(x, 100, profiles[i].name, WHITE);
        if (i == selected) lcd_text(x - 16, 100, ">", WHITE);
    }
    lcd_center(140, profiles[selected].speeds, WHITE);
    lcd_center(184, "Volume / Left-Right to choose", WHITE);
    lcd_center(208, "Home / Enter to confirm", WHITE);
    lcd_present();
}
static void apply_profile(unsigned selected) {
    *(volatile uint32_t *)0x3c500000u = profiles[selected].cpu;
    *(volatile uint32_t *)0x3c50001cu = profiles[selected].gpu;
    __asm__ volatile("dsb sy" ::: "memory");
    active_profile = selected;
}
static void launcher(unsigned selected) {
    lcd_clear(0xff1a1a1a);
    lcd_center(16, profiles[active_profile].title, WHITE);
    for (unsigned i = 0; i < APPS; ++i) {
        int x = LCD_WIDTH / (2 * APPS) + (int)i * (LCD_WIDTH / APPS);
        if (i == selected) lcd_circle(x, 112, 41, WHITE);
        lcd_circle(x, 112, i == selected ? 38 : 32, colors[i]);
        lcd_text(x-8, 108, labels[i], WHITE);
    }
    lcd_center(180, names[selected], WHITE);
    lcd_present();
}
static void placeholder(unsigned target) {
    lcd_clear(0xff1a1a1a);
    lcd_center(100, names[target], WHITE);
    lcd_center(132, "press Home to go back", WHITE);
    lcd_present();
}
void kernel_main(void) {
    unsigned selected = 0, scene = 0, profile = 0;
    unsigned choosing_clock = 1;
    lcd_init();
    input_init();
    clock_selector(profile);
    for (;;) {
        unsigned keys = input_poll();
        devmode_tick();
        if (choosing_clock) {
            unsigned old = profile;
            if (keys & BUTTON_LEFT) profile = (profile + 2) % 3;
            if (keys & BUTTON_RIGHT) profile = (profile + 1) % 3;
            if (profile != old) clock_selector(profile);
            if (keys & BUTTON_HOME) {
                apply_profile(profile);
                choosing_clock = 0;
                launcher(selected);
            }
        } else if (scene) {
            if (keys & BUTTON_HOME) { scene = 0; launcher(selected); }
            else if (scene == DEVMODE) devmode_draw();
        } else {
            unsigned old = selected;
            if (keys & BUTTON_LEFT) selected = (selected + APPS - 1) % APPS;
            if (keys & BUTTON_RIGHT) selected = (selected + 1) % APPS;
            if (selected != old) launcher(selected);
            if (keys & BUTTON_HOME) {
                if (!selected) reset_stockos();
                scene = selected;
                if (scene == DEVMODE) { devmode_enter(); devmode_draw(); }
                else placeholder(scene);
            }
        }
    }
}
