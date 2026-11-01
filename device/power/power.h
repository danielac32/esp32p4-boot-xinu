/**
 * @file power.h
 *
 * Driver crudo del AXP2101 (el PMIC del bus I2C0 compartido): solo lecturas
 * de estado.  El device `power` (device/power) lo envuelve detras de
 * control(); aqui no se toma ningun monitor, porque cada transaccion va por
 * control(I2C0, ...) y es i2cControl() quien serializa el bus.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _POWERDRV_H_
#define _POWERDRV_H_

#include <stdbool.h>
#include <stdint.h>

/** Alimentacion y bateria, en una sola pasada de lecturas. */
typedef struct {
    uint8_t percent;            /**< 0xA4: carga de la bateria, 0..100      */
    uint16_t vbat_mv;           /**< ADC canal 0: 1 mV por LSB              */
    bool vbus_good;             /**< 0x00[5]: la entrada VBUS es valida     */
    bool battery;               /**< 0x00[3]: hay bateria conectada         */
    bool system_on;             /**< 0x01[4]: el sistema esta encendido     */
    uint8_t charge;             /**< 0x01[2:0], ver POWER_CHG_*             */
    uint8_t current;            /**< 0x01[6:5], ver POWER_CUR_*             */
} power_info_t;

/* Bits 2:0 de 0x01 (PMU status2): estado del cargador. */
#define POWER_CHG_TRICKLE  0    /**< 000: precarga a corriente reducida    */
#define POWER_CHG_FAST     2    /**< 010: corriente constante              */
#define POWER_CHG_ABSORB   3    /**< 011: tension constante                */
#define POWER_CHG_DONE     4    /**< 100: carga terminada                  */
#define POWER_CHG_NONE     5    /**< 101: no esta cargando                 */

/* Bits 6:5 de 0x01: hacia donde va la corriente de la bateria. */
#define POWER_CUR_STANDBY    0
#define POWER_CUR_CHARGE     1
#define POWER_CUR_DISCHARGE  2

/* Identifica el PMIC (ChipID 0x03 == 0x4A).  False si no responde. */
bool power_init(void);

bool power_present(void);

/* Carga en %.  SYSERR si no hay PMIC o si falla la lectura. */
int power_percent(void);

/* Instantanea completa; false si alguna lectura falla. */
bool power_info(power_info_t *info);

#endif                          /* _POWERDRV_H_ */
