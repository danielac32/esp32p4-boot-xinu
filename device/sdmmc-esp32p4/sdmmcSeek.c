/**
 * @file sdmmcSeek.c
 *
 * SDMMC host device (SDMMC0) for the ESP32-P4: move the host cursor.
 * Nothing is sent to the card; the address travels with the next CMD17 or
 * CMD24.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <device.h>
#include <sdmmc.h>

#include "sdhost.h"

devcall sdmmcSeek(device *devptr, long offset)
{
    (void)devptr;

    if (offset < 0)
    {
        return SYSERR;
    }
    sd_host_lba = (uint32_t)offset;
    return OK;
}
