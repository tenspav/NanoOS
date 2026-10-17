#ifndef NANOOS_DRIVERS_H
#define NANOOS_DRIVERS_H
/* DevMode "Drivers" page: one status line per driver (DRIVER_PORT.md) */
void drivers_enter(void);    /* run the one-shot probes, start BT + tone */
void drivers_service(void);  /* every main-loop pass while the page is open */
void drivers_draw(void);
#endif
