/**
 * @file sdblkSeek.c
 *
 * SD0, the microSD card device for the ESP32-P4: move the sector cursor.
 * Nothing reaches the card; the address travels with the next transfer.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <device.h>
#include <sdblock.h>

#include "sdCard.h"

devcall sdblkSeek(device *devptr, long offset)
{
    (void)devptr;

    if (offset < 0)
    {
        return SYSERR;
    }
    sdCardLock();
    sd_lba = (uint32_t)offset;
    sdCardUnlock();
    return OK;
}
