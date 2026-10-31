/**
 * @file fatcontrol.c
 *
 * control() on the `fat` device: report whether a volume is mounted, how big
 * the underlying card is, and rebuild the volume on request.  File operations
 * themselves are the library's (fl_*), not this device's.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <device.h>
#include <kernel.h>
#include <sdblock.h>

#include "fat.h"

devcall fatControl(device *devptr, int func, long arg1, long arg2)
{
    (void)devptr;
    (void)arg2;

    switch (func)
    {
    case FAT_CTRL_MOUNTED:
        return fatMounted() ? 1 : 0;

    case FAT_CTRL_BLOCKS:
        return control(SD0, SD_CTRL_BLOCKS, 0, 0);

    case FAT_CTRL_FORMAT:
        return fatFormat((const char *)arg1);

    default:
        return SYSERR;
    }
}
