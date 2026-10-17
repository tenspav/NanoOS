/* TEMPORARY debug trace: (USEC, tag) ring at 0x08300000, read by QMP */
#ifndef NANOOS_TRACE_H
#define NANOOS_TRACE_H
#include <stdint.h>
#include "timer.h"
static inline void trace(uint32_t tag) {
    volatile uint32_t *t = (volatile uint32_t *)0x08300000u;
    uint32_t i = t[0] & 1023;
    t[2 + 2 * i] = USEC; t[3 + 2 * i] = tag; t[0] = i + 1;
}
#endif
