/**
 * @file sdmmcInit.c
 *
 * SDMMC host device (SDMMC0) for the ESP32-P4: init.
 *
 * The host brings up the mechanics of slot 0 and nothing else: the socket
 * rail, the IO_MUX pads, the clock tree and the controller itself.  Finding
 * and identifying a card is the job of the SD0 device one layer up.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <kernel.h>
#include <device.h>
#include <sdmmc.h>

#include "sdhost.h"

devcall sdmmcInit(device *devptr)
{
    (void)devptr;

    sdPadsInit();

    if (!sdControllerInit())
    {
        kprintf("[sdmmc] SDHOST controller did not come out of reset\r\n");
        return SYSERR;
    }

    return OK;
}
