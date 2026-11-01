/**
 * @file es8311.c
 *
 * Control por I2C del codec ES8311.  Cada transaccion sale por el device
 * I2C0 (control(I2C0, ...)), que es quien toma el monitor del bus: este
 * fichero no serializa nada ni puede hacerlo.
 *
 * Tabla de coeficientes y secuencias de registros, copiadas del driver de
 * referencia del SDK (drivers/es8311/es8311.c, derivado del componente
 * ES8311 de esp-bsp de Espressif).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <clock.h>
#include <device.h>
#include <board.h>
#include <i2c.h>
#include "es8311.h"

/* Registro 0x00: reset de bloques (0x1F), modo serie (bit 6) y enable (bit 7). */
#define ES_REG_RESET       0x00U
#define ES_REG_CLK01       0x01U    /* fuente de MCLK e inversiones        */
#define ES_REG_CLK02       0x02U    /* pre_div / pre_multi                 */
#define ES_REG_CLK03       0x03U    /* fs_mode + adc_osr                   */
#define ES_REG_CLK04       0x04U    /* dac_osr                             */
#define ES_REG_CLK05       0x05U    /* adc_div | dac_div                   */
#define ES_REG_CLK06       0x06U    /* bclk_div en [4:0], SCLK en [5]      */
#define ES_REG_CLK07       0x07U    /* lrck_h en [5:0]                     */
#define ES_REG_CLK08       0x08U    /* lrck_l                              */
#define ES_REG_SDPIN       0x09U    /* formato del slot de datos entrante  */
#define ES_REG_SDPOUT      0x0AU    /* formato del slot de datos saliente  */
#define ES_REG_SYS0D       0x0DU
#define ES_REG_SYS0E       0x0EU
#define ES_REG_SYS12       0x12U
#define ES_REG_SYS13       0x13U
#define ES_REG_SYS14       0x14U    /* ruta del microfono: entrada + PGA     */
#define ES_REG_ADC15       0x15U    /* fade del ADC                        */
#define ES_REG_ADC16       0x16U    /* escala digital del ADC              */
#define ES_REG_ADC17       0x17U    /* volumen digital del ADC             */
#define ES_REG_ADC1C       0x1CU
#define ES_REG_DAC31       0x31U    /* mute de la salida DAC (bits 6|5)    */
#define ES_REG_DAC32       0x32U    /* volumen del DAC, 0..255             */
#define ES_REG_DAC37       0x37U    /* fade del DAC                        */

#define ES_BIT(n)          (1U << (n))

/* Ancho de muestra en los bits 4:2 de los dos registros de formato. */
#define ES_FORMAT_16BIT    (3U << 2)

/* Volumen digital del ADC con el que se abre la ruta (registro 0x17): 200 =
 * +4,5 dB, el valor del driver de referencia.  De reset vale 0, que es
 * silencio. */
#define ES_MIC_VOLUME      200U

/* Escala digital del ADC (registro 0x16) con la que se abre la ruta: 0..7 =
 * 0..+42 dB en pasos de 6, ultimo escalon antes de que la muestra salga por el
 * I2S.  Medido con el altavoz de la placa dando un tono de 1 kHz a full scale,
 * DAC en su techo limpio (vol 75 = 0 dB) y el PGA analogico apagado: 0 -> 0,3 %,
 * 2 -> 1,4 %, 4 -> 5 %, 6 -> 21 %, 7 -> 42 %.  La escalera es de 6 dB justos,
 * asi que el registro se obedece y lo que falta no es ganancia digital, es
 * senal acustica.
 *
 * Con el PGA analogico a tope (10 = +30 dB, el default de este driver) ese mismo
 * tono clava el pico al 100 %; una voz a 20-30 cm esta unos 30 dB por debajo de
 * ese estimulo y con 4 quedaria en un 1..3 % --casi inaudible--, que es lo que se
 * oia.  Medido en sala con 6: el suelo ambiente da un pico estable del 5..6 % y
 * deja toda la mitad alta de la escala para la voz, que es donde queria caer.
 * El codec no se resetea con el P4 y este registro sobrevive al flasheo, asi que
 * sin escribirlo aqui la captura arrancaria con el nivel que dejo la sesion
 * anterior. */
