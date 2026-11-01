/**
 * @file i2s.c
 *
 * Relojes I2S0 (MCLK/BCLK/LRCK) en modo maestro, al nivel de registro: el
 * acceso es por offsets crudos como en device/i2c, sin headers de Espressif.
 *
 * En el ESP32-P4 el bloque I2S no contiene ningun registro de reloj: la
 * fuente y el divisor fraccionario viven en HP_SYS_CLKRST, y la salida de
 * MCLK por el pad se habilita en LP_AON_CLKRST.  Todos los campos son de
 * solo lectura-modificacion-escritura porque esos registros son compartidos
 * con I2S1/I2S2 y con el resto del arbol de clocks.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include "i2s.h"
#include "board.h"
#include "esp32p4.h"
#include "gpio.h"
#include "clock.h"
#include "platform.h"

/* --- I2S0 register offsets (SVD hw_ver1) ------------------------- */
#define I2S_RX_CONF          0x20U
#define I2S_TX_CONF          0x24U
#define I2S_RX_CONF1         0x28U
#define I2S_TX_CONF1         0x2CU
#define I2S_TX_PCM2PDM_CONF  0x40U
#define I2S_RX_TDM_CTRL      0x50U
#define I2S_TX_TDM_CTRL      0x54U
#define I2S_LC_HUNG_CONF     0x60U
#define I2S_RXEOF_NUM        0x64U
#define I2S_CONF_SINGLE_DATA 0x68U
#define I2S_STATE            0x6CU
#define I2S_FIFO_CNT         0x74U
#define I2S_BCK_CNT          0x78U

#define I2S0_REGP(off)    ((volatile uint32_t *)(I2S0_BASE + (off)))
#define I2S0_REG(off)     (*I2S0_REGP(off))

/* TX_CONF */
#define TX_RESET            (1U << 0)
#define TX_FIFO_RESET       (1U << 1)
#define TX_START            (1U << 2)
#define TX_SLAVE_MOD        (1U << 3)
#define TX_STOP_EN          (1U << 4)
#define TX_UPDATE           (1U << 8)
#define TX_MONO_FST_VLD     (1U << 9)
#define TX_MSB_SHIFT        (1U << 13)
#define TX_BCK_NO_DLY       (1U << 14)
#define TX_PCM_BYPASS       (1U << 12)
#define TX_TDM_EN           (1U << 19)
#define TX_BCK_DIV_MASK     (0x3FU << 21)
#define TX_BCK_DIV_SHIFT    21U

/* TX_CONF1: ws_width[8:0] bits_mod[18:14] half_sample[26:19] chan_bits[31:27] */
#define WS_WIDTH_MASK       0x1FFU
#define WS_WIDTH_SHIFT      0U
#define BITS_MOD_MASK       (0x1FU << 14)
#define BITS_MOD_SHIFT      14U
#define HALF_SAMPLE_MASK    (0xFFU << 19)
#define HALF_SAMPLE_SHIFT   19U
#define CHAN_BITS_MASK      (0x1FU << 27)
#define CHAN_BITS_SHIFT     27U

/* TX_TDM_CTRL: chan_en[15:0] tot_chan_num[19:16] skip_msk[20] */
#define TDM_CHAN_EN_MASK    0xFFFFU
#define TDM_CHAN_EN_SHIFT   0U
#define TDM_TOT_CHAN_MASK   (0xFU << 16)
#define TDM_TOT_CHAN_SHIFT  16U

/* RX_CONF: el mismo mapa que TX_CONF, pero con los valores de reset
 * distintos (rx_msb_shift, rx_left_align, rx_pcm_bypass y rx_mono_fst_vld
 * vienen de fabrica en 1).  Se escribe el registro entero, como en TX. */
#define RX_RESET            (1U << 0)
#define RX_FIFO_RESET       (1U << 1)
#define RX_START            (1U << 2)
#define RX_SLAVE_MOD        (1U << 3)
#define RX_UPDATE           (1U << 8)
#define RX_MONO_FST_VLD     (1U << 9)
#define RX_PCM_BYPASS       (1U << 12)
#define RX_MSB_SHIFT        (1U << 13)
#define RX_TDM_EN           (1U << 19)
#define RX_BCK_DIV_MASK     (0x3FU << 21)
#define RX_BCK_DIV_SHIFT    21U

