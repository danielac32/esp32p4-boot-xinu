/**
 * @file     xsh_mkdir.c
 *
 * Shell command (mkdir): create one or more directories.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <fsutil.h>

/* Create @p path and any missing parents (absolute, normalized). */
static int mkdir_p(const char *path)
{
    char tmp[FS_PATH_MAX];
    char *p;
    size_t len = strlen(path);

    if (len == 0 || len >= sizeof(tmp))
        return SYSERR;

    strcpy(tmp, path);

    for (p = tmp + 1; ; p++)
    {
        if (*p == '/' || *p == '\0')
        {
            char saved = *p;

            *p = '\0';
            if (tmp[1] != '\0' && !fl_is_dir(tmp))
            {
                if (fl_createdirectory(tmp) != 1 && !fl_is_dir(tmp))
                {
                    *p = saved;
                    return SYSERR;
                }
            }
            *p = saved;

            if (saved == '\0')
                break;
        }
    }

    return OK;
}

/**
 * @ingroup shell
 *
 * Shell command (mkdir [-p] DIR...).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_mkdir(int nargs, char *args[])
{
    bool parents = FALSE;
    char path[FS_PATH_MAX];
    int i;
    int rc = OK;
    int ndirs = 0;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s [-p] DIR...\n\n", args[0]);
        printf("Description:\n");
        printf("\tCreates each directory DIR.\n");
        printf("Options:\n");
        printf("\t-p\tcreate parent directories as needed\n");
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
                if (args[i][k] == 'p')
                    parents = TRUE;
                else
                {
                    fprintf(stderr, "%s: invalid option '%s'\n",
                            args[0], args[i]);
                    rc = SYSERR;
                }
            }
            continue;
        }

        ndirs++;

        if (!fs_resolve(args[i], path, sizeof(path)))
        {
            fprintf(stderr, "%s: path too long\n", args[0]);
            rc = SYSERR;
            continue;
        }

        if (parents)
        {
            if (mkdir_p(path) != OK)
            {
                fprintf(stderr, "%s: %s: could not create\n", args[0], path);
                rc = SYSERR;
            }
            continue;
        }

        if (fs_exists(path))
        {
            fprintf(stderr, "%s: %s: already exists\n", args[0], path);
            rc = SYSERR;
            continue;
        }

        if (fl_createdirectory(path) != 1)
        {
            fprintf(stderr, "%s: %s: could not create\n", args[0], path);
            rc = SYSERR;
        }
    }

    if (ndirs == 0)
    {
        fprintf(stderr, "%s: missing operand\n", args[0]);
        return SYSERR;
    }

    return rc;
}
