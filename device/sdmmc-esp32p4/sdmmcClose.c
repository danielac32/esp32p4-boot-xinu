/**
 * @file sdmmcClose.c
 *
 * SDMMC host device (SDMMC0) for the ESP32-P4: close.  The block stays
 * powered and clocked: slot 1 of the same controller may still be in use.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <device.h>

devcall sdmmcClose(device *devptr)
{
    (void)devptr;
    return OK;
}
