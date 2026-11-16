#ifndef GL2_STD_H
#define GL2_STD_H
#include <stdint.h>
struct gl2_ctx;
void gl2std_set_context(struct gl2_ctx *c);
int gl2std_host_init(void);
uint32_t *gl2std_host_target(void);
typedef void (*gl2std_frame_fn)(void);
void gl2std_set_frame_hooks(gl2std_frame_fn ensure, gl2std_frame_fn changed);
#endif
