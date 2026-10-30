/**
 * @file xsh_stack.c
 *
 * Comando de shell (stack): muestra el high-water mark de pila por tarea.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <thread.h>
#include <shell.h>
#include <stack.h>

static void stack_usage(const char *cmd)
{
    printf("Usage: %s [-r] [tid]\n\n", cmd);
    printf("Description:\n");
    printf("\tShows each thread's peak stack usage (high-water mark).\n");
    printf("Options:\n");
    printf("\t-r\treset the peak counters (all threads, or just tid)\n");
    printf("\ttid\tshow only that thread\n");
    printf("\t--help\tdisplay this help and exit\n");
}

static void stack_header(void)
{
    printf("%3s %-16s %8s %8s %8s %5s %4s\n",
           "TID", "NAME", "TOTAL", "PEAK", "FREE", "USE%", "OVF");
    printf("%3s %-16s %8s %8s %8s %5s %4s\n",
           "---", "----------------", "--------", "--------", "--------",
           "-----", "----");
}

static void stack_row(tid_typ tid)
{
    struct thrent *thrptr = &thrtab[tid];
    uint32_t total;
    uint32_t peak;
    uint32_t freeb;
    uint32_t pct = 0;

    if (thrptr->state == THRFREE)
    {
        return;
    }

    total = stack_total(tid);
    peak = stack_peak(tid);
    freeb = stack_free(tid);

    if (total > 0)
    {
        pct = (peak * 100) / total;
    }

    printf("%3d %-16s %8lu %8lu %8lu %4lu%% %4s\n",
           tid, thrptr->name,
           (ulong)total, (ulong)peak, (ulong)freeb, (ulong)pct,
           stack_overflow(tid) ? "YES" : "no");
}

/**
 * @ingroup shell
 *
 * Shell command (stack).
 */
shellcmd xsh_stack(int nargs, char *args[])
{
    bool reset = FALSE;
    tid_typ only = BADTID;
    int i;

    for (i = 1; i < nargs; i++)
    {
        if (strcmp(args[i], "--help") == 0)
        {
            stack_usage(args[0]);
            return 0;
        }
        else if (strcmp(args[i], "-r") == 0)
        {
            reset = TRUE;
        }
        else if (args[i][0] == '-')
        {
            fprintf(stderr, "%s: unknown option '%s'\n", args[0], args[i]);
            return 1;
        }
        else
        {
            only = (tid_typ)atoi(args[i]);
            if (only < 0 || only >= NTHREAD || thrtab[only].state == THRFREE)
            {
                fprintf(stderr, "%s: invalid tid '%s'\n", args[0], args[i]);
                return 1;
            }
        }
    }

    if (reset)
    {
        if (only == BADTID)
        {
            stack_reset_all();
        }
        else
        {
            stack_reset(only);
        }
        return 0;
    }

    stack_header();

    if (only != BADTID)
    {
        stack_row(only);
        return 0;
    }

    for (i = 0; i < NTHREAD; i++)
    {
        stack_row((tid_typ)i);
    }

    return 0;
}
