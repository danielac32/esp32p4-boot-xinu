/**
 * @file memory.h
 * Definitions for kernel memory allocator and maintenance.
 *
 */
/* Embedded Xinu, Copyright (C) 2009.  All rights reserved. */

#ifndef _MEMORY_H_
#define _MEMORY_H_

#include <stddef.h>
#include <stdint.h>

#define MEMORY_DMBDEC (2*sizeof(uintptr_t)-1)
/* roundmb - round address up to size of memblock  */
#define roundmb(x)  (void *)( (MEMORY_DMBDEC + (ulong)(x)) & ~MEMORY_DMBDEC )
/* truncmb - truncate address down to size of memblock */
#define truncmb(x)  (void *)( ((ulong)(x)) & ~MEMORY_DMBDEC )

/**
 * @ingroup memory_mgmt
 *
 * Frees memory allocated with stkget().
 *
 * @param p
 *      Pointer to the topmost (highest address) word of the allocated stack (as
 *      returned by stkget()).
 * @param len
 *      Size of the allocated stack, in bytes.  (Same value passed to stkget().)
 */
#define stkfree(p, len) memfree((void *)((ulong)(p)         \
                                - (ulong)roundmb(len)       \
                                + (ulong)sizeof(ulong)),    \
                                (ulong)roundmb(len))


/**
 * Structure for a block of memory.
 */
struct memblock
{
    struct memblock *next;          /**< pointer to next memory block       */
    uintptr_t length;                    /**< size of memory block (with struct) */
};

extern struct memblock memlist;     /**< head of free memory list           */
extern struct memblock psramlist;   /**< head of free PSRAM list            */
extern void *psminheap;             /**< start of PSRAM heap                */
extern void *psmaxheap;             /**< end of PSRAM heap                  */

/* Other memory data */

extern void *_end;              /**< linker provides end of image           */
extern void *_etext;            /**< linker provides end of text segment    */
extern void *memheap;           /**< bottom of heap                         */

/* Memory function prototypes */
void *memget(uintptr_t);
syscall memfree(void *, uintptr_t);
void *stkget(uintptr_t);

/* PSRAM allocator (second free list over [_psram_free_start,_psram_free_end)) */
void psram_meminit(void);
void *psram_getmem(uintptr_t);
syscall psram_freemem(void *, uintptr_t);
uintptr_t psram_free_bytes(void);
void *psram_malloc(uintptr_t);
void psram_free(void *);
void *psram_calloc(size_t, size_t);
void *psram_realloc(void *, size_t);

#endif                          /* _MEMORY_H_ */
