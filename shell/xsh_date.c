/*
 * @file     xsh_date.c
 *
 */
/* Embedded Xinu, Copyright (C) 2008.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <date.h>

/**
 * @ingroup shell
 *
 * Shell command (date).
 * @param nargs  number of arguments in args array
 * @param args   array of arguments
 * @return OK for success, SYSERR for syntax error
 */
shellcmd xsh_date(int nargs, char *args[])
{


    /* Output help, if '--help' argument was supplied */
    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s\n\n", args[0]);
        printf("Description:\n");
        printf("\tDisplays current time\n");
        printf("Options:\n");
        printf("\t--help\tdisplay this help and exit\n");
        return OK;
    }

    /* Check for correct number of arguments */
    if (nargs > 1)
    {
        fprintf(stderr, "%s: too many arguments\n", args[0]);
        fprintf(stderr, "Try '%s --help' for more information\n",
                args[0]);
        return SYSERR;
    }
    if (nargs < 1)
    {
        fprintf(stderr, "%s: too few arguments\n", args[0]);
        fprintf(stderr, "Try '%s --help' for more information\n",
                args[0]);
        return SYSERR;
    }

    int dt = get_datetime();

    /* Con el RTC sin ajustar clktime vale 0 y convertDate() devuelve índices
     * fuera de month_names[]/weekday_names[]: strlen() sobre un puntero
     * basura y el kernel cae en un trap de carga. */
    if (dt < SECS_IN_YEAR)
    {
        printf("date: reloj en 0 (%d s); use 'date <mm dd yyyy hh mm ss>'\n",
               dt);
        return OK;
    }

    printDate(dt);

    return OK;
}
