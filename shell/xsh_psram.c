/**
 * @file     xsh_psram.c
 *
 * Shell command (psram): estado del heap de la PSRAM.
 *
 * El heap de PSRAM es la segunda lista de Xinu (system/psrammem.c): un
 * memblock por nodo sobre [_psram_free_start, _psram_free_end), y es el que
 * usan psram_malloc()/psram_free() para los buefers grandes (anillos de audio,
 * ELF cargables).  `mem' y `memdump' miran el heap de L2MEM y cualquier
 * direccion, pero nadie enseñaba esta lista: sin ver sus bloques no se
 * distingue "no hay sitio" de "la lista esta pisada", y esa segunda causa es
 * la que se pago con un NULL habiendo 31 MB libres.
 *
 * El recorrido va cota arriba (igual que en psrammem.c) y corta en 512 nodos:
 * una lista pisada no debe congelar la hebra que la consulta.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <memory.h>
#include <shell.h>

/** Cota del recorrido, la misma que psram_free_bytes(). */
#define PSRAM_CMD_WALK_MAX  512U

static void psram_human(unsigned long bytes)
{
    if (bytes >= 1048576UL)
    {
        printf("%lu.%lu MiB (%lu B)", bytes / 1048576UL,
               (bytes % 1048576UL) / 104857UL, bytes);
    }
    else
    {
        printf("%lu.%lu KiB (%lu B)", bytes / 1024UL,
               (bytes % 1024UL) * 10UL / 1024UL, bytes);
    }
}

/** Imprime la lista de bloques libres; devuelve el numero de nodos vistos. */
static unsigned long psram_show_free(int *bad)
{
    struct memblock *memptr;
    uintptr_t prevaddr = 0;
    unsigned long seen = 0;
    unsigned long sum = 0;
    unsigned long largest = 0;
    uintptr_t lo = (uintptr_t)psminheap;
    uintptr_t hi = (uintptr_t)psmaxheap;

    *bad = 0;
    printf("  #  direccion    tamanno\n");
    for (memptr = psramlist.next; memptr != NULL;
         prevaddr = (uintptr_t)memptr, memptr = memptr->next)
    {
        if (++seen > PSRAM_CMD_WALK_MAX)
        {
            printf("  ... corte en %u nodos (lista pisada?) en 0x%08x\n",
                   PSRAM_CMD_WALK_MAX, (unsigned)(uintptr_t)memptr);
            *bad = 1;
            break;
        }

        /* Sano = dentro del rango del heap y detras del nodo anterior.  Con
         * la cabecera no se compara: psramlist vive en L2MEM (0x4ff2..) y los
         * nodos en PSRAM (0x48..), asi que el primero siempre sale "antes". */
        if ((uintptr_t)memptr < lo || (uintptr_t)memptr + memptr->length > hi
            || ((uintptr_t)memptr <= prevaddr && prevaddr != 0))
        {
            printf("  %2lu 0x%08x  %10lu  <-- fuera de rango o desordenado\n",
                   seen, (unsigned)(uintptr_t)memptr,
                   (unsigned long)memptr->length);
            *bad = 1;
            break;
        }

        sum += (unsigned long)memptr->length;
        if (memptr->length > largest)
            largest = (unsigned long)memptr->length;

        printf("  %2lu 0x%08x  %10lu\n", seen, (unsigned)(uintptr_t)memptr,
               (unsigned long)memptr->length);
    }

    printf("  bloques: %lu   suma libre: ", seen);
    psram_human(sum);
    printf("   mayor bloque: ");
    psram_human(largest);
    printf("\n");

    if (sum != (unsigned long)psramlist.length)
    {
        printf("  ALERTA: la suma de nodos (%lu) no coincide con psramlist.length"
               " (%lu)\n", sum, (unsigned long)psramlist.length);
        *bad = 1;
    }
    return seen;
}

/**
 * @ingroup shell
 *
 * Shell command (psram): estado del heap de PSRAM.
 *
 * @param nargs numero de argumentos en args
 * @param args  vector de argumentos; "free" lista solo los bloques libres
 * @return SHELL_OK
 */
shellcmd xsh_psram(int nargs, char *args[])
{
    uintptr_t lo = (uintptr_t)psminheap;
    uintptr_t hi = (uintptr_t)psmaxheap;
    unsigned long total;
    unsigned long freeb;
    int bad = 0;

    if (nargs > 1 && 0 == strcmp(args[1], "--help"))
    {
        printf("Uso:\n");
        printf("\tpsram [free]\n");
        printf("Descripcion:\n");
        printf("\tEnseña el heap de la PSRAM: el rango que gestiona el kernel\n");
        printf("\t(sin la RAM del core LP, que ulp.ld aparca al final), lo\n");
        printf("\tlibre, lo usado y los bloques libres de psramlist.\n");
        printf("\tfree    solo la lista de bloques libres\n");
        printf("\tPara volcar el contenido de una direccion ya esta `memdump'.\n");
        return SHELL_OK;
    }

    if (0 == psramlist.length && NULL == psramlist.next)
    {
        printf("psram: el heap no esta inicializado (psramInit no corro?)\n");
        return SHELL_OK;
    }

    total = (unsigned long)(hi - lo);

    if (nargs < 2 || 0 != strcmp(args[1], "free"))
    {
        freeb = (unsigned long)psram_free_bytes();

        printf("psram heap\n");
        printf("  rango:       0x%08x .. 0x%08x  (", (unsigned)lo,
               (unsigned)hi);
        psram_human(total);
        printf(")\n");
        printf("  libre:      ");
        psram_human(freeb);
        printf("\n");
        printf("  usado:      ");
        psram_human(total - freeb);
        printf("\n");
        printf("  cabecera:   next=0x%08x length=%lu\n",
               (unsigned)(uintptr_t)psramlist.next,
               (unsigned long)psramlist.length);
    }

    psram_show_free(&bad);

    if (bad)
    {
        printf("psram: lista inconsistente, no fio de estos numeros\n");
    }
    return SHELL_OK;
}
