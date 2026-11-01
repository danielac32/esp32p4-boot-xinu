/**
 * @file gpioInit.c
 *
 * GPIO device init: bring up the ESP32-P4 GPIO/IO_MUX controller.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <device.h>
#include <kernel.h>
#include <gpio.h>

/**
 * @ingroup gpio
 *
 * Initialize the GPIO controller.
 *
 * @param devptr GPIO device table entry
 * @return OK
 */
devcall gpioInit(device *devptr)
{
    (void)devptr;

    gpio_init();

    kprintf("[gpio] ready\n");
    return OK;
}
