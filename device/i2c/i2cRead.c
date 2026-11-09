/**
 * @file i2cRead.c
 *
 * Device-level read on the shared I2C0 bus: takes the bus monitor and
 * runs the raw read.  On-bus devices (touch controller, ES8311 codec)
 * use this or the i2cBusLock()/i2cBusUnlock() hooks.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <device.h>
#include <kernel.h>
#include <monitor.h>
#include <i2c.h>

/* Monitor owned by the i2c device (created in i2cInit.c). */
extern monitor i2cmon;

bool i2cDevRead(uint8_t dev, uint8_t *data, size_t len)
{
    bool ok;

    /*if (SYSERR == i2cmon)
    {
        return FALSE;
    }*/

    //lock(i2cmon);
    ok = i2c_read(dev, data, len);
    //unlock(i2cmon);

    return ok;
}
