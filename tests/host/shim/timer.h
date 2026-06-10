/* Host shim for src/timer.h. */
#ifndef HOST_TIMER_H
#define HOST_TIMER_H

#include <stdint.h>

typedef uint32_t tick_t;
static inline tick_t getticks(void) { return 0; }

#endif
