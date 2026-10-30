/**
 * @file     xsh_ls.c
 *
 * Shell command (ls): list directories on the FAT volume.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <fsutil.h>

#define LS_MAX_PATHS 32

/* List one resolved path.  @p header forces a "path:" header line. */
static int ls_one(const char *what, const char *path, bool longfmt, bool all,
                  bool header)
{
    FL_DIR dir;
    fl_dirent ent;
    unsigned count = 0;
    unsigned long total = 0;

    if (fl_is_dir(path))
    {
        if (header)
            printf("%s:\n", what);

        if (fl_opendir(path, &dir) == NULL)
        {
            fprintf(stderr, "ls: %s: cannot open directory\n", path);
            return SYSERR;
        }

        while (fl_readdir(&dir, &ent) == 0)
        {
            if (!all && (0 == strcmp(ent.filename, ".") ||
                         0 == strcmp(ent.filename, "..")))
            {
                continue;
            }

            if (longfmt)
            {
                if (ent.is_dir)
                    printf("%10s  %s/\n", "<DIR>", ent.filename);
                else
                {
                    printf("%10lu  %s\n", (unsigned long)ent.size,
                           ent.filename);
                    total += ent.size;
                }
            }
            else
            {
                printf("%s%s\n", ent.filename, ent.is_dir ? "/" : "");
            }
            count++;
        }

        fl_closedir(&dir);

        if (longfmt)
            printf("%u item(s), %lu byte(s)\n", count, total);
    }
    else
    {
        /* A plain file: just name it. */
        printf("%s\n", what);
    }

    return OK;
}

/**
 * @ingroup shell
 *
 * Shell command (ls [-l] [-a] [PATH...]).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_ls(int nargs, char *args[])
{
    bool longfmt = FALSE;
    bool all = FALSE;
    char *paths[LS_MAX_PATHS];
    int npaths = 0;
    char path[FS_PATH_MAX];
    int i;
    int rc = OK;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s [-l] [-a] [PATH...]\n\n", args[0]);
        printf("Description:\n");
        printf("\tLists the contents of each PATH (default '.').\n");
        printf("Options:\n");
        printf("\t-l\tlong format (size in bytes)\n");
        printf("\t-a\tinclude entries starting with '.' ('.', '..')\n");
        printf("\t--help\tdisplay this help and exit\n");
        return OK;
    }

    for (i = 1; i < nargs; i++)
    {
        if (strcmp(args[i], "--") == 0)
        {
            for (i++; i < nargs; i++)
            {
                if (npaths < LS_MAX_PATHS)
                    paths[npaths++] = args[i];
            }
            break;
        }

        if (args[i][0] == '-' && args[i][1] != '\0')
        {
            int k;
            for (k = 1; args[i][k] != '\0'; k++)
            {
                if (args[i][k] == 'l')      longfmt = TRUE;
                else if (args[i][k] == 'a') all = TRUE;
                else
                {
                    fprintf(stderr, "%s: invalid option '%s'\n",
                            args[0], args[i]);
                    fprintf(stderr, "Try '%s --help' for more information\n",
                            args[0]);
                    return SYSERR;
                }
            }
        }
        else if (npaths < LS_MAX_PATHS)
        {
            paths[npaths++] = args[i];
        }
    }

    if (npaths == 0)
    {
        static char dot[] = ".";
        paths[npaths++] = dot;
    }

    if (!fs_mounted())
    {
        fprintf(stderr, "%s: no FAT volume mounted\n", args[0]);
        return SYSERR;
    }

    for (i = 0; i < npaths; i++)
    {
        bool header = (npaths > 1);

        if (!fs_resolve(paths[i], path, sizeof(path)))
        {
            fprintf(stderr, "%s: path too long\n", args[0]);
            rc = SYSERR;
            continue;
        }

        if (ls_one(paths[i], path, longfmt, all, header) != OK)
            rc = SYSERR;
    }

    return rc;
}
