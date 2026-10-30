/**
 * @file     xsh_sem.c
 *
 * Shell command (sem): list the kernel semaphores currently in use.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <semaphore.h>

/**
 * @ingroup shell
 *
 * Shell command (sem).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_sem(int nargs, char *args[])
{
    int i;
    int used = 0;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s\n\n", args[0]);
        printf("Description:\n");
        printf("\tLists the semaphores currently in use.\n");
        printf("Options:\n");
        printf("\t--help\tdisplay this help and exit\n");
        return OK;
    }

    if (nargs > 1)
    {
        fprintf(stderr, "%s: too many arguments\n", args[0]);
        return SYSERR;
    }

    printf("%4s %-6s %6s %6s\n", "SEM", "STATE", "COUNT", "QUEUE");

    for (i = 0; i < NSEM; i++)
    {
        if (semtab[i].state == SUSED)
        {
            printf("%4d %-6s %6d %6d\n", i, "used",
                   semtab[i].count, (int)semtab[i].queue);
            used++;
        }
    }

    if (used == 0)
    {
        printf("(no semaphores in use)\n");
    }

    return OK;
}
