/**
 * @file sdmmcRead.c
 *
 * SDMMC host device (SDMMC0) for the ESP32-P4: block read at the host's
 * own cursor.
 *
 * No card policy lives here: the caller has already decided that the card
 * is present, selected and addressed at \c sd_host_lba.  Each block is one
 * CMD17 through the IDMAC, and the whole run is one bus transaction so
 * slot 1 cannot slip in between blocks.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <device.h>
#include <string.h>
#include <sdmmc.h>

#include "sdhost.h"

/**
 * Read \p len bytes from the card at the host cursor, then advance it.
 * Transfers are whole 512 byte blocks, so \p len is rounded up; the
 * caller's buffer must hold that much.
 */
devcall sdmmcRead(device *devptr, void *buf, uint len)
{
    uint32_t blocks;
    uint32_t i;
    uint8_t *dst = (uint8_t *)buf;

    (void)devptr;

    if (!sd_host_ready || NULL == buf || 0 == len)
    {
        return SYSERR;
    }

    blocks = (len + SD_BLOCK_SIZE - 1) / SD_BLOCK_SIZE;
    sdmmcBusLock();
    for (i = 0; i < blocks; i++)
    {
        if (!sdReadBlock(sd_host_lba + i))
        {
            sdmmcBusUnlock();
            return SYSERR;
        }
        if ((i + 1) == blocks)
        {
            sdBlockCopyOut(dst, len - (i * SD_BLOCK_SIZE));
        }
        else
        {
            sdBlockCopyOut(dst, SD_BLOCK_SIZE);
        }
        dst += SD_BLOCK_SIZE;
    }
    sd_host_lba += blocks;
    sdmmcBusUnlock();
    return (devcall)len;
}
