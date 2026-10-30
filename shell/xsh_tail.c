/**
 * @file     xsh_tail.c
 *
 * Shell command (tail): print the last N lines (or bytes) of a file.  The whole
 * file is read into PSRAM and the start of the requested tail is located.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <kernel.h>
#include <memory.h>
#include <fsutil.h>

/**
 * @ingroup shell
 *
 * Shell command (tail [-n N | -c N | -N] FILE).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_tail(int nargs, char *args[])
{
    long n = 10;
    bool bytes = FALSE;
    const char *arg = NULL;
    char path[FS_PATH_MAX];
    void *f;
    char *buf;
    long len;
    unsigned long total = 0;
    unsigned long skip;
    long i;
    int j;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s [-n N | -c N | -N] FILE\n\n", args[0]);
        printf("Description:\n");
        printf("\tPrints the last N lines (default N=10) of FILE.\n");
        printf("Options:\n");
        printf("\t-n N\tprint the last N lines\n");
        printf("\t-c N\tprint the last N bytes\n");
        printf("\t-N\tprint the last N lines (shorthand)\n");
        printf("\t--help\tdisplay this help and exit\n");
        return OK;
    }

    for (j = 1; j < nargs; j++)
    {
        const char *a = args[j];

        if (a[0] == '-' && a[1] != '\0')
        {
            if (a[1] == 'n' || a[1] == 'c')
            {
                bytes = (a[1] == 'c');
                if (a[2] != '\0')
                    n = atoi(a + 2);
                else if (j + 1 < nargs)
                    n = atoi(args[++j]);
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

    /* Determine the file length, then read it whole. */
    fl_fseek(f, 0, SEEK_END);
    len = fl_ftell(f);
    fl_fseek(f, 0, SEEK_SET);

    if (len <= 0)
    {
        fl_fclose(f);
        return OK;
    }

    buf = (char *)psram_malloc((size_t)len + 1);
    if (buf == NULL)
    {
        fl_fclose(f);
        fprintf(stderr, "%s: out of memory (%ld bytes)\n", args[0], len);
        return SYSERR;
    }

    len = fl_fread(buf, 1, (int)len, f);
    fl_fclose(f);
    if (len < 0)
    {
        psram_free(buf);
        fprintf(stderr, "%s: read error\n", args[0]);
        return SYSERR;
    }
    buf[len] = '\0';

    if (bytes)
    {
        for (i = (len > n) ? len - n : 0; i < len; i++)
            putchar(buf[i]);
        psram_free(buf);
        return OK;
    }

    /* Count lines (a trailing partial line counts as one). */
    for (i = 0; i < len; i++)
    {
        if (buf[i] == '\n')
            total++;
    }
    if (len > 0 && buf[len - 1] != '\n')
        total++;

    skip = (total > (unsigned long)n) ? total - (unsigned long)n : 0;
    i = 0;
    while (skip > 0 && i < len)
    {
        if (buf[i] == '\n')
            skip--;
        i++;
    }

    while (i < len)
    {
        putchar(buf[i]);
        i++;
    }

    psram_free(buf);
    return OK;
}
