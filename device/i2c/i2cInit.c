/**
 * @file i2cInit.c
 *
 * The `i2c` device: ESP32-P4 I2C0 master bus glue.  Brings the bus up
 * through the raw driver (device/i2c/i2c.c) and owns the kernel monitor
 * that serializes the shared bus (touch + ES8311 + AXP2101).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <device.h>
#include <kernel.h>
#include <monitor.h>
#include <i2c.h>

/* One physical I2C master on this board, so a single monitor suffices. */
monitor i2cmon = SYSERR;

/**
 * @ingroup i2c
 *
 * Initialize the I2C0 master and the bus monitor.
 *
 * @param devptr I2C device table entry
 * @return OK, or SYSERR if the monitor could not be created
 */
devcall i2cInit(device *devptr)
{
    (void)devptr;

    /*i2cmon = moncreate();
    if (SYSERR == i2cmon)
    {
        return SYSERR;
    }*/

    i2c_init();

    kprintf("[i2c] ready\n");
    return OK;
}

/**
 * @ingroup i2c
 *
 * Lock the shared I2C0 bus monitor.  On-bus devices that drive the raw
 * driver directly (the touch controller, and later the ES8311 codec)
 * bracket their transactions with this hook.
 */
void i2cBusLock(void)
{
    if (SYSERR != i2cmon)
    {
        lock(i2cmon);
    }
}

/**
 * @ingroup i2c
 *
 * Unlock the shared I2C0 bus monitor.
 */
void i2cBusUnlock(void)
{
    if (SYSERR != i2cmon)
    {
        unlock(i2cmon);
    }
}
