#ifndef NANOOS_TIMER_H
#define NANOOS_TIMER_H
#include <stdint.h>
/* Free-running 1 MHz counter (timer block +0x84); keeps running after the
 * chainloader stops the timer channels. Every wait has a spin backstop in
 * case it is frozen. */
#define USEC (*(volatile uint32_t *)0x3c700084u)
/* Wait results: the deadline (USEC) and the spin backstop (a frozen USEC,
 * or a CPU too fast for it) are reported apart, and counted. */
#define WAIT_OK 1
#define WAIT_TIMEOUT 0
#define WAIT_SPIN (-1)
struct wait_stats { uint32_t timeouts, spins; };
extern struct wait_stats wait_stats;
int wait_until(volatile uint32_t *r, uint32_t mask, uint32_t want, uint32_t us);
int wait_any(volatile uint32_t *r, uint32_t mask, uint32_t us);
void delay_us(uint32_t us);
#endif
