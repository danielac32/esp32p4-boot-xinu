/**
 * @file sdblkRead.c
 *
 * SD0, the microSD card device for the ESP32-P4: block read at the card's
 * cursor.
 *
 * The card layer owns the cursor and the presence check; the host device
 * (SDMMC0) moves the blocks.  The card lock keeps another card user off the
 * cursor for the whole operation and the bus lock under it makes the
 * seek/transfer pair one transaction, so slot 1 cannot slip in between.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <device.h>
#include <sdblock.h>
#include <sdmmc.h>

#include "sdCard.h"

/**
 * Read \p len bytes from the card at the device cursor, then advance it.
 * Transfers are whole 512 byte blocks, so \p len is rounded up; the
 * caller's buffer must hold that much.
 */
devcall sdblkRead(device *devptr, void *buf, uint len)
{
    uint32_t blocks;
    devcall rc = SYSERR;

    (void)devptr;

    if (NULL == buf || 0 == len)
    {
        return SYSERR;
    }

    blocks = (len + SD_BLOCK_SIZE - 1) / SD_BLOCK_SIZE;
    sdCardLock();
    sdmmcBusLock();
    if (sd_present
        && SYSERR != control(SDMMC0, SDMMC_CTRL_SEEK, (long)sd_lba, 0)
        && (devcall)len == read(SDMMC0, buf, len))
    {
        sd_lba += blocks;
        rc = (devcall)len;
    }
    sdmmcBusUnlock();
    sdCardUnlock();
    return rc;
}
