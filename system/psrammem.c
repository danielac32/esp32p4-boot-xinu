/**
 * @file psrammem.c
 *
 * Free list for the ESP32-P4 PSRAM.
 *
 * This is the exact Xinu memory mechanics (@c memblock / first-fit /
 * coalescence, see memget.c/memfree.c) applied to a SECOND list,
 * @c psramlist, over [_psram_free_start, _psram_free_end)
 * (0x48000000..0x49F00000, 31 MiB; it excludes the LP-core RAM at
 * 0x49F00000+).  The PSRAM is initialized and mapped by the SBL
 * (mmu_init), so here we only manage it.
 *
 * The L2MEM heap (memlist) is untouched: malloc()/calloc()/free() from
 * the C library keep using L2MEM as in every Xinu.  PSRAM is used through
 * its own API: psram_malloc / psram_calloc / psram_free (same memblock
 * accounting header, on this list).
 *
 * Guard: psramlist lives in .bss (all zero), so until psram_meminit()
 * runs (from the psram device init) every psram_getmem returns SYSERR and
 * psram_malloc returns NULL.
 */
/* Embedded Xinu, Copyright (C) 2009.  All rights reserved. */

#include <interrupt.h>
#include <memory.h>
#include <stddef.h>
#include <string.h>

struct memblock psramlist;  /* Head of the free PSRAM list */
void *psminheap;            /* Start address of the PSRAM heap */
void *psmaxheap;            /* End address of the PSRAM heap */

extern unsigned char _psram_free_start[];
extern unsigned char _psram_free_end[];

/**
 * @ingroup memory_mgmt
 *
 * Build the free PSRAM list as one big block.
 */
void psram_meminit(void)
{
    struct memblock *memptr;

    psminheap = (void *)_psram_free_start;
    psmaxheap = (void *)_psram_free_end;

    /* Initialize the memory list as one big block */
    psramlist.next = (struct memblock *)psminheap;
    memptr = psramlist.next;

    memptr->next = NULL;
    psramlist.length = memptr->length =
        (uintptr_t)psmaxheap - (uintptr_t)psminheap;
}

/**
 * @ingroup memory_mgmt
 *
 * Allocate PSRAM storage, returning the lowest word address.  Verbatim
 * clone of memget() over @c psramlist.
 */
void *psram_getmem(uintptr_t nbytes)
{
    register struct memblock *prev, *curr, *leftover;
    irqmask im;

    if (0 == nbytes)
    {
        return (void *)SYSERR;
    }

    nbytes = (ulong)roundmb(nbytes);

    im = disable();

    prev = &psramlist;
    curr = psramlist.next;
    while (curr != NULL)
    {
        if (curr->length == nbytes)
        {
            prev->next = curr->next;
            psramlist.length -= nbytes;

            restore(im);
            return (void *)(curr);
        }
        else if (curr->length > nbytes)
        {
            leftover = (struct memblock *)((ulong)curr + nbytes);
            prev->next = leftover;
            leftover->next = curr->next;
            leftover->length = curr->length - nbytes;
            psramlist.length -= nbytes;

            restore(im);
            return (void *)(curr);
        }
        prev = curr;
        curr = curr->next;
    }
    restore(im);
    return (void *)SYSERR;
}

/**
 * @ingroup memory_mgmt
 *
 * Free a PSRAM block, returning it to the free list.  Verbatim clone of
 * memfree() over @c psramlist / psminheap / psmaxheap.
 */
