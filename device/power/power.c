/**
 * @file power.c
 *
 * AXP2101 (X-Powers) en el bus I2C0 compartido, con direccion TWSI fija 0x34.
 *
 * El device es intencionadamente de solo lectura.  0x10 contiene los bits
 * RWAC de soft PWROFF y de reset del SoC, y 0x80/0x90/0x91 apagan las DCDC y
 * las LDO del rail de 3V3 que alimenta al propio P4: una escritura cruda
 * puede tumbar la placa.  Sin escrituras no hay forma de hacerlo desde aqui.
 *
 * El medidor de combustible viene habilitado de fabrica (0x18[3] = 1) y el
 * canal ADC de VBAT tambien (0x30[0] = 1), asi que el porcentaje y la
 * tension se leen sin configurar nada.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <device.h>
#include <board.h>
#include <i2c.h>
#include "power.h"

#define AXP_REG_STATUS1     0x00U   /* RO: VBUS, BATFET, bateria presente    */
#define AXP_REG_STATUS2     0x01U   /* RO: cargador y corriente de bateria   */
#define AXP_REG_CHIPID      0x03U   /* RO: 0x4A en el AXP2101                */
#define AXP_REG_VBAT_H      0x34U   /* vbat[13:8] en los bits 5:0            */
#define AXP_REG_VBAT_L      0x35U   /* vbat[7:0]                             */
#define AXP_REG_SOC         0xA4U   /* RO: porcentaje de la bateria          */

#define AXP_CHIPID_VALUE    0x4AU
#define AXP_SOC_MAX         100U

static bool pmic;

/**
 * Leer registros consecutivos del PMIC.  El acceso pasa por el device I2C0,
 * que toma el monitor del bus alrededor de la transaccion completa.
 */
static bool axp_read(uint8_t reg, uint8_t *buf, size_t len)
{
    struct i2c_req req;

    req.reg = reg;
    req.buf = buf;
    req.len = len;

    return OK == control(I2C0, I2C_CTRL_READ_REGS, AXP2101_ADDR, (long)&req);
}

/**
 * @ingroup power
 *
 * Confirmar que el PMIC esta en el bus y es un AXP2101.
 * @return true cuando responde con el ChipID correcto
 */
bool power_init(void)
{
    uint8_t id = 0;

    pmic = false;

    if (OK != control(I2C0, I2C_CTRL_PROBE, AXP2101_ADDR, 0))
    {
        return false;
    }

    if (!axp_read(AXP_REG_CHIPID, &id, 1) || AXP_CHIPID_VALUE != id)
    {
        return false;
    }

    pmic = true;
    return true;
}

bool power_present(void)
{
    return pmic;
}

/**
 * @ingroup power
 *
 * Carga de la bateria segun el medidor de combustible del PMIC.
 * @return 0..100, o SYSERR si no hay PMIC o si falla la lectura
 */
int power_percent(void)
{
    uint8_t soc;

    if (!pmic || !axp_read(AXP_REG_SOC, &soc, 1))
    {
        return SYSERR;
    }

    return (soc > AXP_SOC_MAX) ? (int)AXP_SOC_MAX : (int)soc;
}

/**
 * @ingroup power
 *
 * Estado completo de alimentacion y bateria.  Son tres transacciones: los
 * registros 0x00/0x01 van juntos, y 0x34/0x35 tambien.
 *
 * @param info donde escribir el resultado
 * @return false si no hay PMIC o si falla alguna lectura
 */
bool power_info(power_info_t *info)
{
    uint8_t status[2];
    uint8_t adc[2];
    uint8_t soc;

    if (info == NULL || !pmic)
    {
        return false;
    }

    if (!axp_read(AXP_REG_STATUS1, status, sizeof(status)) ||
        !axp_read(AXP_REG_VBAT_H, adc, sizeof(adc)) ||
        !axp_read(AXP_REG_SOC, &soc, 1))
    {
        return false;
    }

    /* El canal del ADC es de 14 bits; los dos bits altos de 0x34 son de
     * depuracion del canal, no forman parte de la medida. */
    info->vbat_mv = (uint16_t)((((uint16_t) (adc[0] & 0x3FU)) << 8) | adc[1]);
    info->percent = (soc > AXP_SOC_MAX) ? AXP_SOC_MAX : soc;
    info->vbus_good = (status[0] & 0x20U) != 0;
    info->battery = (status[0] & 0x08U) != 0;
    info->system_on = (status[1] & 0x10U) != 0;
    info->charge = status[1] & 0x07U;
    info->current = (uint8_t)((status[1] >> 5) & 0x03U);

    return true;
}
