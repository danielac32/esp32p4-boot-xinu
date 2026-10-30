/**
 * @file     xsh_hexdump.c
 *
 * Shell command (hexdump): canonical hex+ASCII dump of a file.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <kernel.h>
#include <memory.h>
#include <fsutil.h>

/**
 * @ingroup shell
 *
 * Shell command (hexdump FILE).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_hexdump(int nargs, char *args[])
{
    char path[FS_PATH_MAX];
    void *f;
    unsigned char *buf;
    long len;
    long i;
    int j;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s FILE\n\n", args[0]);
        printf("Description:\n");
        printf("\tWrites a canonical hex dump of FILE.\n");
        printf("Options:\n");
        printf("\t--help\tdisplay this help and exit\n");
        return OK;
    }

    if (nargs != 2)
    {
        fprintf(stderr, "%s: expected one FILE\n", args[0]);
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
        fprintf(stderr, "%s: %s: is a directory\n", args[0], path);
        return SYSERR;
    }

    f = fl_fopen(path, "r");
    if (f == NULL)
    {
        fprintf(stderr, "%s: %s: cannot open\n", args[0], path);
        return SYSERR;
    }

    fl_fseek(f, 0, SEEK_END);
    len = fl_ftell(f);
    fl_fseek(f, 0, SEEK_SET);

    if (len <= 0)
    {
        fl_fclose(f);
        return OK;
    }

    buf = (unsigned char *)psram_malloc((size_t)len);
    if (buf == NULL)
    {
        fl_fclose(f);
        fprintf(stderr, "%s: out of memory (%ld bytes)\n", args[0], len);
        return SYSERR;
    }

    len = fl_fread(buf, 1, (int)len, f);
    fl_fclose(f);
    if (len < 0)
    {
        psram_free(buf);
        fprintf(stderr, "%s: read error\n", args[0]);
        return SYSERR;
    }

    for (i = 0; i < len; i += 16)
    {
        printf("%08lx  ", (unsigned long)i);
        for (j = 0; j < 16; j++)
        {
            if (i + j < len)
            {
                printf("%02x ", buf[i + j]);
            }
            else
            {
                printf("   ");
            }
        }
        printf(" |");
        for (j = 0; j < 16 && i + j < len; j++)
        {
            unsigned char c = buf[i + j];
            putchar(isprint(c) ? c : '.');
        }
        printf("|\n");
    }

    psram_free(buf);
    return OK;
}