/* TX_CONF bit30: en modo duplex compartido el BCLK y el LRCK del TX se
 * reenvian por dentro al RX.  Sin este bit el RX, que es esclavo, no ve
 * relojes. (i2s_ll_share_bck_ws del SDK) */
#define TX_SIG_LOOPBACK     (1U << 30)

/* STATE / FIFO_CNT / BCK_CNT */
#define TX_IDLE             (1U << 0)
#define TX_FIFO_CNT_MASK    0x7FFFFFFFU
#define TX_FIFO_CNT_RST     (1U << 31)
#define BCK_CNT_MASK        0x7FFFFFFFU
#define BCK_CNT_RST         (1U << 31)

/* LC_HUNG_CONF: fifo_timeout[7:0] shift[10:8] timeout_ena[11] */
#define LC_FIFO_TIMEOUT_ENA (1U << 11)

/* --- HP_SYS_CLKRST + LP_AON_CLKRST -------------------------------- */
#define CLKRST_SOC_CLK_CTRL2   0x1CU
#define CLKRST_PERI_CLK_CTRL11 0x44U
#define CLKRST_PERI_CLK_CTRL12 0x48U
#define CLKRST_PERI_CLK_CTRL13 0x4CU
#define CLKRST_PERI_CLK_CTRL14 0x50U
#define LPCLK_HP_CLK_CTRL      0x40U    /* LP_AON_CLKRST + 0x40       */

#define CLKRST_REG(off)     (*(volatile uint32_t *)(HP_SYS_CLKRST_BASE + (off)))
#define LPCLK_REG(off)      (*(volatile uint32_t *)(LP_AON_CLKRST_BASE + (off)))

#define C2_I2S0_APB_CLK_EN  (1U << 14)
#define LP_PAD_I2S0_MCLK_EN (1U << 12)

/* PERI_CLK_CTRL11: ademas del reloj funcional RX (bit 24) trae el divisor de
 * I2C1 en los bits bajos, asi que aqui solo se tocan el bit y la fuente del
 * RX (rx_clk_src_sel[26:25]); sin fuente el RX no recibe reloj de 40 MHz. */
#define C11_RX_CLK_EN        (1U << 24)
#define C11_RX_SRC_SEL_MASK  (0x3U << 25)
#define C11_RX_SRC_SEL_SHIFT 25U

/* PERI_CLK_CTRL12: rx_div_n[7:0] rx_div_x[16:8] rx_div_y[25:17].  La cadena
 * RX tiene su propio divisor fraccionario, exactamente igual que la TX pero
 * repartido en otros sitios: N/X/Y aqui y Z/YN1 en CTRL13. */
#define C12_RX_DIV_N_MASK   0xFFU
#define C12_RX_DIV_N_SHIFT  0U
#define C12_RX_DIV_X_MASK   (0x1FFU << 8)
#define C12_RX_DIV_X_SHIFT  8U
#define C12_RX_DIV_Y_MASK   (0x1FFU << 17)
#define C12_RX_DIV_Y_SHIFT  17U

/* PERI_CLK_CTRL13: rx_div_z[8:0] rx_div_yn1[9] tx_clk_en[10]
 * tx_clk_src_sel[12:11] tx_div_n[20:13] tx_div_x[29:21] */
#define C13_TX_CLK_EN       (1U << 10)
#define C13_SRC_SEL_MASK    (0x3U << 11)
#define C13_SRC_SEL_SHIFT   11U
#define C13_DIV_N_MASK      (0xFFU << 13)
#define C13_DIV_N_SHIFT     13U
#define C13_DIV_X_MASK      (0x1FFU << 21)
#define C13_DIV_X_SHIFT     21U
#define C13_RX_DIV_Z_MASK   0x1FFU
#define C13_RX_DIV_Z_SHIFT  0U
#define C13_RX_DIV_YN1      (1U << 9)

/* PERI_CLK_CTRL14: tx_div_y[8:0] tx_div_z[17:9] tx_div_yn1[18]
 * mst_clk_sel[19] */
#define C14_DIV_Y_MASK      0x1FFU
#define C14_DIV_Y_SHIFT     0U
#define C14_DIV_Z_MASK      (0x1FFU << 9)
#define C14_DIV_Z_SHIFT     9U
#define C14_DIV_YN1         (1U << 18)
#define C14_MST_CLK_SEL_TX  (1U << 19)

