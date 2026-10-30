/**
 * @file     xsh_rm.c
 *
 * Shell command (rm): remove files, and directories with -r.
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
 * Shell command (rm [-r] [-f] PATH...).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_rm(int nargs, char *args[])
{
    bool recursive = FALSE;
    bool force = FALSE;
    char path[FS_PATH_MAX];
    int i;
    int rc = OK;
    int nfiles = 0;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s [-r] [-f] PATH...\n\n", args[0]);
        printf("Description:\n");
        printf("\tRemoves each PATH.  Directories need -r.\n");
        printf("Options:\n");
        printf("\t-r\tremove directories and their contents\n");
        printf("\t-f\tignore missing files and never prompt\n");
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
        if (args[i][0] == '-' && args[i][1] != '\0' &&
            strcmp(args[i], "--") != 0)
        {
            int k;
            for (k = 1; args[i][k] != '\0'; k++)
            {
                if (args[i][k] == 'r' || args[i][k] == 'R')
                    recursive = TRUE;
                else if (args[i][k] == 'f')
                    force = TRUE;
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
            if (!recursive)
            {
                fprintf(stderr, "%s: %s: is a directory (use -r)\n",
                        args[0], path);
                rc = SYSERR;
                continue;
            }
            if (fs_remove_tree(path) != OK)
            {
                fprintf(stderr, "%s: %s: could not remove\n", args[0], path);
                rc = SYSERR;
            }
        }
        else
        {
            if (!fs_exists(path))
            {
                if (!force)
                {
                    fprintf(stderr, "%s: %s: no such file\n", args[0], path);
                    rc = SYSERR;
                }
                continue;
            }
            if (fl_remove(path) != 0)
            {
                fprintf(stderr, "%s: %s: could not remove\n", args[0], path);
                rc = SYSERR;
            }
        }
    }

    if (nfiles == 0)
    {
        fprintf(stderr, "%s: missing operand\n", args[0]);
        return SYSERR;
    }

    return rc;
}
