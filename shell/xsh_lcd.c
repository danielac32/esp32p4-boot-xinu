/*
 * @file xsh_lcd.c
 *
 * Shell command: exercise the ST7796 LCD (demo pattern) and poll the touch
 * controller.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <clock.h>
#include <device.h>
#include <lcd.h>
#include <touch.h>

/**
 * @ingroup shell
 *
 * Shell command (lcd).
 * @param nargs  number of arguments in args array
 * @param args   array of arguments
 * @return OK for success, SYSERR for syntax error
 */
shellcmd xsh_lcd(int nargs, char *args[])
{
    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s [touch]\n\n", args[0]);
        printf("Description:\n");
        printf("\tDraws the bring-up pattern on the LCD.\n");
        printf("\tWith 'touch', also polls the touch panel for ~5 s.\n");
        return OK;
    }

    if (nargs > 2)
    {
        fprintf(stderr, "%s: too many arguments\n", args[0]);
        fprintf(stderr, "Try '%s --help' for more information\n", args[0]);
        return SYSERR;
    }

    if (nargs == 2 && strcmp(args[1], "touch") == 0)
    {
        touch_point_t pts[5];
        int k, n;
        printf("%s: sondeando el táctil ~5 s (toca la pantalla)...\n", args[0]);
        for (k = 0; k < 250; k++)
        {
            n = control(TOUCH0, TOUCH_CTRL_READ, (long)pts, 5);            if (n > 0)
            {
                printf("  %d punto(s): (%u,%u) id=%u\n",
                       n, (unsigned)pts[0].x, (unsigned)pts[0].y,
                       (unsigned)pts[0].id);
            }
            mdelay(20);
        }
        return OK;
    }

    printf("%s: dibujando patrón de bring-up...\n", args[0]);
    if (control(LCD0, LCD_CTRL_DEMO, 0, 0) == SYSERR)
    {
        fprintf(stderr, "%s: LCD no disponible\n", args[0]);
        return SYSERR;
    }
    printf("%s: listo\n", args[0]);
    return OK;
}