#define CLK_SRC_XTAL        0U      /* 0 XTAL, 1 APLL, 2 externo, 3 solo rev3 */
#define I2S_SRC_HZ          P4_XTAL_FREQ

/* Indices de la matriz GPIO de I2S0 (soc/gpio_sig_map.h).  O_SD e I_SD
 * comparten indice: el pad lo decide la direccion del pad. */
#define I2S0_SIG_BCK        25U
#define I2S0_SIG_MCLK       26U
#define I2S0_SIG_WS         27U
#define I2S0_SIG_SD         28U

/* El tx_update es un handshake con el dominio de reloj CDC: se sondea con
 * un tope para no colgar el kernel si el reloj funcional no esta. */
#define TX_SYNC_GUARD       200000U

static bool started;
static bool rxStarted;

static void field_write(volatile uint32_t *reg, uint32_t mask,
                        uint32_t shift, uint32_t value)
{
    *reg = (*reg & ~mask) | ((value << shift) & mask);
}

static uint32_t gcd(uint32_t a, uint32_t b)
{
    while (b != 0U)
    {
        uint32_t t = a % b;

        a = b;
        b = t;
    }
    return a;
}

/**
 * @ingroup audio
 *
 * Coeficientes del divisor fraccionario de MCLK: MCLK = SRC / (N + b/a).
 * Se reduce la razon SRC/MCLK por su mcd, asi que solo hay dos salidas: o la
 * fraccion cabe en los registros (a y b de 9 bits) y la frecuencia es exacta,
 * o la funcion falla.  No produce nunca un reloj aproximado sin avisar.
 */
static bool mclk_divider(uint32_t *div_n, uint32_t *x, uint32_t *y,
                         uint32_t *z, bool *yn1)
{
    uint32_t g = gcd(I2S_SRC_HZ, ES8311_MCLK_HZ);
    uint32_t num = I2S_SRC_HZ / g;             /* razon = num/den, > 1 */
    uint32_t den = ES8311_MCLK_HZ / g;
    uint32_t n = num / den;
    uint32_t b = num % den;                    /* resto: la parte fracciona */
    uint32_t a = den;

    if (n < 2U || n > 255U || (b != 0U && a > 511U))
    {
        return false;
    }

    /* Codificacion del hardware, igual que la capa baja del SDK. */
    *yn1 = (b * 2U > a);
    *z = *yn1 ? a - b : b;
    *x = b == 0U ? 0U : a / *z - 1U;
    *y = b == 0U ? 0U : a % *z;
    *div_n = n;
    return true;
}

/**
 * @ingroup audio
 *
 * Divisor de BCLK: MCLK/BCLK debe ser entero y estar en [2,64] (el campo
 * tx_bck_div_num tiene 6 bits y guarda el valor - 1).
 */
static bool bclk_divider(uint32_t *div)
{
    if (ES8311_MCLK_HZ % I2S_BCLK_HZ != 0U)
    {
        return false;
    }
    *div = ES8311_MCLK_HZ / I2S_BCLK_HZ;
    return *div >= 2U && *div <= 64U;
}

static bool tx_sync(void)
{
    volatile uint32_t *r = I2S0_REGP(I2S_TX_CONF);
    uint32_t guard = TX_SYNC_GUARD;

    *r |= TX_UPDATE;
    while ((*r & TX_UPDATE) != 0U)
    {
        if (--guard == 0U)
        {
            return false;
        }
    }
    return true;
}

static void pad_to_clock(uint8_t pin, uint32_t sig)
{
    /* El pad pasa a la matriz GPIO y saca la senal del periferico; con
     * OEN_SEL a 0 la salida la goberna el propio I2S, como en el SPI del
     * LCD. */
    gpio_set_dir(pin, GPIO_OUTPUT);
    gpio_matrix_out(pin, sig, false, false);
}

/**
 * @ingroup audio
 *
 * Arrancar los relojes TX en maestro: 48 kHz, 16 bits por slot, dos slots,
 * MCLK de 12,288 MHz por el pad.  No toca el codec ni el amplificador.
 *
 * @return OK si los relojes quedaron corriendo, FALSE en caso contrario
 */
