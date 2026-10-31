/**
 * @file sdblkClose.c
 *
 * SD0, the microSD card device for the ESP32-P4: close.  The card stays
 * selected and clocked; there is no per-open state to drop.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <device.h>

devcall sdblkClose(device *devptr)
{
    (void)devptr;
    return OK;
}
