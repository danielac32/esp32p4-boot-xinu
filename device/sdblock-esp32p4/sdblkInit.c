/**
 * @file sdblkInit.c
 *
 * SD0, the microSD card device for the ESP32-P4: init.
 *
 * The host device (SDMMC0) has already been initialised by the time this
 * runs; init only opens it and identifies the card that is plugged in.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <kernel.h>
#include <device.h>
#include <sdblock.h>
#include <sdmmc.h>

#include "sdCard.h"

devcall sdblkInit(device *devptr)
{
    struct sdmmcinfo hi;
    uint32_t khz;

    (void)devptr;

    /* The card is a shared resource from here on: FAT and any raw user
     * serialize on this monitor. */
    sdCardMonInit();

    if (SYSERR == open(SDMMC0))
    {
        kprintf("[sd] SDMMC host did not come up\r\n");
        return SYSERR;
    }

    if (!sdIdentify())
    {
        /* sd_hwerr says what the controller saw at that moment: an error
         * interrupt, a command still inhibited, or nothing at all. */
        kprintf("[sd] no card on SDMMC slot 0 (stopped at %s, hwerr 0x%08lX)\r\n",
                sd_fail, (unsigned long)sd_hwerr);
        return SYSERR;
    }

    khz = 0;
    if (OK == control(SDMMC0, SDMMC_CTRL_INFO, (long)&hi, 0))
    {
        khz = hi.card_khz;
    }

    kprintf("[sd] %lu blocks (%lu MB), %s, %u-bit bus, %lu kHz\r\n",
            (unsigned long)sd_blocks,
            (unsigned long)(sd_blocks / 2048UL),
            sd_hc ? "SDHC/SDXC" : "SDSC", sd_width, (unsigned long)khz);
    return OK;
}
