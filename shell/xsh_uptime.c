/**
 * @file     xsh_uptime.c
 *
 * Shell command (uptime): show how long the system has been running.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <clock.h>

/**
 * @ingroup shell
 *
 * Shell command (uptime).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_uptime(int nargs, char *args[])
{
    ulong secs;
    ulong days;
    ulong hours;
    ulong mins;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s\n\n", args[0]);
        printf("Description:\n");
        printf("\tShows the system uptime and timer resolution.\n");
        printf("Options:\n");
        printf("\t--help\tdisplay this help and exit\n");
        return OK;
    }

    if (nargs > 1)
    {
        fprintf(stderr, "%s: too many arguments\n", args[0]);
        fprintf(stderr, "Try '%s --help' for more information\n", args[0]);
        return SYSERR;
    }

    secs = (ulong)clktime;
    days = secs / 86400UL;
    secs %= 86400UL;
    hours = secs / 3600UL;
    secs %= 3600UL;
    mins = secs / 60UL;
    secs %= 60UL;

    printf("up %lu day(s), %02lu:%02lu:%02lu (%lu s, %lu tick(s))\n",
           days, hours, mins, secs,
           (unsigned long)clktime, (unsigned long)clkticks);

    return OK;
}
