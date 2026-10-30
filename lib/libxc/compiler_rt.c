/**
 * @file compiler_rt.c
 *
 * Minimal compiler run-time helpers for RV32.  The P4 kernel is linked
 * without libgcc, so the few integer routines the compiler cannot expand
 * inline (64-bit division, bit scanning) are provided here.
 *
 * All the routines are written with 32-bit operations only on purpose: using
 * 64-bit shifts/multiplies in their own implementation would just pull in more
 * helpers (__lshrdi3, __muldi3, ...) and defeat the point.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stdint.h>

/* ---------------- bit scanning ---------------- */

/* Number of leading zero bits in a 32-bit word (32 if x == 0). */
int __clzsi2(unsigned int x)
{
    int n = 0;

    if (x == 0)
    {
        return 32;
    }
    if (x <= 0x0000FFFFU)
    {
        n += 16;
        x <<= 16;
    }
    if (x <= 0x00FFFFFFU)
    {
        n += 8;
        x <<= 8;
    }
    if (x <= 0x0FFFFFFFU)
    {
        n += 4;
        x <<= 4;
    }
    if (x <= 0x3FFFFFFFU)
    {
        n += 2;
        x <<= 2;
    }
    if (x <= 0x7FFFFFFFU)
    {
        n += 1;
    }
    return n;
}

/* Number of trailing zero bits in a 32-bit word (32 if x == 0). */
int __ctzsi2(unsigned int x)
{
    int n = 0;

    if (x == 0)
    {
        return 32;
    }
    if ((x & 0x0000FFFFU) == 0)
    {
        n += 16;
        x >>= 16;
    }
    if ((x & 0x000000FFU) == 0)
    {
        n += 8;
        x >>= 8;
    }
    if ((x & 0x0000000FU) == 0)
    {
        n += 4;
        x >>= 4;
    }
    if ((x & 0x00000003U) == 0)
    {
        n += 2;
        x >>= 2;
    }
    if ((x & 0x00000001U) == 0)
    {
        n += 1;
    }
    return n;
}

/* Index (1-based) of the least significant set bit, or 0 if a == 0. */
int __ffssi2(int a)
{
    return (a == 0) ? 0 : __ctzsi2((unsigned int) a) + 1;
}

/* ---------------- 64-bit division ---------------- */

/*
 * Unsigned 64/64 -> 64 division with remainder: 32-bit shift-and-subtract so
 * that no 64-bit shift/multiply helper is required.  If d == 0 the result is
 * implementation-defined (we return 0 to avoid a divide trap).
 */
static uint64_t udiv64(uint64_t n, uint64_t d, uint64_t *rem)
{
    uint32_t nh = (uint32_t) (n >> 32);
    uint32_t nl = (uint32_t) n;
    uint32_t dh = (uint32_t) (d >> 32);
    uint32_t dl = (uint32_t) d;
    uint32_t qh = 0, ql = 0, rh = 0, rl = 0;
    int i;

    if (dh == 0 && dl == 0)
    {
        if (rem != 0)
        {
            *rem = 0;
        }
        return 0;
    }

    for (i = 63; i >= 0; i--)
    {
        uint32_t bit;

        /* r = (r << 1) | next bit of n */
        if (i >= 32)
        {
            bit = (nh >> (i - 32)) & 1U;
        }
        else
        {
            bit = (nl >> i) & 1U;
        }
        rh = (rh << 1) | (rl >> 31);
        rl = (rl << 1) | bit;

        /* if r >= d then r -= d and set bit i of the quotient */
        if (rh > dh || (rh == dh && rl >= dl))
        {
            uint32_t borrow = (rl < dl) ? 1U : 0U;

            rl -= dl;
            rh = rh - dh - borrow;
            if (i >= 32)
            {
                qh |= (1U << (i - 32));
            }
            else
            {
                ql |= (1U << i);
            }
        }
    }

    if (rem != 0)
    {
        *rem = ((uint64_t) rh << 32) | rl;
    }
    return ((uint64_t) qh << 32) | ql;
}

uint64_t __udivdi3(uint64_t a, uint64_t b)
{
    return udiv64(a, b, 0);
}

uint64_t __umoddi3(uint64_t a, uint64_t b)
{
    uint64_t r;

    (void) udiv64(a, b, &r);
    return r;
}
