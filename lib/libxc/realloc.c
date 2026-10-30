/**
 * @file realloc.c
 *
 * Resize a block previously returned by malloc()/calloc()/realloc() from the
 * kernel L2MEM heap.
 *
 * Xinu's allocator (memblock / first-fit / coalescing) has no in-place grow
 * primitive, so this allocates a fresh block, copies the common prefix and
 * releases the old one.  The PSRAM heap offers the same operation as
 * psram_realloc(), system/psrammem.c.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <memory.h>
#include <stdlib.h>
#include <string.h>

/**
 * @ingroup libxc
 *
 * Change the size of the memory block pointed to by @p ptr to @p size bytes.
 *
 * @param ptr
 *      Block previously returned by malloc()/calloc()/realloc(), or @c NULL.
 * @param size
 *      New size, in bytes.
 *
 * @return
 *      Pointer to the resized block, or @c NULL on failure.  When @p ptr is
 *      @c NULL this behaves like malloc(); when @p size is 0 the block is
 *      freed and @c NULL is returned.
 */
void *realloc(void *ptr, size_t size)
{
    struct memblock *block;
    size_t oldsize;
    void *newptr;

    if (NULL == ptr)
    {
        return malloc(size);
    }
    if (0 == size)
    {
        free(ptr);
        return NULL;
    }

    /* Back up to the accounting header written by malloc(). */
    block = (struct memblock *)ptr;
    block--;

    /* Same validity marker free() relies on. */
    if (block->next != block)
    {
        return NULL;
    }

    oldsize = (size_t)block->length - sizeof(struct memblock);

    newptr = malloc(size);
    if (NULL == newptr)
    {
        return NULL;
    }

    memcpy(newptr, ptr, (oldsize < size) ? oldsize : size);
    free(ptr);
    return newptr;
}
