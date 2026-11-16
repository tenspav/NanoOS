#ifndef GL2_EGL_H
#define GL2_EGL_H
#include <stdint.h>
struct nanoos_api;
void gl2_egl_set_platform(const struct nanoos_api *api);
void gl2_egl_stats(uint32_t *frames, uint32_t *software, uint32_t *fault);
#endif
