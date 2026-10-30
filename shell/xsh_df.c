/**
 * @file     xsh_df.c
 *
 * Shell command (df): report free space on the FAT volume.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <fsutil.h>
#include <fat_table.h>
#include <drivers/sd/sd.h>

/* Print @p kib in a human-friendly unit (GiB/MiB/KiB). */
static void df_human(unsigned long kib)
{
    if (kib >= 1048576UL)
    {
        printf("%lu.%lu GiB", kib / 1048576UL,
               (kib % 1048576UL) / 104857UL);
    }
    else if (kib >= 1024UL)
    {
        printf("%lu.%lu MiB", kib / 1024UL, (kib % 1024UL) / 102UL);
    }
    else
    {
        printf("%lu KiB", kib);
    }
}

/**
 * @ingroup shell
 *
 * Shell command (df [-h]).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_df(int nargs, char *args[])
{
    struct fatfs *fs;
    uint32 free_clusters;
    ulong spc;
    ulong total_kib;
    ulong free_sectors;
    ulong free_kib;
    bool human = FALSE;
    int i;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s [-h]\n\n", args[0]);
        printf("Description:\n");
        printf("\tReports total/free space on the FAT volume.\n");
        printf("Options:\n");
        printf("\t-h\thuman-readable sizes (GiB/MiB/KiB)\n");
        printf("\t--help\tdisplay this help and exit\n");
        return OK;
    }

    for (i = 1; i < nargs; i++)
    {
        if (strcmp(args[i], "-h") == 0)
            human = TRUE;
        else
        {
            fprintf(stderr, "%s: invalid argument '%s'\n", args[0], args[i]);
            return SYSERR;
        }
    }

    if (!fs_mounted())
    {
        fprintf(stderr, "%s: no FAT volume mounted\n", args[0]);
        return SYSERR;
    }

    fs = fl_get_fs();
    if (fs == NULL)
    {
        fprintf(stderr, "%s: filesystem not available\n", args[0]);
        return SYSERR;
    }

    spc = (ulong)fs->sectors_per_cluster;
    free_clusters = fatfs_count_free_clusters(fs);
    free_sectors = (ulong)free_clusters * spc;

    total_kib = (ulong)sd_capacity_blocks() / 2UL;      /* 2 * 512 B = 1 KiB   */
    free_kib = free_sectors / 2UL;

    printf("Volume : FAT32 on microSD (SDMMC slot 0)\n");
    printf("Cluster: %lu byte(s)\n", spc * 512UL);

    if (human)
    {
        printf("Total  : ");
        df_human(total_kib);
        printf("\n");
        printf("Free   : ");
        df_human(free_kib);
        printf(" (%lu cluster(s))\n", (unsigned long)free_clusters);
        printf("Used   : ");
        df_human(total_kib > free_kib ? total_kib - free_kib : 0UL);
        printf("\n");
    }
    else
    {
        printf("Total  : %lu KiB\n", total_kib);
        printf("Free   : %lu KiB (%lu cluster(s))\n",
               free_kib, (unsigned long)free_clusters);
        printf("Used   : %lu KiB\n",
               (total_kib > free_kib) ? total_kib - free_kib : 0UL);
    }

    return OK;
}
