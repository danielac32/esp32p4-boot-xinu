/**
 * @file disk.c
 *
 * Sector glue between fat_filelib and the microSD card device.  The library
 * asks for a run of 512-byte sectors; this file walks the run one sector at a
 * time, placing the card cursor with SD_CTRL_SEEK and moving the block with
 * read()/write() on SD0.  Nothing here touches SDMMC registers: the card
 * device and the host device below it own the bus, and the FAT lock the `fat`
 * device installs around every fl_* call keeps two logical operations from
 * interleaving their seek/transfer pairs.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <device.h>
#include <kernel.h>
#include <stdint.h>
#include <sdblock.h>

#include "disk.h"
#include "fat_filelib.h"

/**
 * Hook the FAT library to the card and read the volume's BPB.
 *
 * The library keeps the callbacks and the partition geometry; the locks are
 * already installed by the `fat` device.  Formatting is the device's
 * decision, not this glue's.
 *
 * @return FAT_INIT_OK on success, a negative FAT_INIT_* code otherwise
 */
int fatfs_mount(void)
{
    fl_init();
    return fl_attach_media(sd_readsector, sd_writesector);
}

/**
 * Open the card and report its size.
 * @return number of 512-byte sectors, or 0 if there is no usable card
 */
int sd_init(void)
{
    long blocks;

    if (SYSERR == open(SD0))
    {
        return 0;
    }
    blocks = control(SD0, SD_CTRL_BLOCKS, 0, 0);
    return (SYSERR == blocks || blocks <= 0) ? 0 : (int)blocks;
}

/**
 * Read @p sector_count sectors starting at @p sector into @p buffer.
 * @return 1 on success, 0 on failure (the library's convention)
 *
 * Una busqueda y una sola pasada para toda la rafaga: SD0 mueve el cursor del
 * host bloque a bloque dentro de la misma llamada, asi que N sectores cuestan
 * un bloqueo de tarjeta, un bloqueo de bus y un SEEK.  Antes cada sector se
 * pedia con su propio SEEK y su propia lectura: 1,8 ms por sector y 27 s para
 * la copia entera de la FAT de esta tarjeta.
 */
int sd_readsector(uint32 sector, uint8 *buffer, uint32 sector_count)
{
    uint32 bytes = sector_count * FAT_SECTOR_SIZE;

    if (NULL == buffer || 0 == sector_count)
    {
        return 0;
    }

    if (SYSERR == control(SD0, SD_CTRL_SEEK, (long)sector, 0)
        || (devcall)bytes != read(SD0, buffer, bytes))
    {
        return 0;
    }
    return 1;
}

/**
 * Write @p sector_count sectors starting at @p sector from @p buffer.
 * @return 1 on success, 0 on failure (the library's convention)
 */
int sd_writesector(uint32 sector, uint8 *buffer, uint32 sector_count)
{
    uint32 i;

    if (NULL == buffer || 0 == sector_count)
    {
        return 0;
    }

    for (i = 0; i < sector_count; i++)
    {
        if (SYSERR == control(SD0, SD_CTRL_SEEK, (long)(sector + i), 0)
            || SYSERR == write(SD0, buffer + (i * FAT_SECTOR_SIZE),
                               FAT_SECTOR_SIZE))
        {
            return 0;
        }
    }
    return 1;
}