syscall psram_freemem(void *memptr, uintptr_t nbytes)
{
    register struct memblock *block, *next, *prev;
    irqmask im;
    ulong top;

    if ((0 == nbytes)
        || ((ulong)memptr < (ulong)psminheap)
        || ((ulong)memptr > (ulong)psmaxheap))
    {
        return SYSERR;
    }

    nbytes = (ulong)roundmb(nbytes);
    block = (struct memblock *)memptr;

    im = disable();

    prev = &psramlist;
    next = psramlist.next;
    while ((next != NULL) && (next < block))
    {
        prev = next;
        next = next->next;
    }

    if (prev == &psramlist)
    {
        top = (ulong)NULL;
    }
    else
    {
        top = (ulong)prev + prev->length;
    }

    /* Ensure the new block does not overlap the previous/next blocks */
    if (((prev != &psramlist) && ((ulong)block < top))
        || ((next != NULL) && ((ulong)block + nbytes > (ulong)next)))
    {
        restore(im);
        return SYSERR;
    }

    psramlist.length += nbytes;

    /* Coalesce with the previous block or link as a new node */
    if (top == (ulong)block)
    {
        prev->length += nbytes;
        block = prev;
    }
    else
    {
        block->next = next;
        block->length = nbytes;
        prev->next = block;
    }

    /* Coalesce with the next block if adjacent */
    if (((ulong)block + block->length) == (ulong)next)
    {
        block->length += next->length;
        block->next = next->next;
    }
    restore(im);
    return OK;
}

/**
 * @ingroup memory_mgmt
 *
 * Total number of bytes currently free in the PSRAM list.
 */
uintptr_t psram_free_bytes(void)
{
    struct memblock *memptr;
    uintptr_t free_mem = 0;

    for (memptr = psramlist.next; memptr != NULL; memptr = memptr->next)
    {
        free_mem += memptr->length;
    }
    return free_mem;
}

/**
 * @ingroup memory_mgmt
 *
 * Request PSRAM storage, recording accounting information in a preceding
 * memblock (body identical to libxc/malloc.c, PSRAM backend).
 */
void *psram_malloc(uintptr_t nbytes)
{
    struct memblock *pmem;

    if (0 == nbytes)
    {
        return NULL;
    }

    nbytes += sizeof(struct memblock);

    pmem = (struct memblock *)psram_getmem(nbytes);
    if (SYSERR == (intptr_t)pmem)
    {
        return NULL;
    }

    pmem->next = pmem;
    pmem->length = nbytes;

    return (void *)(pmem + 1);
}

/**
 * @ingroup memory_mgmt
 *
 * Free a PSRAM block allocated by psram_malloc (body identical to
 * libxc/free.c, PSRAM backend).
 */
void psram_free(void *pmem)
{
    struct memblock *block;

    /* ISO C: free(NULL) is a no-op (otherwise block-- reads ~0-12) */
    if (NULL == pmem)
    {
        return;
    }

    block = (struct memblock *)pmem;
    block--;

    /* Don't free if we fail the basic checks */
    if (block->next != block)
    {
        return;
    }

    psram_freemem((void *)block, block->length);
}

/**
 * @ingroup memory_mgmt
 *
 * Zero-initialized PSRAM allocation (body identical to calloc() in
 * libxc, PSRAM backend).
 */
void *psram_calloc(size_t nmemb, size_t size)
{
    size_t s;
    char *ptr;

    s = nmemb * size;
    ptr = (char *)psram_malloc(s);
    if (NULL == ptr)
    {
        return NULL;
    }
    memset(ptr, 0, s);
    return (ptr);
}

/**
 * @ingroup memory_mgmt
 *
 * Resize a block previously returned by psram_malloc()/psram_calloc().
 * As with realloc() over the L2MEM heap, the memblock allocator has no
 * in-place grow: allocate, copy the common prefix, free the old block.
 */
void *psram_realloc(void *pmem, size_t size)
{
    struct memblock *block;
    size_t oldsize;
    void *newptr;

    if (NULL == pmem)
    {
        return psram_malloc(size);
    }
    if (0 == size)
    {
        psram_free(pmem);
        return NULL;
    }

    block = (struct memblock *)pmem;
    block--;

    /* Same validity marker psram_free() relies on. */
    if (block->next != block)
    {
        return NULL;
    }

    oldsize = (size_t)block->length - sizeof(struct memblock);

    newptr = psram_malloc(size);
    if (NULL == newptr)
    {
        return NULL;
    }

    memcpy(newptr, pmem, (oldsize < size) ? oldsize : size);
    psram_free(pmem);
    return newptr;
}
