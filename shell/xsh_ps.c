/**
 * @file     xsh_ps.c 
 *
 */
/* Embedded Xinu, Copyright (C) 2009.  All rights reserved. */

#include <stddef.h>
#include <thread.h>
#include <stdio.h>
#include <string.h>
#include <clock.h>
#include <stack.h>

/**
 * @ingroup shell
 *
 * Shell command (ps) outputs thread table information.
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_ps(int nargs, char *args[])
{
    struct thrent *thrptr;      /* pointer to thread entry  */
    int i;                      /* temp variable            */

    /* readable names for PR* status in thread.h */
    static const char * const pstnams[] = {
        "curr ", "free ", "ready", "recv ",
        "sleep", "susp ", "wait ", "rtim ", "migr "
    };

    /* Output help, if '--help' argument was supplied */
    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s\n\n", args[0]);
        printf("Description:\n");
        printf("\tDisplays a table of running threads.\n");
        printf("Options:\n");
        printf("\t--help\t display this help and exit\n");

        return 0;
    }

    /* Check for correct number of arguments */
    if (nargs > 1)
    {
        fprintf(stderr, "%s: too many arguments\n", args[0]);
        fprintf(stderr, "Try '%s --help' for more information\n",
                args[0]);
        return 1;
    }

    /* Output thread table header */
    printf("%3s %-16s %5s %4s %4s %5s %-17s\n",
           "TID", "NAME", "STATE", "PRIO", "PPID", "CPU%",
           "STK peak/total");

    printf("%3s %-16s %5s %4s %4s %5s %-17s\n",
           "---", "----------------", "-----", "----", "----",
           "-----", "---------------");

    /* Output information for each thread */
    for (i = 0; i < NTHREAD; i++)
    {
        uint32_t life;
        uint32_t pct = 0;

        thrptr = &thrtab[i];
        if (thrptr->state == THRFREE)
        {
            continue;
        }

        /* Porcentaje de CPU desde que nació la tarea.  Se evita la
         * división de 64 bits (el target no enlaza libgcc). */
        life = (uint32_t)clktotal - thrptr->born_ticks;
        if (life >= 100)
        {
            pct = thrptr->cpu_ticks / (life / 100);
        }
        else if (life > 0)
        {
            pct = (thrptr->cpu_ticks * 100) / life;
        }
        if (pct > 100)
        {
            pct = 100;
        }

        printf("%3d %-16s %s %4d %4d %4lu%% %8lu/%-8lu\n",
               i, thrptr->name,
               (thrptr->state >= 1 && thrptr->state <= 9)
                   ? pstnams[(int)thrptr->state - 1] : "?????",
               thrptr->prio, thrptr->parent,
               (ulong)pct,
               (ulong)stack_peak(i), (ulong)stack_total(i));
    }

    return 0;
}
