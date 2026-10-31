/**
 * @file sdblkOpen.c
 *
 * SD0, the microSD card device for the ESP32-P4: open.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <device.h>
#include <sdblock.h>

devcall sdblkOpen(device *devptr)
{
    (void)devptr;
    return sd_present ? OK : SYSERR;
}
