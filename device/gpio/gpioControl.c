/**
 * @file gpioControl.c
 *
 * GPIO pin operations through control().  See include/gpio.h for the
 * command codes (control(GPIO0, CMD, pin, value)).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <device.h>
#include <kernel.h>
#include <gpio.h>

/**
 * @ingroup gpio
 *
 * Control a GPIO pin.
 *
 * @param devptr GPIO device table entry
 * @param func   command (GPIO_CTRL_*)
 * @param arg1   pin number
 * @param arg2   command-dependent value
 * @return OK/SYSERR, or the pin level for GPIO_CTRL_READ
 */
devcall gpioControl(device *devptr, int func, long arg1, long arg2)
{
    (void)devptr;

    switch (func)
    {
    case GPIO_CTRL_SET_DIR:
        gpio_set_dir((uint8_t) arg1, (gpio_dir_t) arg2);
        return OK;

    case GPIO_CTRL_SET_PULL:
        gpio_set_pull((uint8_t) arg1, (gpio_pull_t) arg2);
        return OK;

    case GPIO_CTRL_SET_OPEN_DRAIN:
        gpio_set_open_drain((uint8_t) arg1, (bool) arg2);
        return OK;

    case GPIO_CTRL_WRITE:
        gpio_write((uint8_t) arg1, (bool) arg2);
        return OK;

    case GPIO_CTRL_READ:
        return gpio_read((uint8_t) arg1);

    case GPIO_CTRL_TOGGLE:
        gpio_toggle((uint8_t) arg1);
        return OK;

    default:
        return SYSERR;
    }
}
