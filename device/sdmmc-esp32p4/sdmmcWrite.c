/**
 * @file sdmmcWrite.c
 *
 * SDMMC host device (SDMMC0) for the ESP32-P4: block write at the host's
 * own cursor.  Same shape as the read: whole 512 byte blocks, one CMD24
 * per block, the whole run under one bus lock.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <device.h>
#include <string.h>
#include <sdmmc.h>

#include "sdhost.h"

/**
 * Write \p len bytes to the card at the host cursor, then advance it.
 * \p len is rounded up to whole blocks; the caller's buffer must hold that
 * much.
 */
devcall sdmmcWrite(device *devptr, const void *buf, uint len)
{
    uint32_t blocks;
    uint32_t i;
    const uint8_t *src = (const uint8_t *)buf;

    (void)devptr;

    if (!sd_host_ready || NULL == buf || 0 == len)
    {
        return SYSERR;
    }

    blocks = (len + SD_BLOCK_SIZE - 1) / SD_BLOCK_SIZE;
    sdmmcBusLock();
    for (i = 0; i < blocks; i++)
    {
        sdBlockCopyIn(src, SD_BLOCK_SIZE);
        if (!sdWriteBlock(sd_host_lba + i))
        {
            sdmmcBusUnlock();
            return SYSERR;
        }
        src += SD_BLOCK_SIZE;
    }
    sd_host_lba += blocks;
    sdmmcBusUnlock();
    return (devcall)len;
}
