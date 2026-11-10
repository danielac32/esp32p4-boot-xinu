/**
 * @file taskmon.c
 *
 * Implementacion del monitor de tareas expuesto a apps ELF.  Recorre
 * thrtab[] como hace el comando ps, pero en una sola llamada que rellena
 * una lista plana, pensada para que un ELF la pinte en el panel.
 *
 * No hay nada de PSRAM ni de ventana directa: es solo lectura de estado del
 * kernel.  El CPU% en crudo va en cpu_ticks (ticks de 1 ms); el porcentaje
 * lo calcula el consumidor comparando dos muestras.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <thread.h>
#include <string.h>
#include <stack.h>
#include <taskmon.h>

int taskmon_get(struct taskmon_row *out, int max)
{
    int i;
    int n = 0;

    if (NULL == out || max <= 0)
    {
        return 0;
    }

    for (i = 0; i < NTHREAD && n < max; i++)
    {
        struct thrent *thrptr = &thrtab[i];

        if (THRFREE == thrptr->state)
        {
            continue;
        }

        out[n].tid = i;
        out[n].state = (int)thrptr->state;
        out[n].prio = thrptr->prio;
        out[n].parent = thrptr->parent;
        out[n].cpu_ticks = (ulong)thrptr->cpu_ticks;
        out[n].born_ticks = (ulong)thrptr->born_ticks;
        out[n].stk_peak = (ulong)stack_peak(i);
        out[n].stk_total = (ulong)stack_total(i);
        out[n].elf_task = thrptr->elf_task ? 1 : 0;
        strlcpy(out[n].name, thrptr->name, TASKMON_NAME);

        n++;
    }
    return n;
}
