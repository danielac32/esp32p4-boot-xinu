/**
 * @file riscvclk.c
 *
 * ESP32-P4 system timer (CLINT mtime), used as the Xinu real-time clock.
 *
 * The mtime counter increments at the CPU root clock (360 MHz).  mtime is
 * 64-bit but the P4 is RV32, so it is accessed as two 32-bit words with a
 * rollover-safe read and the safe mtimecmp write sequence (high = all
 * ones, then low, then high).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <clock.h>
#include <stdint.h>
#include <riscv.h>
#include <esp32p4.h>

static uint64_t mtime_read(void)
{
    uint32_t hi, lo;

    do
    {
        hi = P4_MTIME_HI;
        lo = P4_MTIME_LO;
    }
    while (hi != P4_MTIME_HI);

    return ((uint64_t)hi << 32) | lo;
}

/* clkcount() interface is documented in clock.h  */
ulong clkcount(void)
{
    return (ulong)mtime_read();
}

/* clkupdate() interface is documented in clock.h  */
void clkupdate(ulong cycles)
{
    uint64_t when = mtime_read() + (uint64_t)cycles;

    P4_MTIMECMP_HI = 0xFFFFFFFFU;
    P4_MTIMECMP_LO = (uint32_t)when;
    P4_MTIMECMP_HI = (uint32_t)(when >> 32);
}
