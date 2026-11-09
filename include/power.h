/**
 * @file power.h
 *
 * Device `power`: el PMIC AXP2101 del bus I2C0 compartido, como medidor de
 * bateria de solo lectura (porcentaje, tension, estado del cargador).  El
 * driver crudo esta en device/power/; el bus lo serializa el device `i2c`.
 *
 * No hay controles de escritura a proposito: el PMIC manda la rail de 3V3 y
 * puede apagar o resetear el SoC desde sus registros de configuracion.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _POWER_H_
#define _POWER_H_

#include <device.h>
#include <device/power/power.h>

/* control(POWER0, CMD, arg1, arg2) */
#define POWER_CTRL_PERCENT 0x01 /**< -> 0..100, SYSERR si no hay PMIC        */
#define POWER_CTRL_INFO    0x02 /**< arg1=power_info_t* -> OK o SYSERR       */

/* Driver functions */
devcall powerInit(device *);
devcall powerControl(device *, int, long, long);

#endif                          /* _POWER_H_ */
