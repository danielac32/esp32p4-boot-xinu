/**
 * @file touchInit.c
 *
 * The `touch` device: brings the FT6336 out of reset and identifies it
 * through the raw driver (device/touch/touch.c).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <device.h>
#include <kernel.h>
#include <i2c.h>
#include <board.h>
#include <touch.h>

/**
 * @ingroup touch
 *
 * Release the controller from reset and detect it.  A missing controller is
 * reported, not fatal: read() then simply returns no points.
 *
 * @param devptr touch device table entry
 * @return OK
 */
devcall touchInit(device *devptr)
{
    (void)devptr;

    if (touch_init())
    {
        kprintf("[touch] FT6336 en 0x%02x\n", TOUCH_ADDR_FT6336);
    }
    else
    {
        kprintf("[touch] nada en 0x%02x (codigo %d)\n", TOUCH_ADDR_FT6336,
                i2c_last_error());
    }

    return OK;
}
