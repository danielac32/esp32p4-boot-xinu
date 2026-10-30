/**
 * @file     xsh_head.c
 *
 * Shell command (head): print the first N lines (or bytes) of a file.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <kernel.h>
#include <fsutil.h>

/**
 * @ingroup shell
 *
 * Shell command (head [-n N | -c N] FILE).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_head(int nargs, char *args[])
{
    long n = 10;
    bool bytes = FALSE;
    const char *arg = NULL;
    char path[FS_PATH_MAX];
    void *f;
    int c;
    long done = 0;
    long lines = 0;
    int i;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s [-n N | -c N | -N] FILE\n\n", args[0]);
        printf("Description:\n");
        printf("\tPrints the first N lines (default N=10) of FILE.\n");
        printf("Options:\n");
        printf("\t-n N\tprint the first N lines\n");
        printf("\t-c N\tprint the first N bytes\n");
        printf("\t-N\tprint the first N lines (shorthand)\n");
        printf("\t--help\tdisplay this help and exit\n");
        return OK;
    }

    for (i = 1; i < nargs; i++)
    {
        const char *a = args[i];

        if (a[0] == '-' && a[1] != '\0')
        {
            if (a[1] == 'n' || a[1] == 'c')
            {
                bytes = (a[1] == 'c');
                if (a[2] != '\0')
                    n = atoi(a + 2);
                else if (i + 1 < nargs)
                    n = atoi(args[++i]);
                else
                {
                    fprintf(stderr, "%s: option -%c needs an argument\n",
                            args[0], a[1]);
                    return SYSERR;
                }
            }
            else
            {
                int k;
                for (k = 1; a[k] != '\0' && a[k] >= '0' && a[k] <= '9'; k++)
                    ;
                if (a[k] != '\0')
                {
                    fprintf(stderr, "%s: invalid option '%s'\n", args[0], a);
                    return SYSERR;
                }
                n = atoi(a + 1);
            }
        }
        else if (arg == NULL)
        {
            arg = a;
        }
        else
        {
            fprintf(stderr, "%s: too many arguments\n", args[0]);
            return SYSERR;
        }
    }

    if (arg == NULL || n < 0)
    {
        fprintf(stderr, "%s: invalid arguments\n", args[0]);
        return SYSERR;
    }

    if (!fs_mounted())
    {
        fprintf(stderr, "%s: no FAT volume mounted\n", args[0]);
        return SYSERR;
    }

    if (!fs_resolve(arg, path, sizeof(path)))
    {
        fprintf(stderr, "%s: path too long\n", args[0]);
        return SYSERR;
    }

    f = fl_fopen(path, "r");
    if (f == NULL)
    {
        fprintf(stderr, "%s: %s: cannot open\n", args[0], path);
        return SYSERR;
    }

    if (bytes)
    {
        while (done < n && (c = fl_fgetc(f)) >= 0)
        {
            putchar(c);
            done++;
        }
    }
    else
    {
        while ((c = fl_fgetc(f)) >= 0)
        {
            putchar(c);
            if (c == '\n' && ++lines >= n)
                break;
        }
    }

    fl_fclose(f);
    return OK;
}
