/**
 * @file     xsh_touch.c
 *
 * Shell command (touch): create empty files, or leave existing files
 * unchanged.  The bundled FAT library has no utime(), so this only creates
 * missing files (its classic use).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <fsutil.h>

/**
 * @ingroup shell
 *
 * Shell command (touch [-c] FILE...).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_touch(int nargs, char *args[])
{
    bool no_create = FALSE;
    char path[FS_PATH_MAX];
    int i;
    int rc = OK;
    int nfiles = 0;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s [-c] FILE...\n\n", args[0]);
        printf("Description:\n");
        printf("\tCreates each FILE if it does not exist.  Existing files are\n");
        printf("\tleft unchanged (no timestamp support).\n");
        printf("Options:\n");
        printf("\t-c\tdo not create any file\n");
        printf("\t--help\tdisplay this help and exit\n");
        return OK;
    }

    if (nargs < 2)
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

    for (i = 1; i < nargs; i++)
    {
        void *f;

        if (args[i][0] == '-' && args[i][1] != '\0' &&
            strcmp(args[i], "--") != 0)
        {
            int k;
            for (k = 1; args[i][k] != '\0'; k++)
            {
                if (args[i][k] == 'c')
                    no_create = TRUE;
                else if (args[i][k] == 'a' || args[i][k] == 'm')
                    ;                   /* accepted, no effect */
                else
                {
                    fprintf(stderr, "%s: invalid option '%s'\n",
                            args[0], args[i]);
                    rc = SYSERR;
                }
            }
            continue;
        }

        nfiles++;

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

        if (fs_exists(path))
            continue;                   /* existing file: nothing to do */

        if (no_create)
        {
            fprintf(stderr, "%s: %s: no such file\n", args[0], path);
            rc = SYSERR;
            continue;
        }

        f = fl_fopen(path, "w");
        if (f == NULL)
        {
            fprintf(stderr, "%s: %s: cannot create\n", args[0], path);
            rc = SYSERR;
            continue;
        }
        fl_fclose(f);
    }

    if (nfiles == 0)
    {
        fprintf(stderr, "%s: missing operand\n", args[0]);
        return SYSERR;
    }

    return rc;
}
