/**
 * @file sdblkControl.c
 *
 * SD0, the microSD card device for the ESP32-P4: control.
 *
 * Each request is answered under the card lock: the cursor, the presence
 * flag and the identification registers are the card's own state, and a
 * caller must not see them change in the middle of one request.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <device.h>
#include <stdlib.h>
#include <string.h>
#include <sdblock.h>
#include <sdmmc.h>

#include "sdCard.h"

static devcall sdblkControlLocked(int func, long arg1)
{

    switch (func)
    {
    case SD_CTRL_PRESENT:
        return sd_present ? 1 : 0;

    case SD_CTRL_SEEK:
        if (arg1 < 0)
        {
            return SYSERR;
        }
        sd_lba = (uint32_t)arg1;
        return OK;

    case SD_CTRL_BLOCKS:
        return (devcall)sd_blocks;

    case SD_CTRL_HIGHCAP:
        return sd_hc ? 1 : 0;

    case SD_CTRL_INFO:
    {
        struct sdinfo *info = (struct sdinfo *)arg1;
        struct sdmmcinfo hi;

        if (NULL == info)
        {
            return SYSERR;
        }
        bzero(info, sizeof(*info));
        info->blocks = sd_blocks;
        info->rca = (uint16_t)sd_rca;
        info->present = sd_present ? 1 : 0;
        info->highcap = sd_hc ? 1 : 0;
        info->bus_width = sd_width;
        if (OK == control(SDMMC0, SDMMC_CTRL_INFO, (long)&hi, 0))
        {
            info->card_clk_khz = hi.card_khz;
        }
        return OK;
    }

    case SD_CTRL_RESET:
        if (!sd_present)
        {
            return SYSERR;
        }
        /* sdIdentify() takes the card and bus locks itself. */
        return sdIdentify() ? OK : SYSERR;

    case SD_CTRL_OCR:
        return (devcall)sd_ocr;

    case SD_CTRL_CID:
    case SD_CTRL_CSD:
    {
        uint32_t *dst = (uint32_t *)arg1;
        const uint32_t *src = (SD_CTRL_CID == func) ? sd_cid : sd_csd;

        if (NULL == dst)
        {
            return SYSERR;
        }
        memcpy(dst, src, 4 * sizeof(uint32_t));
        return OK;
    }

    default:
        return SYSERR;
    }
}

devcall sdblkControl(device *devptr, int func, long arg1, long arg2)
{
    devcall rc;

    (void)devptr;
    (void)arg2;

    sdCardLock();
    rc = sdblkControlLocked(func, arg1);
    sdCardUnlock();
    return rc;
}
