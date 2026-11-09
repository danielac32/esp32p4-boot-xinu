/**
 * @file sdmmcOpen.c
 *
 * SDMMC host device (SDMMC0) for the ESP32-P4: open.  The controller is
 * up once init has run; there is nothing per-open to acquire.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <device.h>
#include <sdmmc.h>

#include "sdhost.h"

devcall sdmmcOpen(device *devptr)
{
    (void)devptr;
    return sd_host_ready ? OK : SYSERR;
}
