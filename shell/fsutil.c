/**
 * @file fsutil.c — helpers shared by the filesystem shell commands.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <string.h>
#include <kernel.h>
#include <device.h>
#include <fat.h>
#include <fsutil.h>

char fs_cwd[FS_PATH_MAX] = "/";

/* Collapse an absolute path in place: remove '//', '/./' and resolve '..'.
 * The buffer must start with '/'. */
static void fs_normalize(char *path)
{
    char *o;                    /* output cursor */
    char *s;                    /* scan cursor   */

    o = path;
    s = path;
    while (*s == '/')
    {
        s++;
    }

    *o++ = '/';

    while (*s)
    {
        char *seg = s;
        size_t len;

        while (*s && *s != '/')
        {
            s++;
        }
        len = (size_t)(s - seg);

        if (len == 1 && seg[0] == '.')
        {
            /* current directory: drop */
        }
        else if (len == 2 && seg[0] == '.' && seg[1] == '.')
        {
            if (o > path + 1)   /* not already at root */
            {
                o--;            /* step back over the last char */
                while (o > path + 1 && o[-1] != '/')
                {
                    o--;
                }
                if (o > path + 1)
                {
                    o--;        /* and over the separating '/' */
                }
            }
        }
        else
        {
            if (o > path + 1)
            {
                *o++ = '/';
            }
            memmove(o, seg, len);
            o += len;
        }

        while (*s == '/')
        {
            s++;
        }
    }

    *o = '\0';
}

bool fs_mounted(void)
{
    return control(FAT0, FAT_CTRL_MOUNTED, 0, 0) == 1;
}

bool fs_exists(const char *path)
{
    void *f;

    if (fl_is_dir(path))
    {
        return TRUE;
    }

    f = fl_fopen(path, "r");
    if (f != NULL)
    {
        fl_fclose(f);
        return TRUE;
    }
    return FALSE;
}

bool fs_resolve(const char *arg, char *out, size_t outsz)
{
    size_t cl;

    if (arg == NULL || *arg == '\0')
    {
        arg = ".";
    }

    if (arg[0] == '/')
    {
        if (strlen(arg) >= outsz)
        {
            return FALSE;
        }
        strcpy(out, arg);
    }
    else
    {
        cl = strlen(fs_cwd);
        if (cl + 1 + strlen(arg) >= outsz)
        {
            return FALSE;
        }
        strcpy(out, fs_cwd);
        if (out[cl - 1] != '/')
        {
            out[cl] = '/';
            out[cl + 1] = '\0';
        }
        strcat(out, arg);
    }

    fs_normalize(out);
    return TRUE;
}

int fs_copy(const char *src, const char *dst)
{
    void *in;
    void *out;
    char buf[256];
    int n;
    int rc = OK;

    in = fl_fopen(src, "r");
    if (in == NULL)
    {
        return SYSERR;
    }

    out = fl_fopen(dst, "w");
    if (out == NULL)
    {
        fl_fclose(in);
        return SYSERR;
    }

    while ((n = fl_fread(buf, 1, (int)sizeof(buf), in)) > 0)
    {
        if (fl_fwrite(buf, 1, n, out) != n)
        {
            rc = SYSERR;
            break;
        }
    }

    fl_fclose(in);
    fl_fclose(out);
    return rc;
}

const char *fs_basename(const char *path)
{
    const char *slash;

    if (path == NULL)
        return "";

    slash = strrchr(path, '/');
    return (slash != NULL) ? slash + 1 : path;
}

/* Build "a/b" into @p out. */
static bool fs_path_join(char *out, size_t outsz, const char *a, const char *b)
{
    size_t al = strlen(a);
    size_t bl = strlen(b);

    if (al + 1 + bl + 1 > outsz)
    {
        return FALSE;
    }

    memcpy(out, a, al);
    out[al] = '/';
    memcpy(out + al + 1, b, bl + 1);
    return TRUE;
}

int fs_copy_tree(const char *src, const char *dst)
{
    FL_DIR dir;
    fl_dirent ent;
    int rc = OK;

    if (!fl_is_dir(src))
    {
        return fs_copy(src, dst);
    }

    if (!fl_is_dir(dst))
    {
        if (fl_createdirectory(dst) != 1 && !fl_is_dir(dst))
        {
            return SYSERR;
        }
    }

    if (fl_opendir(src, &dir) == NULL)
    {
        return SYSERR;
    }

    while (fl_readdir(&dir, &ent) == 0)
    {
        char cs[FS_PATH_MAX];
        char cd[FS_PATH_MAX];

        if (0 == strcmp(ent.filename, ".") ||
            0 == strcmp(ent.filename, ".."))
        {
            continue;
        }

        if (!fs_path_join(cs, sizeof(cs), src, ent.filename) ||
            !fs_path_join(cd, sizeof(cd), dst, ent.filename))
        {
            rc = SYSERR;
            continue;
        }

        if (fs_copy_tree(cs, cd) != OK)
        {
            rc = SYSERR;
        }
    }

    fl_closedir(&dir);
    return rc;
}

int fs_remove_tree(const char *path)
{
    FL_DIR dir;
    fl_dirent ent;
    char child[FS_PATH_MAX];
    int rc = OK;

    if (fl_opendir(path, &dir) != NULL)
    {
        while (fl_readdir(&dir, &ent) == 0)
        {
            if (0 == strcmp(ent.filename, ".") ||
                0 == strcmp(ent.filename, ".."))
            {
                continue;
            }

            if (!fs_path_join(child, sizeof(child), path, ent.filename))
            {
                rc = SYSERR;
                continue;
            }

            if (ent.is_dir)
            {
                if (fs_remove_tree(child) != OK)
                    rc = SYSERR;
            }
            else if (fl_remove(child) != 0)
            {
                rc = SYSERR;
            }
        }

        fl_closedir(&dir);
    }
    else if (!fl_is_dir(path))
    {
        /* Not a directory: a plain file. */
        return (fl_remove(path) != 0) ? SYSERR : OK;
    }

    if (fl_removedirectory(path) != 0)
    {
        rc = SYSERR;
    }
    return rc;
}
