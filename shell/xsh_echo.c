/**
 * @file     xsh_echo.c
 *
 * Shell command (echo): write its arguments, separated by spaces, to stdout.
 * Follows the common (BusyBox/Linux) behaviour: -n suppresses the trailing
 * newline and -e enables backslash escapes.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>

/* Interpret backslash escapes in @p s, writing to stdout.  Returns TRUE if a
 * '\c' sequence was found (stop all output, including the final newline). */
static bool echo_escapes(const char *s)
{
    while (*s != '\0')
    {
        if (*s != '\\')
        {
            putchar(*s++);
            continue;
        }

        s++;
        switch (*s)
        {
        case 'a': putchar('\a'); s++; break;
        case 'b': putchar('\b'); s++; break;
        case 'e': putchar(27);  s++; break;
        case 'f': putchar('\f'); s++; break;
        case 'n': putchar('\n'); s++; break;
        case 'r': putchar('\r'); s++; break;
        case 't': putchar('\t'); s++; break;
        case 'v': putchar('\v'); s++; break;
        case '\\': putchar('\\'); s++; break;
        case 'c':
            return TRUE;
        case '0':
        {
            int v = 0;
            int k = 0;
            s++;
            while (k < 3 && *s >= '0' && *s <= '7')
            {
                v = v * 8 + (*s - '0');
                s++;
                k++;
            }
            putchar(v & 0xFF);
            break;
        }
        case '\0':
            putchar('\\');
            break;
        default:
            putchar('\\');
            putchar(*s);
            s++;
            break;
        }
    }
    return FALSE;
}

/**
 * @ingroup shell
 *
 * Shell command (echo [-n] [-e|-E] [ARG...]).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return OK
 */
shellcmd xsh_echo(int nargs, char *args[])
{
    bool newline = TRUE;
    bool escapes = FALSE;
    bool stop = FALSE;
    int i = 1;
    int first;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s [-neE] [ARG...]\n\n", args[0]);
        printf("Description:\n");
        printf("\tWrites ARG(s) to standard output, space separated.\n");
        printf("Options:\n");
        printf("\t-n\tdo not output the trailing newline\n");
        printf("\t-e\tenable interpretation of backslash escapes\n");
        printf("\t-E\tdisable interpretation of escapes (default)\n");
        printf("\t--help\tdisplay this help and exit\n");
        return OK;
    }

    /* Leading options: only n/e/E characters. */
    while (i < nargs)
    {
        const char *a = args[i];
        int k;
        bool ok;

        if (a[0] != '-' || a[1] == '\0')
            break;
        if (strcmp(a, "--") == 0)
        {
            i++;
            break;
        }

        ok = TRUE;
        for (k = 1; a[k] != '\0'; k++)
        {
            if (a[k] != 'n' && a[k] != 'e' && a[k] != 'E')
            {
                ok = FALSE;
                break;
            }
        }
        if (!ok)
            break;

        for (k = 1; a[k] != '\0'; k++)
        {
            if (a[k] == 'n')      newline = FALSE;
            else if (a[k] == 'e') escapes = TRUE;
            else                  escapes = FALSE;
        }
        i++;
    }

    first = i;
    for (; i < nargs && !stop; i++)
    {
        if (i > first)
            putchar(' ');
        if (escapes)
            stop = echo_escapes(args[i]);
        else
            fputs(args[i], stdout);
    }

    if (newline && !stop)
        putchar('\n');

    return OK;
}
