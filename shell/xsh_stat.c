/**
 * @file     xsh_stat.c
 *
 * Shell command (stat): report basic information about a file or directory.
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
 * Shell command (stat PATH).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_stat(int nargs, char *args[])
{
    char path[FS_PATH_MAX];
    FL_FILE *fp;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s PATH\n\n", args[0]);
        printf("Description:\n");
        printf("\tShows information about the file or directory PATH.\n");
        printf("Options:\n");
        printf("\t--help\tdisplay this help and exit\n");
        return OK;
    }

    if (nargs != 2)
    {
        fprintf(stderr, "%s: expected one PATH\n", args[0]);
        fprintf(stderr, "Try '%s --help' for more information\n", args[0]);
        return SYSERR;
    }

    if (!fs_mounted())
    {
        fprintf(stderr, "%s: no FAT volume mounted\n", args[0]);
        return SYSERR;
    }

    if (!fs_resolve(args[1], path, sizeof(path)))
    {
        fprintf(stderr, "%s: path too long\n", args[0]);
        return SYSERR;
    }

    if (fl_is_dir(path))
    {
        printf("path : %s\n", path);
        printf("type : directory\n");
        return OK;
    }

    fp = (FL_FILE *)fl_fopen(path, "r");
    if (fp == NULL)
    {
        fprintf(stderr, "%s: %s: no such file\n", args[0], path);
        return SYSERR;
    }

    printf("path    : %s\n", path);
    printf("type    : file\n");
    printf("name    : %s\n", fp->filename);
    printf("size    : %lu byte(s)\n", (unsigned long)fp->filelength);
    printf("cluster : %lu\n", (unsigned long)fp->startcluster);

    fl_fclose(fp);
    return OK;
}
