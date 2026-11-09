/**
 * @file sdblkWrite.c
 *
 * SD0, the microSD card device for the ESP32-P4: block write at the card's
 * cursor.  Same locking as the read: the card lock holds the cursor, the
 * bus lock makes the seek/transfer pair one transaction on the controller.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <device.h>
#include <sdblock.h>
#include <sdmmc.h>

#include "sdCard.h"

/**
 * Write \p len bytes to the card at the device cursor, then advance it.
 * \p len is rounded up to whole 512 byte blocks; the caller's buffer must
 * hold that much.
 */
devcall sdblkWrite(device *devptr, const void *buf, uint len)
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
        && (devcall)len == write(SDMMC0, buf, len))
    {
        sd_lba += blocks;
        rc = (devcall)len;
    }
    sdmmcBusUnlock();
    sdCardUnlock();
    return rc;
}
