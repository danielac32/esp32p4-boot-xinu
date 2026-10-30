/**
 * @file     xsh_rmdir.c
 *
 * Shell command (rmdir): remove empty directories.
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
 * Shell command (rmdir DIR...).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_rmdir(int nargs, char *args[])
{
    char path[FS_PATH_MAX];
    int i;
    int rc = OK;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s DIR...\n\n", args[0]);
        printf("Description:\n");
        printf("\tRemoves each empty directory DIR.\n");
        printf("Options:\n");
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
        if (!fs_resolve(args[i], path, sizeof(path)))
        {
            fprintf(stderr, "%s: path too long\n", args[0]);
            rc = SYSERR;
            continue;
        }

        if (!fl_is_dir(path))
        {
            fprintf(stderr, "%s: %s: not a directory\n", args[0], path);
            rc = SYSERR;
            continue;
        }

        if (fl_removedirectory(path) != 0)
        {
            fprintf(stderr, "%s: %s: directory not empty or busy\n",
                    args[0], path);
            rc = SYSERR;
        }
    }

    return rc;
}