bool i2s_clocks_start(void)
{
    uint32_t div_n, x, y, z, bdiv;
    bool yn1;

    if (started)
    {
        return true;
    }
    if (!mclk_divider(&div_n, &x, &y, &z, &yn1) || !bclk_divider(&bdiv))
    {
        return false;
    }

    /* 1. Apertura de relojes: el de registros (APB), el funcional TX y la
     *    salida de MCLK por el pad. */
    CLKRST_REG(CLKRST_SOC_CLK_CTRL2) |= C2_I2S0_APB_CLK_EN;
    LPCLK_REG(LPCLK_HP_CLK_CTRL) |= LP_PAD_I2S0_MCLK_EN;
    CLKRST_REG(CLKRST_PERI_CLK_CTRL13) |= C13_TX_CLK_EN;

    /* 2. Fuente XTAL de 40 MHz y MCLK ligado al dominio TX (bit propio del
     *    P4: 1 = TX, 0 = RX). */
    field_write(&CLKRST_REG(CLKRST_PERI_CLK_CTRL13), C13_SRC_SEL_MASK,
                C13_SRC_SEL_SHIFT, CLK_SRC_XTAL);
    CLKRST_REG(CLKRST_PERI_CLK_CTRL14) |= C14_MST_CLK_SEL_TX;

    /* 3. Divisor fraccionario.  El hardware exige una secuencia: primero la
     *    division mas corta (N=2, fraccion nula) y solo entonces la objetivo,
     *    por el errata de la doble division. */
    field_write(&CLKRST_REG(CLKRST_PERI_CLK_CTRL13), C13_DIV_N_MASK,
                C13_DIV_N_SHIFT, 2U);
    CLKRST_REG(CLKRST_PERI_CLK_CTRL14) &= ~C14_DIV_YN1;
    field_write(&CLKRST_REG(CLKRST_PERI_CLK_CTRL14), C14_DIV_Y_MASK,
                C14_DIV_Y_SHIFT, 1U);
    field_write(&CLKRST_REG(CLKRST_PERI_CLK_CTRL14), C14_DIV_Z_MASK,
                C14_DIV_Z_SHIFT, 0U);
    field_write(&CLKRST_REG(CLKRST_PERI_CLK_CTRL13), C13_DIV_X_MASK,
                C13_DIV_X_SHIFT, 0U);

    if (yn1)
    {
        CLKRST_REG(CLKRST_PERI_CLK_CTRL14) |= C14_DIV_YN1;
    }
    else
    {
        CLKRST_REG(CLKRST_PERI_CLK_CTRL14) &= ~C14_DIV_YN1;
    }
    field_write(&CLKRST_REG(CLKRST_PERI_CLK_CTRL14), C14_DIV_Z_MASK,
                C14_DIV_Z_SHIFT, z);
    field_write(&CLKRST_REG(CLKRST_PERI_CLK_CTRL14), C14_DIV_Y_MASK,
                C14_DIV_Y_SHIFT, y);
    field_write(&CLKRST_REG(CLKRST_PERI_CLK_CTRL13), C13_DIV_X_MASK,
                C13_DIV_X_SHIFT, x);
    field_write(&CLKRST_REG(CLKRST_PERI_CLK_CTRL13), C13_DIV_N_MASK,
                C13_DIV_N_SHIFT, div_n);

    /* 4. Timing del TX: maestro (tx_slave_mod a 0) e I2S estandard.  En el
     *    P4 el estandar lo genera el motor TDM con dos slots, y el LRCK sale
     *    con un BCLK de retraso sobre el dato (tx_msb_shift).  El resto de
     *    campos queda deliberadamente a 0: sin tx_stop_en el FSM sigue dando
     *    reloj aunque el FIFO se vacie, y saca el valor constante de
     *    conf_single_data (a cero mas abajo).  Con el DMA detras, un hueco del
     *    motor se oye como silencio en lugar de tumbar el MCLK del codec, que
     *    es lo que el DAC agradece. */
    I2S0_REG(I2S_TX_CONF) = TX_MSB_SHIFT | TX_BCK_NO_DLY | TX_MONO_FST_VLD |
                            TX_TDM_EN | TX_PCM_BYPASS |
                            ((bdiv - 1U) << TX_BCK_DIV_SHIFT);

    /* El vigilante del FIFO viene activado de fabrica: si se queda sin
     * datos salta tx_hung y el FSM TX se puede quedar plantado.  Con un anillo
     * en bucle cerrado no hay nada que rescatar de un hueco, asi que el
     * contador se apaga y el reloj sigue corriendo. */
    I2S0_REG(I2S_LC_HUNG_CONF) &= ~LC_FIFO_TIMEOUT_ENA;

    field_write(I2S0_REGP(I2S_TX_CONF1), WS_WIDTH_MASK, WS_WIDTH_SHIFT,
                I2S_FRAME_BITS - 1U);
    field_write(I2S0_REGP(I2S_TX_CONF1), BITS_MOD_MASK, BITS_MOD_SHIFT,
                I2S_SLOT_BITS - 1U);
    field_write(I2S0_REGP(I2S_TX_CONF1), HALF_SAMPLE_MASK, HALF_SAMPLE_SHIFT,
                I2S_FRAME_BITS - 1U);
    field_write(I2S0_REGP(I2S_TX_CONF1), CHAN_BITS_MASK, CHAN_BITS_SHIFT,
                I2S_SLOT_BITS - 1U);

    field_write(I2S0_REGP(I2S_TX_TDM_CTRL), TDM_CHAN_EN_MASK,
                TDM_CHAN_EN_SHIFT, 0x3U);      /* los dos slots activos */
    field_write(I2S0_REGP(I2S_TX_TDM_CTRL), TDM_TOT_CHAN_MASK,
                TDM_TOT_CHAN_SHIFT, I2S_CHANNELS - 1U);

    I2S0_REG(I2S_TX_PCM2PDM_CONF) = 0U;        /* nada de PCM a PDM */
    I2S0_REG(I2S_CONF_SINGLE_DATA) = 0U;

    /* 5. Pads: MCLK, BCLK, LRCK y el dato de salida hacia el DAC del codec. */
    pad_to_clock(I2S_MCK, I2S0_SIG_MCLK);
    pad_to_clock(I2S_BCK, I2S0_SIG_BCK);
    pad_to_clock(I2S_WS, I2S0_SIG_WS);
    pad_to_clock(I2S_DOUT, I2S0_SIG_SD);

    /* 6. Arranque. */
    I2S0_REG(I2S_TX_CONF) |= TX_FIFO_RESET;
    I2S0_REG(I2S_TX_CONF) &= ~TX_FIFO_RESET;
    if (!tx_sync())
    {
        (void)i2s_clocks_stop();
        return false;
    }
    I2S0_REG(I2S_TX_CONF) |= TX_START;
    started = true;
    return true;
}

