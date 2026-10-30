/**
 * @file pmic.h
 *
 * PMIC device (AXP2101 on I2C0).  The raw driver lives in
 * drivers/power/; this device brings it up and serializes register
 * access with the shared I2C bus monitor (owned by device/i2c).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _PMIC_H_
#define _PMIC_H_

#include <device.h>
#include <drivers/power/axp2101.h>

/* control(PMIC0, CMD, arg1, arg2) */
#define PMIC_CTRL_PROBE        0x01 /**< arg1=(uint8_t*)id -> OK if found */
#define PMIC_CTRL_VBAT         0x02 /**< arg1=(int*)mV                     */
#define PMIC_CTRL_VBUS         0x03 /**< arg1=(int*)mV                     */
#define PMIC_CTRL_VSYS         0x04 /**< arg1=(int*)mV                     */
#define PMIC_CTRL_TEMP         0x05 /**< arg1=(int*)c_x10                  */
#define PMIC_CTRL_BAT_PERCENT  0x06 /**< arg1=(int*)pct                    */
#define PMIC_CTRL_STATUS1      0x07 /**< arg1=(uint8_t*)st                */
#define PMIC_CTRL_STATUS2      0x08 /**< arg1=(uint8_t*)st                */

/* Driver functions */
devcall pmicInit(device *);
devcall pmicControl(device *, int, long, long);

#endif                          /* _PMIC_H_ */
