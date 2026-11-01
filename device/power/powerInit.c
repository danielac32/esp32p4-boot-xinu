/**
 * @file powerInit.c
 *
 * El device `power`: identifica el PMIC del bus I2C0 a traves del driver
 * crudo (device/power/power.c).  Un PMIC ausente se informa y no es fatal:
 * control() simplemente devuelve SYSERR.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <device.h>
#include <kernel.h>
#include <board.h>
#include <i2c.h>
#include <power.h>

/**
 * @ingroup power
 *
 * @param devptr power device table entry
 * @return OK
 */
devcall powerInit(device *devptr)
{
    int percent;

    (void)devptr;

    if (!power_init())
    {
        kprintf("[power] nada en 0x%02x (codigo %d)\n", AXP2101_ADDR,
                i2c_last_error());
        return OK;
    }

    percent = power_percent();
    if (SYSERR == percent)
    {
        kprintf("[power] AXP2101 en 0x%02x sin medidor de bateria\n",
                AXP2101_ADDR);
        return OK;
    }

    kprintf("[power] AXP2101 en 0x%02x: bateria al %d%%\n", AXP2101_ADDR,
            percent);

    return OK;
}