/**
 * @ingroup audio
 *
 * Parar los relojes y cerrar las compuertas que se abrieron arriba.
 *
 * @return TRUE
 */
bool i2s_clocks_stop(void)
{
    I2S0_REG(I2S_TX_CONF) &= ~TX_START;
    I2S0_REG(I2S_TX_CONF) |= TX_RESET;
    I2S0_REG(I2S_TX_CONF) &= ~TX_RESET;
    LPCLK_REG(LPCLK_HP_CLK_CTRL) &= ~LP_PAD_I2S0_MCLK_EN;
    CLKRST_REG(CLKRST_PERI_CLK_CTRL13) &= ~C13_TX_CLK_EN;
    started = false;
    return true;
}

/**
 * @ingroup audio
 *
 * @return TRUE si el FSM TX esta trabajando (tx_idle a 0 y tx_start a 1).
 */
bool i2s_clocks_running(void)
{
    if ((I2S0_REG(I2S_TX_CONF) & TX_START) == 0U)
    {
        return false;
    }
    return (I2S0_REG(I2S_STATE) & TX_IDLE) == 0U;
}

/**
 * @ingroup audio
 *
 * Medida real de BCLK con el contador del periferico (tx_bck_cnt): se pone a
 * cero, se espera @p us microsegundos y se divide entre el tiempo medido por
 * el reloj del kernel.  Es la prueba de que el divisor fraccionario da la
 * frecuencia pedida sin necesitar un osciloscopio.
 *
 * @param us duracion de la ventana de medida
 * @return BCLK en Hz, 0 si no corre
 */
