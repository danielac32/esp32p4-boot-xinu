/**
 * @file memmove.c
 *
 * Copy a block of memory, handling overlap (the regions may overlap in either
 * direction).  Used by lwIP (lwip_itoa); Xinu's libxc did not have it.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <string.h>

void *memmove(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    if (d == s || n == 0)
    {
        return dest;
    }

    if (d < s)
    {
        while (n--)
        {
            *d++ = *s++;
        }
    }
    else
    {
        d += n;
        s += n;
        while (n--)
        {
            *--d = *--s;
        }
    }

    return dest;
}
