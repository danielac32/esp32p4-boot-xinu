/**
 * @file timer.h
 *
 * Small timing shim so the ported ESP32-P4 drivers (which were written
 * against a bare-metal `timer.h`) can use the Xinu clock: milliseconds /
 * microseconds counters and busy-wait delays.
 *
 * Implementation: system/timershim.c (over clkcount()/udelay()/mdelay()).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _TIMER_H_
#define _TIMER_H_

#include <stdint.h>
#include <stdbool.h>

/* Kept for source compatibility with the ported drivers; the Xinu clock
 * is already running, so this is a no-op. */
void timer_init(void);

/* Monotonic counters derived from clkcount(). */
uint32_t millis(void);
uint32_t micros(void);

/* Busy-wait delays (udelay/mdelay). */
void delay_ms(uint32_t ms);
void delay_us(uint32_t us);

/* Kept for source compatibility; the Xinu clock handler owns the tick. */
void systick_handler(void);

#endif                          /* _TIMER_H_ */
