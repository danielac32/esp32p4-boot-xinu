/**
 * @file     xsh_mon.c
 *
 * Shell command (mon): list the kernel monitors currently in use.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <monitor.h>

/**
 * @ingroup shell
 *
 * Shell command (mon).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_mon(int nargs, char *args[])
{
    int i;
    int used = 0;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s\n\n", args[0]);
        printf("Description:\n");
        printf("\tLists the monitors currently in use.\n");
        printf("Options:\n");
        printf("\t--help\tdisplay this help and exit\n");
        return OK;
    }

    if (nargs > 1)
    {
        fprintf(stderr, "%s: too many arguments\n", args[0]);
        return SYSERR;
    }

    printf("%4s %-6s %7s %6s %5s\n", "MON", "STATE", "OWNER", "COUNT", "SEM");

    for (i = 0; i < NMON; i++)
    {
        if (montab[i].state == MUSED)
        {
            if (montab[i].owner == NOOWNER)
            {
                printf("%4d %-6s %7s %6u %5d\n", i, "used", "-",
                       (unsigned)montab[i].count, (int)montab[i].sem);
            }
            else
            {
                printf("%4d %-6s %7d %6u %5d\n", i, "used",
                       (int)montab[i].owner, (unsigned)montab[i].count,
                       (int)montab[i].sem);
            }
            used++;
        }
    }

    if (used == 0)
    {
        printf("(no monitors in use)\n");
    }

    return OK;
}
