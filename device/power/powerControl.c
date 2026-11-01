/**
 * @file powerControl.c
 *
 * Device-level control del PMIC: el porcentaje de bateria y la instantanea
 * completa.  Ver include/power.h para los codigos.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <device.h>
#include <power.h>

/**
 * @ingroup power
 *
 * @param devptr power device table entry
 * @param func   command (POWER_CTRL_*)
 * @param arg1   command argument
 * @param arg2   command argument
 * @return OK, un valor, o SYSERR para un comando desconocido
 */
devcall powerControl(device *devptr, int func, long arg1, long arg2)
{
    (void)devptr;
    (void)arg2;

    switch (func)
    {
    case POWER_CTRL_PERCENT:
        return power_percent();

    case POWER_CTRL_INFO:
        return power_info((power_info_t *)arg1) ? OK : SYSERR;

    default:
        return SYSERR;
    }
}
