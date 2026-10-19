#ifndef NANOOS_TOUCH_H
#define NANOOS_TOUCH_H
#include <stdint.h>
#define TOUCH_DOWN 3
#define TOUCH_MOVE 4
#define TOUCH_UP 5
/* last contact, in logical landscape coordinates (lcd.h), and counts */
struct touch_state { int x, y; unsigned state, reports, errors, diag, bad; };  /* diag/bad: touch.c */
extern struct touch_state touch;
void touch_init(void);   /* touch_start(), then (TOUCH_DIAG) the capture window and R84 */
void touch_start(void);  /* gate on, drop reports queued before the handoff (launcher) */
int touch_poll(void);    /* rate-limited; 1 when a new report was read */
/* round 12: 1 = query every poll (launcher, Settings, DevMode), 0 = only
 * on ATN low plus every 16th poll (ViceNano scenes, main.c) */
extern unsigned touch_fast;
/* the transport counters as 2 Drivers lines at (x, y), dy apart; returns
 * the y after them (drivers.c) */
int touch_lines(int x, int y, int dy);
/* R84.txt (report.h); touch_extra[] = other words it carries (buttons.c
 * Home search), so the slim kernel without buttons.c still links.
 * TOUCH_DIAG 0 drops R84 and the Home search (~760 B of kernel.bin). */
#ifndef TOUCH_DIAG
#define TOUCH_DIAG 1
#endif
#define TOUCH_EXTRA 18u
extern uint32_t touch_extra[TOUCH_EXTRA];
#if TOUCH_DIAG
void touch_report(void);
/* 3 s of step() with title and a countdown on screen (capture windows) */
void touch_window(const char *title, int (*step)(void));
#else
static inline void touch_report(void) {}
#endif
#endif
