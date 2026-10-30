/**
 * @file     xsh_gpio.c
 *
 * Shell command (gpio): read/write/configure ESP32-P4 GPIO pins through the
 * GPIO0 device (control()).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <device.h>
#include <gpio.h>

static bool gpio_parse(const char *s, unsigned long *out)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    {
        return sscanf(s + 2, "%lx", out) == 1;
    }
    return sscanf(s, "%lu", out) == 1;
}

static int gpio_usage(char *name);

/**
 * @ingroup shell
 *
 * Shell command (gpio <cmd> ...).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_gpio(int nargs, char *args[])
{
    unsigned long pin;

    if (nargs < 2 || strcmp(args[1], "--help") == 0)
    {
        return gpio_usage(args[0]);
    }

    if (0 == strcmp(args[1], "read") && nargs == 3)
    {
        if (!gpio_parse(args[2], &pin))
        {
            fprintf(stderr, "%s: invalid pin '%s'\n", args[0], args[2]);
            return SYSERR;
        }
        printf("gpio%lu = %d\n", pin,
               (int)control(GPIO0, GPIO_CTRL_READ, (long)pin, 0));
        return OK;
    }

    if (0 == strcmp(args[1], "write") && nargs == 4)
    {
        unsigned long val;
        if (!gpio_parse(args[2], &pin) || !gpio_parse(args[3], &val))
        {
            fprintf(stderr, "%s: invalid argument\n", args[0]);
            return SYSERR;
        }
        control(GPIO0, GPIO_CTRL_WRITE, (long)pin, val ? 1 : 0);
        return OK;
    }

    if (0 == strcmp(args[1], "toggle") && nargs == 3)
    {
        if (!gpio_parse(args[2], &pin))
        {
            fprintf(stderr, "%s: invalid pin '%s'\n", args[0], args[2]);
            return SYSERR;
        }
        control(GPIO0, GPIO_CTRL_TOGGLE, (long)pin, 0);
        return OK;
    }

    if (0 == strcmp(args[1], "mode") && nargs == 4)
    {
        if (!gpio_parse(args[2], &pin))
        {
            fprintf(stderr, "%s: invalid pin '%s'\n", args[0], args[2]);
            return SYSERR;
        }
        if (0 == strcmp(args[3], "out"))
        {
            control(GPIO0, GPIO_CTRL_SET_DIR, (long)pin, GPIO_OUTPUT);
        }
        else if (0 == strcmp(args[3], "in"))
        {
            control(GPIO0, GPIO_CTRL_SET_DIR, (long)pin, GPIO_INPUT);
        }
        else
        {
            fprintf(stderr, "%s: mode must be 'in' or 'out'\n", args[0]);
            return SYSERR;
        }
        return OK;
    }

    if (0 == strcmp(args[1], "pull") && nargs == 4)
    {
        int pull;
        if (!gpio_parse(args[2], &pin))
        {
            fprintf(stderr, "%s: invalid pin '%s'\n", args[0], args[2]);
            return SYSERR;
        }
        if (0 == strcmp(args[3], "none"))
        {
            pull = GPIO_PULL_NONE;
        }
        else if (0 == strcmp(args[3], "up"))
        {
            pull = GPIO_PULL_UP;
        }
        else if (0 == strcmp(args[3], "down"))
        {
            pull = GPIO_PULL_DOWN;
        }
        else
        {
            fprintf(stderr, "%s: pull must be none|up|down\n", args[0]);
            return SYSERR;
        }
        control(GPIO0, GPIO_CTRL_SET_PULL, (long)pin, pull);
        return OK;
    }

    return gpio_usage(args[0]);
}

static int gpio_usage(char *name)
{
    printf("Usage: %s <command> [args]\n\n", name);
    printf("Commands:\n");
    printf("\tread <pin>\n");
    printf("\twrite <pin> <0|1>\n");
    printf("\ttoggle <pin>\n");
    printf("\tmode <pin> <in|out>\n");
    printf("\tpull <pin> <none|up|down>\n");
    return SYSERR;
}
