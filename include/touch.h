/**
 * @file touch.h
 *
 * Touch device (FT6336 on the shared I2C0 bus).  The raw driver lives in
 * device/touch/; this device brings the controller out of reset and takes
 * the bus monitor of the `i2c` device around each transaction.  Polling, no
 * IRQ: TP_INT is wired but unused.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _TOUCH_H_
#define _TOUCH_H_

#include <device.h>
#include <device/touch/touch.h>

/* control(TOUCH0, CMD, arg1, arg2) */
#define TOUCH_CTRL_READ 0x01 /**< arg1=touch_point_t* arg2=cap -> count(0..cap) */
#define TOUCH_CTRL_TYPE 0x02 /**< -> touch_type_t                              */

/* Driver functions */
devcall touchInit(device *);
devcall touchRead(device *, void *, uint);
devcall touchControl(device *, int, long, long);

#endif                          /* _TOUCH_H_ */
