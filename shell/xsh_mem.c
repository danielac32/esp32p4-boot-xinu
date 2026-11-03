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

static void memWalk(const char *tag, struct memblock *head, ulong lo, ulong hi,
                    bool dump, ulong *freeOut, ulong *blocksOut,
                    ulong *largestOut);
static void memRow(const char *name, struct memblock *head, ulong lo, ulong hi,
                   bool dump);

/**
 * Recorrido con cota y comprobaciones de una lista de bloques libres.
 *
 * Xinu recorre estas listas a ciegas: si un nodo se pisa (la direccion de
 * `next` deja de ser un bloque del heap), el bucle se va fuera del heap o da
 * vueltas y la hebra que pregunta se cuelga sin decir nada.  Aqui cada nodo se
 * valida antes de usarlo: dentro del heap, alineado a memblock, con longitud
 * multiple de 8 y mayor que la cabecera, sin pasarse del final y sin solapar al
 * siguiente.  Ademas se contrasta la cuenta que lleva la propia cabecera
 * (head->length) contra la suma real de los nodos: si no cuadran, el contador
 * del heap esta roto aunque la lista se vea bien.
 *
 * Imprime en stderr la anomalia detectada y corta el recorrido; con `dump`
 * tambien vuelca cada bloque (lo que hace 'mem -l').
 */
#define MEM_WALK_MAX 512U

static void memWalk(const char *tag, struct memblock *head, ulong lo,
                    ulong hi, bool dump, ulong *freeOut, ulong *blocksOut,
                    ulong *largestOut)
{
    struct memblock *m;
    ulong n = 0;
    ulong prevEnd = 0;

    *freeOut = 0;
    *blocksOut = 0;
    *largestOut = 0;

    for (m = head->next; m != NULL; m = m->next)
    {
        if (++n > MEM_WALK_MAX)
        {
            fprintf(stderr,
                    "%s: lista con mas de %u nodos, corte en 0x%08lX\n",
                    tag, MEM_WALK_MAX, (ulong)m);
            return;
        }
        if ((ulong)m < lo || (ulong)m >= hi)
        {
            fprintf(stderr,
                    "%s: nodo %lu fuera del heap: 0x%08lX (%08lX..%08lX)\n",
                    tag, (unsigned long)n, (ulong)m, lo, hi);
            return;
        }
        if (0 != ((ulong)m & MEMORY_DMBDEC)
            || m->length < sizeof(struct memblock)
            || 0 != ((ulong)m->length & MEMORY_DMBDEC))
        {
            fprintf(stderr, "%s: nodo %lu desalineado o corto: 0x%08lX len=%lu\n",
                    tag, (unsigned long)n, (ulong)m,
                    (unsigned long)m->length);
            return;
        }
        if ((ulong)m + m->length > hi)
        {
            fprintf(stderr,
                    "%s: nodo %lu en 0x%08lX len=%lu se pasa del fin %08lX\n",
                    tag, (unsigned long)n, (ulong)m,
                    (unsigned long)m->length, hi);
            return;
        }
        if (0 != prevEnd && (ulong)m < prevEnd)
        {
            fprintf(stderr, "%s: SOLAPE: nodo %lu en 0x%08lX, el anterior "
                    "termina en 0x%08lX\n", tag, (unsigned long)n, (ulong)m,
                    prevEnd);
            return;
        }

        if (dump)
        {
            printf("  0x%08lX  %lu bytes\n", (ulong)m,
                   (unsigned long)m->length);
        }

        *freeOut += m->length;
        (*blocksOut)++;
        if (m->length > *largestOut)
        {
            *largestOut = m->length;
        }
        prevEnd = (ulong)m + m->length;
    }

    if (head->length != *freeOut)
    {
        fprintf(stderr, "%s: descuadre: la cabecera apunta %lu libres y la "
                "lista suma %lu\n", tag, (unsigned long)head->length,
                (unsigned long)*freeOut);
    }
}

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

    /* Totales: el heap de L2MEM vive en [memheap, platform.maxaddr) y el de
     * PSRAM en [psminheap, psmaxheap); memRow los deduce de esos extremos. */
    printf("%-6s %10s %10s %10s %8s %8s\n",
           "HEAP", "TOTAL", "FREE", "USED", "BLOCKS", "LARGEST");
    printf("%-6s %10s %10s %10s %8s %8s\n",
           "------", "----------", "----------", "----------",
           "--------", "--------");

    if (heap)
    {
        memRow("L2MEM", &memlist, (ulong)memheap,
               (ulong)platform.maxaddr, list);
    }
    if (psram)
    {
        memRow("PSRAM", &psramlist, (ulong)psminheap, (ulong)psmaxheap, list);
    }

    return OK;
}

/* Print one summary row for the list headed by @p head, then (con dump) el
 * vuelco de sus bloques.  El total del heap es hi-lo, que es como lo calculaba
 * la version original: L2MEM en [memheap, platform.maxaddr) y PSRAM en
 * [psminheap, psmaxheap). */
static void memRow(const char *name, struct memblock *head, ulong lo, ulong hi,
                   bool dump)
{
    ulong freeBytes, blocks, largest;

    if (0 == lo || 0 == hi || hi <= lo)
    {
        printf("%-6s %10lu %10lu %10lu %8lu %8lu  sin inicializar\n",
               name, 0UL, 0UL, 0UL, 0UL, 0UL);
        return;
    }

    memWalk(name, head, lo, hi, FALSE, &freeBytes, &blocks, &largest);
    printf("%-6s %10lu %10lu %10lu %8lu %8lu\n",
           name, (unsigned long)(hi - lo), (unsigned long)freeBytes,
           (unsigned long)(hi - lo - freeBytes), (unsigned long)blocks,
           (unsigned long)largest);

    if (dump)
    {
        printf("%s free list:\n", name);
        memWalk(name, head, lo, hi, TRUE, &freeBytes, &blocks, &largest);
    }
}
