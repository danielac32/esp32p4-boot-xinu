/**
 * @file     xsh_cp.c
 *
 * Shell command (cp): copy files and (with -r) directories on the FAT volume.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <fsutil.h>

#define CP_MAX_ARGS 32

/* Build @p out = @p dir + "/" + basename(@p src). */
static bool cp_join(const char *dir, const char *src, char *out, size_t outsz)
{
    const char *base = fs_basename(src);
    size_t dl = strlen(dir);
    size_t bl = strlen(base);

    if (dl + 1 + bl + 1 > outsz)
        return FALSE;

    strcpy(out, dir);
    if (dl > 0 && dir[dl - 1] != '/')
        strcat(out, "/");
    strcat(out, base);
    return TRUE;
}

/**
 * @ingroup shell
 *
 * Shell command (cp [-r] SRC... DST).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_cp(int nargs, char *args[])
{
    bool recursive = FALSE;
    char *pos[CP_MAX_ARGS];
    int npos = 0;
    char dst[FS_PATH_MAX];
    int i;
    int rc = OK;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s [-r] SRC... DST\n\n", args[0]);
        printf("Description:\n");
        printf("\tCopies SRC to DST (DST is overwritten).  If DST is a\n");
        printf("\tdirectory, each SRC is copied into it.\n");
        printf("Options:\n");
        printf("\t-r\trecurse into directories\n");
        printf("\t-f\tignore (accepted for compatibility)\n");
        printf("\t--help\tdisplay this help and exit\n");
        return OK;
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
                    ;
                else
                {
                    fprintf(stderr, "%s: invalid option '%s'\n",
                            args[0], args[i]);
                    return SYSERR;
                }
            }
        }
        else if (npos < CP_MAX_ARGS)
        {
            pos[npos++] = args[i];
        }
    }

    if (npos < 2)
    {
        fprintf(stderr, "%s: expected SRC... DST\n", args[0]);
        fprintf(stderr, "Try '%s --help' for more information\n", args[0]);
        return SYSERR;
    }

    if (!fs_mounted())
    {
        fprintf(stderr, "%s: no FAT volume mounted\n", args[0]);
        return SYSERR;
    }

    if (!fs_resolve(pos[npos - 1], dst, sizeof(dst)))
    {
        fprintf(stderr, "%s: path too long\n", args[0]);
        return SYSERR;
    }

    if (npos > 2 && !fl_is_dir(dst))
    {
        fprintf(stderr, "%s: %s: not a directory\n", args[0], dst);
        return SYSERR;
    }

    for (i = 0; i < npos - 1; i++)
    {
        char src[FS_PATH_MAX];
        char target[FS_PATH_MAX];

        if (!fs_resolve(pos[i], src, sizeof(src)))
        {
            fprintf(stderr, "%s: path too long\n", args[0]);
            rc = SYSERR;
            continue;
        }

        if (!fs_exists(src))
        {
            fprintf(stderr, "%s: %s: no such file\n", args[0], src);
            rc = SYSERR;
            continue;
        }

        if (fl_is_dir(src) && !recursive)
        {
            fprintf(stderr, "%s: %s: is a directory (use -r)\n",
                    args[0], src);
            rc = SYSERR;
            continue;
        }

        if (fl_is_dir(dst))
        {
            if (!cp_join(dst, src, target, sizeof(target)))
            {
                fprintf(stderr, "%s: path too long\n", args[0]);
                rc = SYSERR;
                continue;
            }
        }
        else
        {
            strcpy(target, dst);
        }

        if (fs_copy_tree(src, target) != OK)
        {
            fprintf(stderr, "%s: copy failed\n", args[0]);
            rc = SYSERR;
        }
    }

    return rc;
}
