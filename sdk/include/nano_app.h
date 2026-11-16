#ifndef NANOOS_SDK_APP_H
#define NANOOS_SDK_APP_H

#include <stdint.h>
#include "gameabi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NANO_SCREEN_W 240u
#define NANO_SCREEN_H 432u

typedef struct {
    int down;          /* 1 while finger is touching screen */
    int pressed;       /* 1 on transition from up -> down */
    int released;      /* 1 on transition from down -> up */
    int moved;         /* 1 when down and coordinates changed */
    int x;             /* Display X in [0, 239] */
    int y;             /* Display Y in [0, 431] */
    int prev_x;        /* Previous display X in [0, 239] */
    int prev_y;        /* Previous display Y in [0, 431] */
    uint32_t buttons;  /* Raw NGAME_* button bitmask */
    int home_pressed;  /* 1 if NGAME_HOME is currently pressed */
} nano_input_t;

/* Initialize NanoOS application SDK (heap, EGL platform, input state).
 * Call once at the start of nano_example_main / game entry. */
int nano_app_init(const struct nanoos_api *api);

/* Return the active NanoOS system services table (or NULL before init). */
const struct nanoos_api *nano_app_api(void);

/* Monotonic microsecond timer. */
uint32_t nano_app_usec(void);

/* Clean CPU D-cache to DRAM. */
void nano_app_dcache_clean(void);

/* Poll touchscreen (mapped to 240x432 display coords) and physical buttons. */
void nano_app_poll_input(nano_input_t *out);

/* Return 1 if NGAME_HOME button is pressed. */
int nano_app_home_pressed(void);

/* Query EGL/GL2 rendering stats (frames, software_frames, fault). */
void nano_app_gfx_stats(uint32_t *frames, uint32_t *software_frames, uint32_t *fault);

#ifdef __cplusplus
}
#endif

#endif /* NANOOS_SDK_APP_H */