uint32_t i2s_bclk_measure(uint32_t us)
{
    uint64_t ticks, cnt;
    uint32_t t0, t1;

    if (!started || us == 0U || us > 200000U)
    {
        return 0U;
    }

    I2S0_REG(I2S_BCK_CNT) = BCK_CNT_RST;
    I2S0_REG(I2S_BCK_CNT) = 0U;

    t0 = clkcount();
    udelay(us);
    t1 = clkcount();
    cnt = I2S0_REG(I2S_BCK_CNT) & BCK_CNT_MASK;

    ticks = (uint32_t) (t1 - t0);
    if (ticks == 0U || cnt == 0U)
    {
        return 0U;
    }
    return (uint32_t) ((cnt * platform.clkfreq) / ticks);
}

/**
 * @ingroup audio
 *
 * Contador crudo de BCLK.  No se reinicia aqui: quien lo use para medir ritmos
 * toma dos lecturas seguidas y reste, con lo que el desborde de los 31 bits no
 * importa mientras el intervalo medido dure menos de 1.39 s.
 *
 * @return cuenta de BCLK desde el ultimo reset
 */
uint32_t i2s_bck_count(void)
{
    return I2S0_REG(I2S_BCK_CNT) & BCK_CNT_MASK;
}

/** Poner a cero el contador de BCLK (bit de pulso, como el de sincronizacion). */
void i2s_bck_reset(void)
{
    I2S0_REG(I2S_BCK_CNT) = BCK_CNT_RST;
    I2S0_REG(I2S_BCK_CNT) = 0U;
}

/**
 * @ingroup audio
 *
 * @return TRUE si el mando TX esta arrancado (tx_start a 1).
 */
bool i2s_tx_started(void)
{
    return (I2S0_REG(I2S_TX_CONF) & TX_START) != 0U;
}

/**
 * @ingroup audio
 *
 * @return TRUE si el FSM TX esta parado (tx_idle a 1): con tx_start puesto e
 *         idle a 1 el bloque esta hambriento de datos.
 */
bool i2s_tx_idle(void)
{
    return (I2S0_REG(I2S_STATE) & TX_IDLE) != 0U;
}

/**
 * @ingroup audio
 *
 * Contador de sincronizacion del FIFO de TX.  No es un nivel: en el P4 el FIFO
 * esta detras del DMA y no tiene ningun registro de llenado, y este numero
 * cuenta lo que el bloque ha sacado desde el ultimo reset.  Vale como prueba de
 * flujo — si el DMA entrega, crece; si se queda quieto, el TX esta hambriento —
 * y es exactamente lo que usa el SDK para detectar el cuelgue.
 *
 * @return cuenta del bloque, modulo 2^31
 */
uint32_t i2s_tx_sync_count(void)
{
    return I2S0_REG(I2S_FIFO_CNT) & TX_FIFO_CNT_MASK;
}

/**
 * @ingroup audio
 *
 * Poner a cero el contador: el bit de reset es de un pulso (WT), asi que se
 * escribe y se quita como hace i2s_ll_tx_reset_fifo_sync_counter() del SDK.
 */
void i2s_tx_sync_reset(void)
{
    I2S0_REG(I2S_FIFO_CNT) = TX_FIFO_CNT_RST;
    I2S0_REG(I2S_FIFO_CNT) = 0U;
}

/**
 * @ingroup audio
 *
 * Handshake de actualizacion del RX, igual que el del TX: el mando pasa al
 * dominio de reloj I2S y el bit se limpia solo.  Se sondea con tope por el
 * mismo motivo que arriba.
 */
static bool rx_sync(void)
{
    volatile uint32_t *r = I2S0_REGP(I2S_RX_CONF);
    uint32_t guard = TX_SYNC_GUARD;

    *r |= RX_UPDATE;
    while ((*r & RX_UPDATE) != 0U)
    {
        if (--guard == 0U)
        {
            return false;
        }
    }
    return true;
}

