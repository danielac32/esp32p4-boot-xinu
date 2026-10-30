/**
 * @file     xsh_mv.c
 *
 * Shell command (mv): rename/move files and directories.  The bundled FAT
 * library exposes no rename primitive, so this is copy + delete.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <fsutil.h>

#define MV_MAX_ARGS 32

/* Build @p out = @p dir + "/" + basename(@p src). */
static bool mv_join(const char *dir, const char *src, char *out, size_t outsz)
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
 * Shell command (mv SRC... DST).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_mv(int nargs, char *args[])
{
    bool force = FALSE;
    char *pos[MV_MAX_ARGS];
    int npos = 0;
    char dst[FS_PATH_MAX];
    int i;
    int rc = OK;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s [-f] SRC... DST\n\n", args[0]);
        printf("Description:\n");
        printf("\tMoves SRC to DST (copy + delete).  If DST is a directory,\n");
        printf("\teach SRC is moved into it.  DST is overwritten.\n");
        printf("Options:\n");
        printf("\t-f\tdo not prompt before overwriting\n");
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
                if (args[i][k] == 'f')
                    force = TRUE;
                else
                {
                    fprintf(stderr, "%s: invalid option '%s'\n",
                            args[0], args[i]);
                    return SYSERR;
                }
            }
        }
        else if (npos < MV_MAX_ARGS)
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
        size_t slen;

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

        if (fl_is_dir(dst))
        {
            if (!mv_join(dst, src, target, sizeof(target)))
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

        /* Do not move a directory into itself. */
        slen = strlen(src);
        if (fl_is_dir(src) && strncmp(target, src, slen) == 0 &&
            target[slen] == '/')
        {
            fprintf(stderr, "%s: %s: cannot move into itself\n",
                    args[0], src);
            rc = SYSERR;
            continue;
        }

        if (fs_exists(target))
        {
            if (fl_is_dir(target))
            {
                fprintf(stderr, "%s: %s: is a directory\n", args[0], target);
                rc = SYSERR;
                continue;
            }
            if (fl_remove(target) != 0 && !force)
            {
                fprintf(stderr, "%s: %s: cannot overwrite\n",
                        args[0], target);
                rc = SYSERR;
                continue;
            }
        }

        if (fs_copy_tree(src, target) != OK)
        {
            fprintf(stderr, "%s: move failed\n", args[0]);
            rc = SYSERR;
            continue;
        }

        if (fs_remove_tree(src) != OK)
        {
            fprintf(stderr, "%s: %s: copied but could not remove source\n",
                    args[0], src);
            rc = SYSERR;
        }
    }

    return rc;
}
