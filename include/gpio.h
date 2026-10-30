/**
 * @file gpio.h
 *
 * GPIO device interface.  The raw ESP32-P4 GPIO/IO_MUX driver lives in
 * drivers/gpio (gpio_init/gpio_set_dir/...); this device only initializes
 * it and exposes the pin operations through control().
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _GPIO_H_
#define _GPIO_H_

#include <device.h>
#include <drivers/gpio/gpio.h>

/* gpioControl() commands: control(GPIO0, CMD, pin, value) */
#define GPIO_CTRL_SET_DIR        0x01 /**< arg1=pin arg2=gpio_dir_t      */
#define GPIO_CTRL_SET_PULL       0x02 /**< arg1=pin arg2=gpio_pull_t     */
#define GPIO_CTRL_SET_OPEN_DRAIN 0x03 /**< arg1=pin arg2=enable          */
#define GPIO_CTRL_WRITE          0x04 /**< arg1=pin arg2=value           */
#define GPIO_CTRL_READ           0x05 /**< arg1=pin returns 0/1          */
#define GPIO_CTRL_TOGGLE         0x06 /**< arg1=pin                      */

/* Driver functions */
devcall gpioInit(device *);
devcall gpioControl(device *, int, long, long);

#endif                          /* _GPIO_H_ */
