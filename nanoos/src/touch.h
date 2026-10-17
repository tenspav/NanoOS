#ifndef NANOOS_TOUCH_H
#define NANOOS_TOUCH_H
#include <stdint.h>
#define TOUCH_DOWN 3
#define TOUCH_MOVE 4
#define TOUCH_UP 5
/* last contact, in logical landscape coordinates (lcd.h), and counts */
struct touch_state { int x, y; unsigned state, reports, errors; };
extern struct touch_state touch;
void touch_init(void);   /* drop reports queued before the handoff */
int touch_poll(void);    /* rate-limited; 1 when a new report was read */
#endif
