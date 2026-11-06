#ifndef RTIMER_ARCH_H_
#define RTIMER_ARCH_H_

#include <stdint.h>


/* 1 tick  = 1 µs, so 1 second = 1 000 000 ticks */
#define RTIMER_ARCH_SECOND 1000000UL

/* ------------------------------------------------------------------ */
/*  Architecture hooks Contiki expects                                */
/* ------------------------------------------------------------------ */
rtimer_clock_t rtimer_arch_now(void);
void           rtimer_arch_init(void);
void           rtimer_arch_schedule(rtimer_clock_t t);

#endif /* RTIMER_ARCH_H_ */