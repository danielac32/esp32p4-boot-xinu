/**
 * @file strcat.c
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <string.h>

/**
 * @ingroup libxc
 *
 * Append a copy of the null-terminated string @p src to the end of the
 * null-terminated string @p dest, overwriting the null terminator of @p dest
 * and adding a new one.  The strings may not overlap.
 *
 * @param dest
 *      Pointer to the null-terminated string to which to append.
 * @param src
 *      Pointer to the source string.
 *
 * @return @p dest
 */
char *strcat(char *dest, const char *src)
{
    char *dest_save = dest;

    dest = strchr(dest, '\0');

    while (*src != '\0')
    {
        *dest++ = *src++;
    }
    *dest = '\0';

    return dest_save;
}
