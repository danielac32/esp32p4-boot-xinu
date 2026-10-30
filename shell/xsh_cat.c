/**
 * @file     xsh_cat.c
 *
 * Shell command (cat): write the contents of one or more files to stdout.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <fsutil.h>

/* Copy file @p f to stdout, optionally numbering lines.  Returns OK/SYSERR. */
static int cat_stream(void *f, bool number, bool number_nonblank, int *lineno)
{
    int c;
    bool at_start = TRUE;

    while ((c = fl_fgetc(f)) >= 0)
    {
        if (at_start)
        {
            bool blank = (c == '\n');
            /* Like GNU cat: -b (number non-blank) takes precedence over -n. */
            if (number_nonblank ? !blank : number)
                printf("%6d\t", ++(*lineno));
            at_start = FALSE;
        }
        putchar(c);
        if (c == '\n')
            at_start = TRUE;
    }
    return OK;
}

/**
 * @ingroup shell
 *
 * Shell command (cat [-n|-b] FILE...).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_cat(int nargs, char *args[])
{
    char path[FS_PATH_MAX];
    bool number = FALSE;
    bool number_nonblank = FALSE;
    int lineno = 0;
    int i = 1;
    int rc = OK;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s [-n] [-b] FILE...\n\n", args[0]);
        printf("Description:\n");
        printf("\tWrites each FILE to standard output.\n");
        printf("Options:\n");
        printf("\t-n\tnumber all output lines\n");
        printf("\t-b\tnumber non-blank output lines\n");
        printf("\t--help\tdisplay this help and exit\n");
        return OK;
    }

    for (; i < nargs; i++)
    {
        if (strcmp(args[i], "--") == 0)
        {
            i++;
            break;
        }
        if (args[i][0] == '-' && args[i][1] != '\0')
        {
            int k;
            for (k = 1; args[i][k] != '\0'; k++)
            {
                if (args[i][k] == 'n')      number = TRUE;
                else if (args[i][k] == 'b') number_nonblank = TRUE;
                else
                {
                    fprintf(stderr, "%s: invalid option '%s'\n",
                            args[0], args[i]);
                    return SYSERR;
                }
            }
        }
        else
        {
            break;
        }
    }

    if (i >= nargs)
    {
        fprintf(stderr, "%s: missing operand\n", args[0]);
        fprintf(stderr, "Try '%s --help' for more information\n", args[0]);
        return SYSERR;
    }

    if (!fs_mounted())
    {
        fprintf(stderr, "%s: no FAT volume mounted\n", args[0]);
        return SYSERR;
    }

    for (; i < nargs; i++)
    {
        void *f;

        if (!fs_resolve(args[i], path, sizeof(path)))
        {
            fprintf(stderr, "%s: path too long\n", args[0]);
            rc = SYSERR;
            continue;
        }

        if (fl_is_dir(path))
        {
            fprintf(stderr, "%s: %s: is a directory\n", args[0], path);
            rc = SYSERR;
            continue;
        }

        f = fl_fopen(path, "r");
        if (f == NULL)
        {
            fprintf(stderr, "%s: %s: cannot open\n", args[0], path);
            rc = SYSERR;
            continue;
        }

        cat_stream(f, number, number_nonblank, &lineno);
        fl_fclose(f);
    }

    return rc;
}
