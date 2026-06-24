#pragma once
#include <stdint.h>

/* High Precision Event Timer: a free-running, high-resolution monotonic
 * counter. Used for sub-millisecond delays and timestamps that the 1 kHz PIT
 * tick is too coarse for. hpet_init() returns 0 if an HPET is present. */
int      hpet_init(void);
int      hpet_present(void);
uint64_t hpet_ns(void);                  /* nanoseconds since hpet_init() */
uint64_t hpet_us(void);                  /* microseconds since hpet_init() */
void     hpet_delay_us(uint64_t us);     /* busy-wait microseconds */
