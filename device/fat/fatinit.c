/**
 * @file fatinit.c
 *
 * The `fat` device: one FAT volume on the microSD card.  sysinit() runs this
 * after SDMMC0 and SD0 have identified the card.  The init installs the
 * library's reentrant monitor, mounts the volume, and — when the card holds
 * no FAT volume (a blank card, or one carrying only an MBR) — formats it once
 * and mounts it again, the policy the ESP32-S3 port also follows.
 *
 * The monitor matters: the library nests several of its calls (fl_remove
 * opens a directory, fl_fputc writes through fl_fwrite), so a plain
 * semaphore would deadlock; a kernel monitor is reentrant by owner.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <device.h>
#include <fatfs.h>
#include <kernel.h>
#include <monitor.h>
#include <sdblock.h>
#include <stddef.h>

#include "disk.h"
#include "fat.h"
#include "fat_filelib.h"

static monitor fatmon = SYSERR;
bool fat_mounted = FALSE;

/** Lock hook handed to the library: no-op until the monitor exists. */
static void fatlib_lock(void)
{
    if (SYSERR != fatmon)
    {
        lock(fatmon);
    }
}

/** Unlock hook handed to the library. */
static void fatlib_unlock(void)
{
    if (SYSERR != fatmon)
    {
        unlock(fatmon);
    }
}

void fatLibLock(void)
{
    if (SYSERR != fatmon)
    {
        lock(fatmon);
    }
}

void fatLibUnlock(void)
{
    if (SYSERR != fatmon)
    {
        unlock(fatmon);
    }
}

devcall fatInit(device *devptr)
{
    long blocks;

    (void)devptr;

    fatmon = moncreate();
    if (SYSERR == fatmon)
    {
        kprintf("[fat] no monitor for the FAT library\r\n");
        return SYSERR;
    }
    fl_attach_locks(fatlib_lock, fatlib_unlock);

    blocks = sd_init();
    if (0 == blocks)
    {
        kprintf("[fat] microSD not available\r\n");
        return SYSERR;
    }

    if (FAT_INIT_OK != fatfs_mount())
    {
        kprintf("[fat] no FAT volume, formatting the card...\r\n");
        if (SYSERR == fatFormat(NULL))
        {
            kprintf("[fat] format failed\r\n");
            return SYSERR;
        }
    }

    fat_mounted = TRUE;
    kprintf("[fat] volume mounted: %lu blocks of 512 B (%lu MiB)\r\n",
            (unsigned long)blocks, (unsigned long)(blocks / 2048));

//kprintf("mounted : %d\n",fatMounted() ? 1 : 0);
    return OK;
}

bool fatMounted(void)
{
    return fat_mounted;
}

/**
 * Erase the card and build a fresh FAT32 volume over all of it.
 *
 * The library walks the new FAT one sector at a time, so a 58 GiB card takes
 * close to a minute; the library monitor is held for that whole time, which
 * is what keeps another thread from reading a half-written volume.  Because
 * fl_format() only locks around itself, the remount that follows runs under
 * the same hold.
 */
devcall fatFormat(const char *label)
{
    long blocks;
    bool ok;

    blocks = control(SD0, SD_CTRL_BLOCKS, 0, 0);
    if (blocks <= 0)
    {
        kprintf("[fat] format: no microSD card\r\n");
        return SYSERR;
    }

    if (NULL == label || '\0' == *label)
    {
        label = FAT_DEFAULT_LABEL;
    }

    fatLibLock();
    ok = fl_format((uint32)blocks, label) && FAT_INIT_OK == fatfs_mount();
    fatLibUnlock();

    fat_mounted = ok;
    if (!ok)
    {
        kprintf("[fat] format of %lu blocks failed\r\n", (unsigned long)blocks);
        return SYSERR;
    }

    kprintf("[fat] formatted %lu blocks (%lu MiB) as FAT32, label \"%s\"\r\n",
            (unsigned long)blocks, (unsigned long)(blocks / 2048), label);
    return OK;
}
