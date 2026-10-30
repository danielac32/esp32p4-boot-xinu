/**
 * @file     xsh_mem.c
 *
 * Shell command (mem): report the two Xinu free lists, L2MEM (the classic
 * kernel heap used by malloc()/free()/calloc()/realloc()) and PSRAM (the
 * second memblock list behind psram_malloc()/psram_free()).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <memory.h>
#include <platform.h>

static void mem_report(const char *name, struct memblock *head, ulong total);

/**
 * @ingroup shell
 *
 * Shell command (mem).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_mem(int nargs, char *args[])
{
    bool list = FALSE;
    bool heap = TRUE;
    bool psram = TRUE;
    int i;
    ulong l2total, pstotal;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s [-l] [-k] [-p]\n\n", args[0]);
        printf("Description:\n");
        printf("\tReports the Xinu memory free lists.\n");
        printf("\tDefault: summary of both heaps.\n");
        printf("Options:\n");
        printf("\t-l\talso list every free block (address + size)\n");
        printf("\t-k\tonly the L2MEM heap (malloc/free backend)\n");
        printf("\t-p\tonly the PSRAM heap (psram_malloc backend)\n");
        printf("\t--help\tdisplay this help and exit\n");
        return OK;
    }

    for (i = 1; i < nargs; i++)
    {
        if (0 == strcmp(args[i], "-l"))
        {
            list = TRUE;
        }
        else if (0 == strcmp(args[i], "-k"))
        {
            heap = TRUE;
            psram = FALSE;
        }
        else if (0 == strcmp(args[i], "-p"))
        {
            psram = TRUE;
            heap = FALSE;
        }
        else
        {
            fprintf(stderr, "%s: invalid option '%s'\n", args[0], args[i]);
            fprintf(stderr, "Try '%s --help' for more information\n", args[0]);
            return SYSERR;
        }
    }

    /* Total sizes: L2MEM heap lives in [memheap, platform.maxaddr); PSRAM in
     * [psminheap, psmaxheap). */
    l2total = (ulong)platform.maxaddr - (ulong)memheap;
    pstotal = (psmaxheap != NULL) ? (ulong)psmaxheap - (ulong)psminheap : 0;

    printf("%-6s %10s %10s %10s %8s %8s\n",
           "HEAP", "TOTAL", "FREE", "USED", "BLOCKS", "LARGEST");
    printf("%-6s %10s %10s %10s %8s %8s\n",
           "------", "----------", "----------", "----------",
           "--------", "--------");

    if (heap)
    {
        mem_report("L2MEM", &memlist, l2total);
    }
    if (psram)
    {
        mem_report("PSRAM", &psramlist, pstotal);
    }

    if (list)
    {
        struct memblock *m;

        if (heap)
        {
            printf("\nL2MEM free list:\n");
            for (m = memlist.next; m != NULL; m = m->next)
            {
                printf("  0x%08lX  %lu bytes\n",
                       (ulong)m, (unsigned long)m->length);
            }
        }
        if (psram)
        {
            printf("\nPSRAM free list:\n");
            for (m = psramlist.next; m != NULL; m = m->next)
            {
                printf("  0x%08lX  %lu bytes\n",
                       (ulong)m, (unsigned long)m->length);
            }
        }
    }

    return OK;
}

/* Print one summary row for the list headed by @p head.  @p name must be at
 * most 6 characters so the columns line up. */
static void mem_report(const char *name, struct memblock *head, ulong total)
{
    struct memblock *m;
    ulong free_bytes = 0;
    ulong blocks = 0;
    ulong largest = 0;

    for (m = head->next; m != NULL; m = m->next)
    {
        free_bytes += m->length;
        blocks++;
        if (m->length > largest)
        {
            largest = m->length;
        }
    }

    printf("%-6s %10lu %10lu %10lu %8lu %8lu\n",
           name, (unsigned long)total, (unsigned long)free_bytes,
           (unsigned long)(total - free_bytes), (unsigned long)blocks,
           (unsigned long)largest);
}
