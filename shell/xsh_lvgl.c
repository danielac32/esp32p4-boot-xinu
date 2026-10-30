/*
 * @file xsh_lvgl.c
 *
 * Shell command: run the LVGL demo loop.  It drives LVGL's tick and timer
 * handler (which renders through the LCD device and reads the touch panel),
 * yielding between frames so the rest of the system keeps running.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <kernel.h>
#include <thread.h>
#include <device.h>
#include <lvgl.h>

/**
 * @ingroup shell
 *
 * Shell command (lvgl [SECONDS]).
 * @param nargs  number of arguments in args array
 * @param args   array of arguments
 * @return OK for success, SYSERR for syntax error
 */
shellcmd xsh_lvgl(int nargs, char *args[])
{
    int secs = 30;
    long i, frames;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s [SECONDS]\n\n", args[0]);
        printf("Description:\n");
        printf("\tRuns the LVGL demo for SECONDS (default 30).\n");
        printf("\tTouch the panel to interact with the demo.\n");
        return OK;
    }

    if (nargs > 2)
    {
        fprintf(stderr, "%s: too many arguments\n", args[0]);
        fprintf(stderr, "Try '%s --help' for more information\n", args[0]);
        return SYSERR;
    }

    if (nargs == 2)
    {
        if (sscanf(args[1], "%d", &secs) != 1 || secs <= 0)
        {
            fprintf(stderr, "%s: invalid number of seconds \"%s\"\n",
                    args[0], args[1]);
            return SYSERR;
        }
    }

    if (control(LVGL0, LVGL_CTRL_INIT, 0, 0) == SYSERR)
    {
        fprintf(stderr, "%s: LVGL no disponible\n", args[0]);
        return SYSERR;
    }

    printf("%s: demo LVGL durante %d s (toca la pantalla)...\n",
           args[0], secs);
    frames = (long) secs * 200;
    for (i = 0; i < frames; i++)
    {
        control(LVGL0, LVGL_CTRL_TICK, 0, 0);
        control(LVGL0, LVGL_CTRL_TASK, 0, 0);
        sleep(5);
    }
    printf("%s: fin\n", args[0]);
    return OK;
}
