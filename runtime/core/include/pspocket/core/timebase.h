/*
 * Guest timebase (mftb). The PS3 timebase runs at 79.8 MHz; the host monotonic clock is scaled to it.
 * C-callable so lifted code can use it directly.
 */
#ifndef PSPOCKET_CORE_TIMEBASE_H
#define PSPOCKET_CORE_TIMEBASE_H

#include <stdint.h>

#define PS_TIMEBASE_HZ 79800000ull

#ifdef __cplusplus
extern "C" {
#endif

/* Timebase ticks since the runtime first asked for the time. Monotonic, thread-safe. */
uint64_t ps_timebase_now(void);

/* Host monotonic nanoseconds (for profiling and frame pacing, not for the guest). */
uint64_t ps_host_now_ns(void);

#ifdef __cplusplus
}
#endif

#endif /* PSPOCKET_CORE_TIMEBASE_H */
