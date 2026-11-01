/**
 * @file     xsh_power.c
 *
 * Shell command (power): la carga de la bateria medida por el PMIC, mas la
 * tension de la celda y quien la esta alimentando.  Todo a traves del
 * descriptor /dev/power0; el comando no toca el bus directamente.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <device.h>
#include <power.h>

static const char *chgtext(uint8_t code)
{
    switch (code)
    {
    case POWER_CHG_TRICKLE:
        return "precarga";
    case POWER_CHG_FAST:
        return "corriente constante";
    case POWER_CHG_ABSORB:
        return "tension constante";
    case POWER_CHG_DONE:
        return "completada";
    case POWER_CHG_NONE:
        return "inactiva";
    default:
        return "reservado";
    }
}

static const char *curtext(uint8_t code)
{
    switch (code)
    {
    case POWER_CUR_STANDBY:
        return "en reposo";
    case POWER_CUR_CHARGE:
        return "hacia la bateria";
    case POWER_CUR_DISCHARGE:
        return "de la bateria";
    default:
        return "reservada";
    }
}

/**
 * @ingroup shell
 *
 * Shell command (power)
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_power(int nargs, char *args[])
{
    power_info_t info;
    int powerfd;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s\n\n", args[0]);
        printf("Description:\n");
        printf("\tBattery charge, cell voltage and charger state as read\n");
        printf("\tfrom the AXP2101 PMIC through /dev/power0.\n");
        return OK;
    }

    if (nargs > 2)
    {
        fprintf(stderr, "%s: too many arguments\n", args[0]);
        return SYSERR;
    }

    powerfd = getdev("/dev/power0");
    if (SYSERR == powerfd)
    {
        fprintf(stderr, "%s: /dev/power0 is not mounted\n", args[0]);
        return SYSERR;
    }

    if (OK != control(powerfd, POWER_CTRL_INFO, (long)&info, 0))
    {
        fprintf(stderr, "%s: el PMIC no responde\n", args[0]);
        close(powerfd);
        return SYSERR;
    }

    close(powerfd);

    printf("bateria:  %u %%\n", (unsigned)info.percent);
    printf("tension:  %u mV\n", (unsigned)info.vbat_mv);
    printf("cargador: %s\n", chgtext(info.charge));
    printf("corriente: %s\n", curtext(info.current));

    printf("entrada:  VBUS %s, bateria %s, sistema %s\n",
           info.vbus_good ? "valida" : "ausente",
           info.battery ? "presente" : "ausente",
           info.system_on ? "encendido" : "apagado");

    return OK;
}