/**
 * @ingroup audio
 *
 * Montar el RX del mismo bloque I2S0 para capturar al microfono.
 *
 * El P4 no tiene reloj de RX propio que valga aqui: en duplex compartido los
 * BCLK/LRCK del TX se reenvian por dentro al RX (sig_loopback) y el MCLK esta
 * ligado al dominio TX, asi que el RX se pone obligatoriamente en modo
 * esclavo y quien manda el timing sigue siendo el TX.  Es la unica forma de
 * capturar sin pelea de pads: los pads de reloj siguen con el TX y aqui solo
 * entra el pad de datos (DIN, GPIO11).  El orden es el de i2s_std.c del SDK:
 * compartir relojes, configurar el slot, y arrancar.
 *
 * Requiere y deja arrancados los relojes TX: sin ellos no hay MCLK para el
 * codec ni BCLK/LRCK para el RX, y el DMA de salida puede estar parado
 * mientras se captura (el TX emite entonces el valor constante de
 * conf_single_data, o sea silencio).
 *
 * @return true si el RX quedo capturando
 */
bool i2s_rx_start(void)
{
    uint32_t bdiv;
    uint32_t div_n, x, y, z;
    bool yn1;

    if (rxStarted)
    {
        return true;
    }
    if (!bclk_divider(&bdiv))
    {
        return false;
    }
    if (!mclk_divider(&div_n, &x, &y, &z, &yn1))
    {
        return false;
    }
    if (!i2s_clocks_start())
    {
        return false;
    }

    /* 1. Reloj funcional del RX y su divisor.  El RX del P4 tiene una cadena
     *    de reloj propia (CTRL11 fuente, CTRL12 N/X/Y, CTRL13 Z/YN1) separada
     *    de la del TX: arrancar solo el bit de enable deja el modulo alimentado
     *    con un reloj derivado de unos divisores que valen 0, y el RX tragaba
     *    8 kB/s en vez de los 192 kB/s del cable.  El SDK programa la cadena
     *    RX siempre (i2s_hal_set_rx_clock) con los mismos coeficientes del
     *    MCLK, y aqui tambien: la unica diferencia es que el pad de MCLK sigue
     *    ligado al TX (mst_clk_sel a 1), porque el ES8311 se alimenta de el.
     *    Se escribe con field_write para no mover los bits del TX que comparten
     *    CTRL11 y CTRL13. */
    CLKRST_REG(CLKRST_PERI_CLK_CTRL11) |= C11_RX_CLK_EN;
    field_write(&CLKRST_REG(CLKRST_PERI_CLK_CTRL11), C11_RX_SRC_SEL_MASK,
                C11_RX_SRC_SEL_SHIFT, CLK_SRC_XTAL);

    /* La misma errata de la doble division: primero la division mas corta y
     * despues la objetivo. */
    field_write(&CLKRST_REG(CLKRST_PERI_CLK_CTRL12), C12_RX_DIV_N_MASK,
                C12_RX_DIV_N_SHIFT, 2U);
    CLKRST_REG(CLKRST_PERI_CLK_CTRL13) &= ~C13_RX_DIV_YN1;
    field_write(&CLKRST_REG(CLKRST_PERI_CLK_CTRL12), C12_RX_DIV_Y_MASK,
                C12_RX_DIV_Y_SHIFT, 1U);
    field_write(&CLKRST_REG(CLKRST_PERI_CLK_CTRL13), C13_RX_DIV_Z_MASK,
                C13_RX_DIV_Z_SHIFT, 0U);
    field_write(&CLKRST_REG(CLKRST_PERI_CLK_CTRL12), C12_RX_DIV_X_MASK,
                C12_RX_DIV_X_SHIFT, 0U);

    if (yn1)
    {
        CLKRST_REG(CLKRST_PERI_CLK_CTRL13) |= C13_RX_DIV_YN1;
    }
    field_write(&CLKRST_REG(CLKRST_PERI_CLK_CTRL13), C13_RX_DIV_Z_MASK,
                C13_RX_DIV_Z_SHIFT, z);
    field_write(&CLKRST_REG(CLKRST_PERI_CLK_CTRL12), C12_RX_DIV_Y_MASK,
                C12_RX_DIV_Y_SHIFT, y);
    field_write(&CLKRST_REG(CLKRST_PERI_CLK_CTRL12), C12_RX_DIV_X_MASK,
                C12_RX_DIV_X_SHIFT, x);
    field_write(&CLKRST_REG(CLKRST_PERI_CLK_CTRL12), C12_RX_DIV_N_MASK,
                C12_RX_DIV_N_SHIFT, div_n);

    gpio_set_dir(I2S_DIN, GPIO_INPUT);
    gpio_matrix_in(I2S_DIN, I2S0_SIG_SD, false);

    /* 2. Compartir BCLK/LRCK con el RX.  Es un bit del TX_CONF, y el TX ya
     *     esta corriendo, asi que va de menos a mas con su handshake. */
    I2S0_REG(I2S_TX_CONF) |= TX_SIG_LOOPBACK;
    if (!tx_sync())
    {
        (void)i2s_rx_stop();
        return false;
    }

    /* 3. Timing del RX, espejo del del TX: esclavo, I2S estandar por el
     *     motor TDM con dos slots, 16 bits por slot y el dato un BCLK
     *     despues del flanco de LRCK (rx_msb_shift), que es como el ES8311
     *     saca ASDOUT.  rx_left_align se deja a 0 explicitamente: viene de
     *     fabrica en 1 y aqui se escribe el registro entero. */
    I2S0_REG(I2S_RX_CONF) = RX_SLAVE_MOD | RX_MSB_SHIFT | RX_MONO_FST_VLD |
                            RX_TDM_EN | RX_PCM_BYPASS |
                            ((bdiv - 1U) << RX_BCK_DIV_SHIFT);

    field_write(I2S0_REGP(I2S_RX_CONF1), WS_WIDTH_MASK, WS_WIDTH_SHIFT,
                I2S_FRAME_BITS - 1U);
    field_write(I2S0_REGP(I2S_RX_CONF1), BITS_MOD_MASK, BITS_MOD_SHIFT,
                I2S_SLOT_BITS - 1U);
    field_write(I2S0_REGP(I2S_RX_CONF1), HALF_SAMPLE_MASK, HALF_SAMPLE_SHIFT,
                I2S_FRAME_BITS - 1U);
    field_write(I2S0_REGP(I2S_RX_CONF1), CHAN_BITS_MASK, CHAN_BITS_SHIFT,
                I2S_SLOT_BITS - 1U);

    field_write(I2S0_REGP(I2S_RX_TDM_CTRL), TDM_CHAN_EN_MASK,
                TDM_CHAN_EN_SHIFT, 0x3U);
    field_write(I2S0_REGP(I2S_RX_TDM_CTRL), TDM_TOT_CHAN_MASK,
                TDM_TOT_CHAN_SHIFT, I2S_CHANNELS - 1U);

    /* El evento de fin de recepcion se ajusta al tamano del trozo que enlaza
     * audioIn.c, para que un eof caiga justo con un descriptor lleno. */
    I2S0_REG(I2S_RXEOF_NUM) = I2S_RX_EOF_BYTES - 1U;

    /* 4. Vaciar el FIFO de entrada antes de empezar: lo que hubiera dentro
     *    son restos del reset. */
    I2S0_REG(I2S_RX_CONF) |= RX_FIFO_RESET;
    I2S0_REG(I2S_RX_CONF) &= ~RX_FIFO_RESET;
    if (!rx_sync())
    {
        (void)i2s_rx_stop();
        return false;
    }
    I2S0_REG(I2S_RX_CONF) |= RX_START;
    rxStarted = true;
    return true;
}

/**
 * @ingroup audio
 *
 * Parar el RX y desconectarlo, dejando el TX exactamente como estaba: los
 * relojes no se cierran porque pueden seguir alimentando la salida, y el
 * bit de compartir relojes se quita para que el bloque vuelva a su puesta
 * mono-direccion.
 *
 * @return true
 */
bool i2s_rx_stop(void)
{
    I2S0_REG(I2S_RX_CONF) &= ~RX_START;
    I2S0_REG(I2S_RX_CONF) |= RX_RESET;
    I2S0_REG(I2S_RX_CONF) &= ~RX_RESET;

    I2S0_REG(I2S_TX_CONF) &= ~TX_SIG_LOOPBACK;
    (void)tx_sync();

    CLKRST_REG(CLKRST_PERI_CLK_CTRL11) &= ~C11_RX_CLK_EN;
    rxStarted = false;
    return true;
}

/**
 * @ingroup audio
 *
 * @return true si el mando RX esta arrancado (rx_start a 1).
 */
bool i2s_rx_started(void)
{
    return (I2S0_REG(I2S_RX_CONF) & RX_START) != 0U;
}

bool i2s_rx_running(void)
{
    return rxStarted && i2s_rx_started();
}
