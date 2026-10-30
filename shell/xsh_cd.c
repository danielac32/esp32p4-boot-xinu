/**
 * @file     xsh_cd.c
 *
 * Shell command (cd): change the shell's working directory.  The FAT library
 * has no notion of a per-thread cwd, so this is a global (single-shell)
 * variable in shell/fsutil.c.
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
 * Shell command (cd [DIR]).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_cd(int nargs, char *args[])
{
    char path[FS_PATH_MAX];
    const char *arg;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s [DIR]\n\n", args[0]);
        printf("Description:\n");
        printf("\tChanges the working directory to DIR.\n");
        printf("\tWithout DIR, changes to the root directory.\n");
        printf("Options:\n");
        printf("\t--help\tdisplay this help and exit\n");
        return OK;
    }

    if (nargs > 2)
    {
        fprintf(stderr, "%s: too many arguments\n", args[0]);
        fprintf(stderr, "Try '%s --help' for more information\n", args[0]);
        return SYSERR;
    }

    if (!fs_mounted())
    {
        fprintf(stderr, "%s: no FAT volume mounted\n", args[0]);
        return SYSERR;
    }

    arg = (nargs == 2) ? args[1] : "/";

    if (!fs_resolve(arg, path, sizeof(path)))
    {
        fprintf(stderr, "%s: path too long\n", args[0]);
        return SYSERR;
    }

    if (!fl_is_dir(path))
    {
        fprintf(stderr, "%s: %s: not a directory\n", args[0], path);
        return SYSERR;
    }

    strlcpy(fs_cwd, path, sizeof(fs_cwd));
    return OK;
}
