/**
 * @file calloc.c
 *
 * Allocate zero-initialized storage from the kernel L2MEM heap.
 *
 * Same backend as malloc()/free() (memget/memfree over @c memlist): the
 * PSRAM heap has its own calloc-like API (psram_calloc, system/psrammem.c).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <memory.h>
#include <stdlib.h>
#include <string.h>

/**
 * @ingroup libxc
 *
 * Allocate memory for an array of @p nmemb elements of @p size bytes each
 * and set it to zero.
 *
 * @param nmemb
 *      Number of elements.
 * @param size
 *      Size of each element, in bytes.
 *
 * @return
 *      Pointer to the zeroed region, or @c NULL on failure (including a
 *      multiplication overflow).
 */
void *calloc(size_t nmemb, size_t size)
{
    size_t total;
    void *ptr;

    /* Refuse to wrap around size_t (the product must fit). */
    if (nmemb != 0 && size > (size_t)(~(size_t)0) / nmemb)
    {
        return NULL;
    }
    total = nmemb * size;

    ptr = malloc(total);
    if (NULL == ptr)
    {
        return NULL;
    }

    memset(ptr, 0, total);
    return ptr;
}