#define ES_MIC_SCALE       6U

/** Un par (MCLK, fs) y los divisores que lo realizan en el codec. */
struct coeff
{
    uint32_t mclk_hz;
    uint32_t sample_rate_hz;
    uint8_t pre_div;
    uint8_t pre_multi;
    uint8_t adc_div;
    uint8_t dac_div;
    uint8_t fs_mode;
    uint8_t lrck_h;
    uint8_t lrck_l;
    uint8_t bclk_div;
    uint8_t adc_osr;
    uint8_t dac_osr;
};

static const struct coeff s_coeff_div[] = {
    /* 8k */
    {12288000, 8000, 0x06, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {18432000, 8000, 0x03, 0x01, 0x03, 0x03, 0x00, 0x05, 0xFF, 0x18, 0x10, 0x10},
    {16384000, 8000, 0x08, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {8192000, 8000, 0x04, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {6144000, 8000, 0x03, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {4096000, 8000, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {3072000, 8000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {2048000, 8000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {1536000, 8000, 0x03, 0x02, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {1024000, 8000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},

    /* 11.025k */
    {11289600, 11025, 0x04, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {5644800, 11025, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {2822400, 11025, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {1411200, 11025, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},

    /* 12k */
    {12288000, 12000, 0x04, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {6144000, 12000, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {3072000, 12000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {1536000, 12000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},

    /* 16k */
    {12288000, 16000, 0x03, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {18432000, 16000, 0x03, 0x01, 0x03, 0x03, 0x00, 0x02, 0xFF, 0x0C, 0x10, 0x10},
    {16384000, 16000, 0x04, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {8192000, 16000, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {6144000, 16000, 0x03, 0x01, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {4096000, 16000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {3072000, 16000, 0x03, 0x02, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {2048000, 16000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {1536000, 16000, 0x03, 0x03, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {1024000, 16000, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},

    /* 22.05k */
    {11289600, 22050, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {5644800, 22050, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {2822400, 22050, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {1411200, 22050, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {705600, 22050, 0x01, 0x03, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},

    /* 24k */
    {12288000, 24000, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {18432000, 24000, 0x03, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {6144000, 24000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {3072000, 24000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {1536000, 24000, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},

    /* 32k */
    {12288000, 32000, 0x03, 0x01, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {18432000, 32000, 0x03, 0x02, 0x03, 0x03, 0x00, 0x02, 0xFF, 0x0C, 0x10, 0x10},
    {16384000, 32000, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {8192000, 32000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {6144000, 32000, 0x03, 0x02, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {4096000, 32000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {3072000, 32000, 0x03, 0x03, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {2048000, 32000, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {1536000, 32000, 0x03, 0x03, 0x01, 0x01, 0x01, 0x00, 0x7F, 0x02, 0x10, 0x10},
    {1024000, 32000, 0x01, 0x03, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},

    /* 44.1k */
    {11289600, 44100, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {5644800, 44100, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {2822400, 44100, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {1411200, 44100, 0x01, 0x03, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},

    /* 48k */
    {12288000, 48000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {18432000, 48000, 0x03, 0x01, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {6144000, 48000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {3072000, 48000, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {1536000, 48000, 0x01, 0x03, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},

    /* 64k */
    {12288000, 64000, 0x03, 0x02, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {18432000, 64000, 0x03, 0x02, 0x03, 0x03, 0x01, 0x01, 0x7F, 0x06, 0x10, 0x10},
    {16384000, 64000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {8192000, 64000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {6144000, 64000, 0x01, 0x02, 0x03, 0x03, 0x01, 0x01, 0x7F, 0x06, 0x10, 0x10},
    {4096000, 64000, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {3072000, 64000, 0x01, 0x03, 0x03, 0x03, 0x01, 0x01, 0x7F, 0x06, 0x10, 0x10},
    {2048000, 64000, 0x01, 0x03, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {1536000, 64000, 0x01, 0x03, 0x01, 0x01, 0x01, 0x00, 0xBF, 0x03, 0x18, 0x18},
    {1024000, 64000, 0x01, 0x03, 0x01, 0x01, 0x01, 0x00, 0x7F, 0x02, 0x10, 0x10},

    /* 88.2k */
    {11289600, 88200, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {5644800, 88200, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {2822400, 88200, 0x01, 0x03, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {1411200, 88200, 0x01, 0x03, 0x01, 0x01, 0x01, 0x00, 0x7F, 0x02, 0x10, 0x10},

    /* 96k */
    {12288000, 96000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {18432000, 96000, 0x03, 0x02, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {6144000, 96000, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {3072000, 96000, 0x01, 0x03, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10},
    {1536000, 96000, 0x01, 0x03, 0x01, 0x01, 0x01, 0x00, 0x7F, 0x02, 0x10, 0x10},
};

#define COEFF_COUNT (sizeof(s_coeff_div) / sizeof(s_coeff_div[0]))

static bool codec;
static bool mic;
static uint8_t mic_pga = ES8311_MIC_PGA_MAX;
static uint8_t mic_vol = ES_MIC_VOLUME;
static uint8_t mic_scale = ES_MIC_SCALE;

static bool reg_write(uint8_t reg, uint8_t value)
{
    struct i2c_req req;

    req.reg = reg;
    req.buf = &value;
    req.len = 1;

    return OK == control(I2C0, I2C_CTRL_WRITE_REG8, ES8311_ADDR0, (long)&req);
}

static bool reg_read(uint8_t reg, uint8_t *value)
{
    struct i2c_req req;

    req.reg = reg;
    req.buf = value;
    req.len = 1;

    return OK == control(I2C0, I2C_CTRL_READ_REGS, ES8311_ADDR0, (long)&req);
}

/** Modificar solo los bits de la mascara, dejando el resto como esta. */
static bool reg_update(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t current;

    if (!reg_read(reg, &current))
    {
        return false;
    }

    current = (uint8_t)((current & ~mask) | (value & mask));

    return reg_write(reg, current);
}

static const struct coeff *coeff_find(uint32_t mclk_hz, uint32_t rate_hz)
{
    unsigned i;

    for (i = 0; i < COEFF_COUNT; i++)
    {
        if (s_coeff_div[i].mclk_hz == mclk_hz &&
            s_coeff_div[i].sample_rate_hz == rate_hz)
        {
            return &s_coeff_div[i];
        }
    }

    return NULL;
}

/**
 * @ingroup audio
 *
 * Pone el codec en el ajuste de ES8311_MCLK_HZ / ES8311_SAMPLE_RATE_HZ:
 * reset de bloques, enable, divisores de reloj, formato de 16 bits y los
 * registros de sistema de la referencia.
 *
 * El reloj que se configura es el que se le pedira al I2S mas adelante.  Sin
 * I2S el codec no tiene MCLK y no suena nada, pero los registros quedan
 * escritos y se pueden leer de vuelta: esa es la prueba de esta etapa.
 *
 * @return true si el codec contesta a todas las transacciones
 */
bool es8311_init(void)
{
    const struct coeff *coeff;
    uint8_t reg06;

    codec = false;

    if (OK != control(I2C0, I2C_CTRL_PROBE, ES8311_ADDR0, 0))
    {
        return false;
    }

    coeff = coeff_find(ES8311_MCLK_HZ, ES8311_SAMPLE_RATE_HZ);
    if (coeff == NULL)
    {
        return false;
    }

    /* Reset: 0x1F reinicia todos los bloques, 0x00 los suelta y 0x80
     * habilita el chip.  Los 20 ms vienen del driver de referencia. */
    if (!reg_write(ES_REG_RESET, 0x1FU))
    {
        return false;
    }
    mdelay(20);
    if (!reg_write(ES_REG_RESET, 0x00U) || !reg_write(ES_REG_RESET, 0x80U))
    {
        return false;
    }

    /* 0x3F = MCLK entrando por el pin MCK, sin invertir. */
    if (!reg_write(ES_REG_CLK01, 0x3FU))
    {
        return false;
    }

    if (!reg_update(ES_REG_CLK02, 0xF8U,
                    (uint8_t)(((coeff->pre_div - 1U) << 5) |
                              (coeff->pre_multi << 3))) ||
        !reg_write(ES_REG_CLK03, (uint8_t)((coeff->fs_mode << 6) |
                                           coeff->adc_osr)) ||
        !reg_write(ES_REG_CLK04, coeff->dac_osr) ||
        !reg_write(ES_REG_CLK05, (uint8_t)(((coeff->adc_div - 1U) << 4) |
                                           coeff->dac_div)))
    {
        return false;
    }

    if (!reg_read(ES_REG_CLK06, &reg06))
    {
        return false;
    }
    /* SCLK sin invertir (bit 5 a 0, como en la referencia) y el divisor de
     * BCLK en los bits 4:0; arriba quedan los bits que no se tocan. */
    reg06 = (uint8_t)(reg06 & ~(ES_BIT(5) | 0x1FU));
    reg06 |= (coeff->bclk_div < 19U) ? (uint8_t)(coeff->bclk_div - 1U)
                                     : coeff->bclk_div;

    if (!reg_write(ES_REG_CLK06, reg06) ||
        !reg_update(ES_REG_CLK07, 0x3FU, coeff->lrck_h) ||
        !reg_write(ES_REG_CLK08, coeff->lrck_l))
    {
        return false;
    }

    /* Modo serie esclavo (bit 6 de 0x00 a 0) y 16 bits en cada lado. */
    if (!reg_update(ES_REG_RESET, 0x40U, 0x00U) ||
        !reg_write(ES_REG_SDPIN, ES_FORMAT_16BIT) ||
        !reg_write(ES_REG_SDPOUT, ES_FORMAT_16BIT))
    {
        return false;
    }

    if (!reg_write(ES_REG_SYS0D, 0x01U) ||
        !reg_write(ES_REG_SYS0E, 0x02U) ||
        !reg_write(ES_REG_SYS12, 0x00U) ||
        !reg_write(ES_REG_SYS13, 0x10U) ||
        !reg_write(ES_REG_ADC1C, 0x6AU) ||
        !reg_write(ES_REG_DAC37, 0x08U))
    {
        return false;
    }

    codec = true;
    return true;
}

bool es8311_present(void)
{
    return codec;
}

/**
 * @ingroup audio
 *
 * Volumen de salida.  El registro va de 0 a 255 y 0 es silencio absoluto, no
 * el escalon mas bajo.
 *
 * Ojo a lo que vale un porcentaje: el registro es lineal en decibelios, no en
 * amplitud (ver es8311_get_volume_db), asi que el 60 % no es "el 60 % del
 * volumen maximo" sino -19,5 dB.  La escala del SDK se mantiene tal cual para
 * no cambiar lo que ya esta validado por consola.
 *
 * @param percent 0..100
 * @return el porcentaje que el codec tiene despues de escribir, o SYSERR
 */
int es8311_set_volume(uint32_t percent)
{
    uint8_t reg32 = 0;

    if (!codec)
    {
        return SYSERR;
    }

    if (percent > 100U)
    {
        percent = 100U;
    }

    if (percent > 0U)
    {
        reg32 = (uint8_t)(((percent * 256U) / 100U) - 1U);
    }

    if (!reg_write(ES_REG_DAC32, reg32))
    {
        return SYSERR;
    }

    return es8311_get_volume();
}

/**
 * @ingroup audio
 *
 * El escalon de 0 dB esta en 191 (0xBF del datasheet) y cada paso del registro
 * mueve medio decibelio, de -95,5 dB en 0 a +32 dB en 255.  Se devuelven decimas
 * para no necesitar coma flotante: 191 -> 0, 152 -> -195 (-19,5 dB).
 *
 * @return decimas de dB leidas del codec, o SYSERR si no responde
 */
int es8311_get_volume_db(void)
{
    uint8_t reg32;

    if (!codec || !reg_read(ES_REG_DAC32, &reg32))
    {
        return SYSERR;
    }

    if (reg32 == 0U)
    {
        return ES8311_VOL_MUTE_DB10;
    }

    return ((int32_t)reg32 - (int32_t)ES8311_VOL_0DB_REG) * ES8311_VOL_STEP_DB10;
}

/** Volumen actual leido del propio codec, sin cache del driver. */
int es8311_get_volume(void)
{
    uint8_t reg32;

    if (!codec || !reg_read(ES_REG_DAC32, &reg32))
    {
        return SYSERR;
    }

    if (reg32 == 0U)
    {
        return 0;
    }

    return (int)((reg32 * 100U) / 256U) + 1;
}

/** Mute de la salida DAC: bits 6 y 5 de 0x31. */
bool es8311_set_mute(bool mute)
{
    uint8_t value = mute ? (uint8_t)(ES_BIT(6) | ES_BIT(5)) : 0U;

    return codec && reg_update(ES_REG_DAC31, ES_BIT(6) | ES_BIT(5), value);
}

/** Estado del mute leido del codec: 1 muteado, 0 activo, SYSERR sin respuesta. */
int es8311_get_mute(void)
{
    uint8_t reg31;

    if (!codec || !reg_read(ES_REG_DAC31, &reg31))
    {
        return SYSERR;
    }

    return (reg31 & (ES_BIT(6) | ES_BIT(5))) != 0;
}

/** Escala digital del ADC: 0..7 = 0..42 dB en pasos de 6 (registro 0x16). */
bool es8311_set_mic_gain(uint32_t gain)
{
    if (!codec || gain > ES8311_MIC_GAIN_MAX)
    {
        return false;
    }

    mic_scale = (uint8_t)gain;
    return reg_write(ES_REG_ADC16, mic_scale);
}

/** Escala digital del ADC leida del registro 0x16. */
int es8311_get_mic_scale(void)
{
    uint8_t reg16;

    if (!codec || !reg_read(ES_REG_ADC16, &reg16))
    {
        return SYSERR;
    }

    return reg16;
}

/**
 * @ingroup audio
 *
 * Abrir la ruta de captura.  Los cuatro pasos son los del driver de referencia
 * (drivers/es8311/es8311.c, es8311_configure_microphone) leidos contra el
 * datasheet, mas el 0x16:
 *
 *  - 0x0E bit6 (PDN_PGA) a 0: sin esto el PGA esta apagado de fabrica y la
 *    entrada analogica no llega a ningun sitio.  es8311_init ya lo deja a 0,
 *    se vuelve a escribir por si alguien parlo microfono antes.
 *  - 0x14 = 0x10 | PGA: LINSEL[5:4] a 1 selecciona la pareja
 *    MIC1P/MIC1N como entrada diferencial --es la unica que tiene el chip--,
 *    y PGAGAIN[3:0] es la ganancia analogica, de 0 a +30 dB en pasos de 3.
 *    DMIC_ON (bit6) queda a 0: la placa lleva microfono analogico.
 *  - 0x16 = escala digital del ADC (es8311_set_mic_gain), de 0 a +42 dB en
 *    pasos de 6.  Se escribe aqui, y no se deja el valor de fabrica, por lo que
 *    se midio en placa: es el mando que mas nivel mueve de la cadena (a +0 dB
 *    una toma de referencia sale al 11 % de escala y a +24 dB recorta), y como
 *    el codec no sigue el reset del P4, heredaria el ultimo valor que le puso
 *    la sesion anterior.
 *  - 0x17 = volumen digital del ADC.  Es lineal en decibelios igual que el del
 *    DAC (0 = -95,5 dB, 191 = 0 dB, 0,5 dB/paso) y vale 0 de reset, o sea
 *    silencio: sin escribirlo el microfono suena por fuera pero no entra
 *    ninguna muestra.
 *
 * @return true si el codec acepto las cuatro escrituras
 */
bool es8311_start_mic(void)
{
    if (!codec)
    {
        return false;
    }

    if (!reg_update(ES_REG_SYS0E, ES_BIT(6), 0U) ||
        !reg_write(ES_REG_SYS14, (uint8_t)(ES_BIT(4) | mic_pga)) ||
        !reg_write(ES_REG_ADC16, mic_scale) ||
        !reg_write(ES_REG_ADC17, mic_vol))
    {
        return false;
    }

    mic = true;
    return true;
}

/**
 * @ingroup audio
 *
 * Cerrar la ruta: el ADC vuelve a su silencio de reset y el PGA se apaga, que
 * es como estaba despues del reset general del chip.  El 0x14 se deja sin
 * seleccion de entrada (LINSEL a 0).
 *
 * @return true si el codec acepto las escrituras
 */
bool es8311_stop_mic(void)
{
    if (!codec)
    {
        return false;
    }

    if (!reg_write(ES_REG_ADC17, 0U) ||
        !reg_write(ES_REG_SYS14, 0U) ||
        !reg_update(ES_REG_SYS0E, ES_BIT(6), ES_BIT(6)))
    {
        return false;
    }

    mic = false;
    return true;
}

bool es8311_mic_running(void)
{
    return mic;
}

/**
 * @ingroup audio
 *
 * Ganancia analogica del PGA.  Se escribe solo PGAGAIN[3:0], porque en los
 * bits altos esta la seleccion de entrada que puso es8311_start_mic.
 */
bool es8311_set_mic_pga(uint32_t gain)
{
    if (!codec || gain > ES8311_MIC_PGA_MAX)
    {
        return false;
    }

    mic_pga = (uint8_t)gain;
    return reg_update(ES_REG_SYS14, 0x0FU, (uint8_t)gain);
}

/** PGA leido del propio registro (no del valor pedido). */
int es8311_get_mic_pga(void)
{
    uint8_t reg14;

    if (!codec || !reg_read(ES_REG_SYS14, &reg14))
    {
        return SYSERR;
    }

    return reg14 & 0x0FU;
}

/** Volumen digital del ADC leido del registro 0x17. */
int es8311_get_mic_volume(void)
{
    uint8_t reg17;

    if (!codec || !reg_read(ES_REG_ADC17, &reg17))
    {
        return SYSERR;
    }

    return reg17;
}

/**
 * @ingroup audio
 *
 * Los tres registros de la ruta leidos de golpe, en la unica forma que
 * interesa para diagnosticar: si falta un bit, la captura no falla por el
 * DMA ni por el I2S, falla aqui.
 *
 * @return bits 0 (PGA encendido), 1 (entrada seleccionada) y 2 (ADC con
 *         volumen), o SYSERR si el codec no contesta
 */
int es8311_mic_status(void)
{
    uint8_t reg0e, reg14, reg17;
    int st = 0;

    if (!codec || !reg_read(ES_REG_SYS0E, &reg0e) ||
        !reg_read(ES_REG_SYS14, &reg14) || !reg_read(ES_REG_ADC17, &reg17))
    {
        return SYSERR;
    }

    if ((reg0e & ES_BIT(6)) == 0U)
    {
        st |= 0x1;
    }
    if ((reg14 & 0x30U) != 0U)
    {
        st |= 0x2;
    }
    if (reg17 != 0U)
    {
        st |= 0x4;
    }
    return st;
}
