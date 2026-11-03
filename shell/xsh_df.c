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
#include <clock.h>
#include <memory.h>
#include <fsutil.h>
#include <fat_table.h>
#include <sdblock.h>

/* Sectores de FAT leidos por pasada.  El limite lo pone la memoria: 128
 * sectores son 64 KB de bufer, y es lo que se paga de PSRAM mientras dura el
 * escaneo.  Con 8 KB por pasada el escaneo ya baja de 27 s a pocos segundos;
 * con mas, el ahorro es de bloqueos de bus, no de busqueda. */
#define DF_SCAN_RUN 128U

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
    uint32 total_clusters;
    ulong spc;
    ulong total_kib;
    ulong free_sectors;
    ulong free_kib;
    ulong t0;
    uint8 *scanBuf;
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

    /* Total is the volume's data area, not the whole card: the boot sector,
     * the reserved sectors and both FAT copies live before the first
     * cluster, which starts at fs->cluster_begin_lba. */
    total_clusters = (sd_capacity_blocks() > fs->cluster_begin_lba)
        ? (uint32)((sd_capacity_blocks() - fs->cluster_begin_lba) / spc)
        : 0;

    printf("Volume : FAT32 on microSD (SDMMC slot 0)\n");
    printf("Cluster: %lu byte(s)\n", spc * 512UL);

    /* La copia entera de la FAT de esta tarjeta son ~15k sectores.  Se leen
     * en rafagas de DF_SCAN_RUN, con un SEEK y un bloqueo por rafaga: el
     * recuento es el mismo sector a sector, solo que en segundos.  El bufer
     * vive en PSRAM mientras dura el escaneo y se devuelve antes de informar,
     * para que `mem -p` no vea restos de un df. */
    scanBuf = (uint8 *)psram_malloc(DF_SCAN_RUN * FAT_SECTOR_SIZE);
    if (NULL == scanBuf)
    {
        fprintf(stderr, "%s: sin hueco de %u bytes en PSRAM\n", args[0],
                (unsigned)(DF_SCAN_RUN * FAT_SECTOR_SIZE));
        return SYSERR;
    }

    printf("Scanning: la FAT, %u sectores por lectura ... ", DF_SCAN_RUN);
    t0 = clktime;
    free_clusters = fatfs_count_free_clusters(fs, total_clusters + 2, scanBuf,
                                              DF_SCAN_RUN);
    printf("%lu s\n", (unsigned long)(clktime - t0));
    psram_free(scanBuf);

    free_sectors = (ulong)free_clusters * spc;

    total_kib = (ulong)total_clusters * spc / 2UL;       /* 2 * 512 B = 1 KiB */
    free_kib = free_sectors / 2UL;

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
