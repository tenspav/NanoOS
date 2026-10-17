#include "timer.h"
struct wait_stats wait_stats;
/* Spin backstop per microsecond: far above any loop rate with caches on,
 * so in practice only a frozen USEC exhausts it. */
#define SPINS_PER_US 64u
static int wait(volatile uint32_t *r, uint32_t mask, uint32_t want, int any,
                uint32_t us) {
    uint32_t t0 = USEC;
    for (uint32_t spins = 0; spins < us * SPINS_PER_US; ++spins) {
        uint32_t v = *r & mask;
        if (any ? v != 0 : v == want) return WAIT_OK;
        if (USEC - t0 >= us) { ++wait_stats.timeouts; return WAIT_TIMEOUT; }
    }
    ++wait_stats.spins;
    return WAIT_SPIN;
}
int wait_until(volatile uint32_t *r, uint32_t mask, uint32_t want, uint32_t us) {
    return wait(r, mask, want, 0, us);
}
int wait_any(volatile uint32_t *r, uint32_t mask, uint32_t us) {
    return wait(r, mask, 0, 1, us);
}
void delay_us(uint32_t us) {
    uint32_t t0 = USEC;
    for (uint32_t spins = 0; spins < us * SPINS_PER_US; ++spins)
        if (USEC - t0 >= us) return;
    ++wait_stats.spins;
}
