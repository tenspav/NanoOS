#include "nano_app.h"
#include "gl2_egl.h"

static const struct nanoos_api *g_api;
static int g_was_down;
static int g_last_x;
static int g_last_y;

int nano_app_init(const struct nanoos_api *api) {
    if (!api || api->abi != NGAME_ABI || NGAME_API_TABLE(api) < 3) return -1;
    g_api = api;
    g_was_down = 0;
    g_last_x = 0;
    g_last_y = 0;
    gl2_egl_set_platform(api);
    return 0;
}

const struct nanoos_api *nano_app_api(void) {
    return g_api;
}

uint32_t nano_app_usec(void) {
    return (g_api && g_api->usec) ? g_api->usec() : 0u;
}

void nano_app_dcache_clean(void) {
    if (g_api && g_api->dcache_clean) g_api->dcache_clean();
}

void nano_app_poll_input(nano_input_t *out) {
    if (!out) return;
    out->down = 0;
    out->pressed = 0;
    out->released = 0;
    out->moved = 0;
    out->x = g_last_x;
    out->y = g_last_y;
    out->prev_x = g_last_x;
    out->prev_y = g_last_y;
    out->buttons = 0;
    out->home_pressed = 0;

    if (!g_api) return;
    if (g_api->input_poll) {
        out->buttons = g_api->input_poll();
        out->home_pressed = (out->buttons & NGAME_HOME) ? 1 : 0;
    }

    if (g_api->touch_poll && g_api->touch) {
        g_api->touch_poll();
        unsigned st = g_api->touch->state;
        if (st == 3u || st == 4u) {
            int dx = 239 - g_api->touch->y;
            int dy = g_api->touch->x;
            if (dx < 0) dx = 0;
            if (dx >= (int)NANO_SCREEN_W) dx = (int)NANO_SCREEN_W - 1;
            if (dy < 0) dy = 0;
            if (dy >= (int)NANO_SCREEN_H) dy = (int)NANO_SCREEN_H - 1;
            out->down = 1;
            out->x = dx;
            out->y = dy;
            if (!g_was_down) {
                out->pressed = 1;
            } else if (dx != g_last_x || dy != g_last_y) {
                out->moved = 1;
            }
            g_last_x = dx;
            g_last_y = dy;
            g_was_down = 1;
        } else if (g_was_down) {
            out->released = 1;
            g_was_down = 0;
        }
    }
}

int nano_app_home_pressed(void) {
    if (!g_api) return 0;
    if (g_api->input_poll && (g_api->input_poll() & NGAME_HOME)) return 1;
    if (g_api->input_held && (g_api->input_held() & NGAME_HOME)) return 1;
    return 0;
}

void nano_app_gfx_stats(uint32_t *frames, uint32_t *software_frames, uint32_t *fault) {
    gl2_egl_stats(frames, software_frames, fault);
}
