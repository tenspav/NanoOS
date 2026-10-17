#ifndef NANOOS_BUTTONS_H
#define NANOOS_BUTTONS_H
#include <stdint.h>
/* Levels of all physical buttons (1 = pressed). input.c keeps the event
 * logic the launcher uses (Home, Vol-/Vol+ on release, the chord). */
#define BTN_HOME 1u
#define BTN_POWER 2u
#define BTN_PLAY 4u
#define BTN_VOLUP 8u
#define BTN_VOLDOWN 16u
struct buttons_state { unsigned now, seen, r7, r8; int temp_c; unsigned pmu_err; };
extern struct buttons_state buttons;
void buttons_init(void);
void buttons_poll(void);   /* rate-limited PMU reads; GPIO every call */
#endif
