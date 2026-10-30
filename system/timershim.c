/**
 * @file timershim.c
 *
 * Timing shim for the ported ESP32-P4 drivers (see include/timer.h).
 * It maps the SDK's timer API onto the Xinu clock:
 *   millis()/micros()  -> clkcount() / platform.clkfreq
 *   delay_ms()/delay_us()-> mdelay()/udelay()
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <conf.h>

#if RTCLOCK

#include <clock.h>
#include <platform.h>
#include <timer.h>

void timer_init(void)
{
    /* The Xinu clock (clkinit/clkhandler) is already running. */
}

uint32_t millis(void)
{
    return (uint32_t)(clkcount() / (platform.clkfreq / 1000UL));
}

uint32_t micros(void)
{
    return (uint32_t)(clkcount() / (platform.clkfreq / 1000000UL));
}

void delay_ms(uint32_t ms)
{
    mdelay((ulong) ms);
}

void delay_us(uint32_t us)
{
    udelay((ulong) us);
}

void systick_handler(void)
{
    /* The Xinu clock handler owns the tick; nothing to do here. */
}

#endif                          /* RTCLOCK */
