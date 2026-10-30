/**
 * @file     xsh_touch.c
 *
 * Shell command (touchpanel): poll the touch panel and print contact points.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <kernel.h>
#include <device.h>
#include <clock.h>
#include <touch.h>

/**
 * @ingroup shell
 *
 * Shell command (touchpanel [SAMPLES]).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_touchpanel(int nargs, char *args[])
{
    int samples = 100;
    int k;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s [SAMPLES]\n\n", args[0]);
        printf("Description:\n");
        printf("\tPolls the touch panel SAMPLES times (default 100,\n");
        printf("\t~20 ms apart) and prints any contact points.\n");
        printf("Options:\n");
        printf("\t--help\tdisplay this help and exit\n");
        return OK;
    }

    if (nargs == 2)
    {
        samples = atoi(args[1]);
    }
    else if (nargs > 2)
    {
        fprintf(stderr, "%s: too many arguments\n", args[0]);
        return SYSERR;
    }

    if (samples <= 0)
    {
        fprintf(stderr, "%s: invalid sample count\n", args[0]);
        return SYSERR;
    }

    printf("%s: sondeando el táctil (%d muestras)...\n", args[0], samples);

    for (k = 0; k < samples; k++)
    {
        touch_point_t pts[5];
        int n = control(TOUCH0, TOUCH_CTRL_READ, (long)pts, 5);
        if (n > 0)
        {
            int j;
            printf("  %d punto(s):", n);
            for (j = 0; j < n && j < 5; j++)
            {
                printf(" (%u,%u,id=%u)", (unsigned)pts[j].x,
                       (unsigned)pts[j].y, (unsigned)pts[j].id);
            }
            printf("\n");
        }
        mdelay(20);
    }

    printf("%s: fin\n", args[0]);
    return OK;
}
