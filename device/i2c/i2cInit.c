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

    /* The monitor must exist before any on-bus device can take it: I2C0 is
     * declared ahead of TOUCH0 in xinu.conf, so this init runs first. */
    i2cmon = moncreate();
    if (SYSERR == i2cmon)
    {
        return SYSERR;
    }

    i2c_init();

    kprintf("[i2c] ready\n");
    return OK;
}

/**
 * @ingroup i2c
 *
 * Lock the shared I2C0 bus monitor.  Single transactions do not need this:
 * i2cControl() takes the monitor itself.  Use it only to keep the bus across
 * several transactions that must not interleave (the monitor is recursive per
 * thread, so control(I2C0, ...) still works inside the held region).
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
