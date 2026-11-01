/*
 * i2c — ver i2c.h.  IP I2C estándar de Espressif (COMD0-7 + FIFOs).
 *
 * Reloj: fuente XTAL 40 MHz seleccionada en HP_SYS_CLKRST.PERI_CLK_CTRL10
 * (no en CLK_CONF).  Timing calculado como el HAL (≈100 kHz para
 * I2C_BUS_HZ).
 *
 * El acceso a registros es por offsets crudos (SVD), igual que gpio.c; no
 * se incluye ningún header de Espressif, sólo esp32p4.h (bases) y
 * board.h/gpio.h/timer.h.
 */

#include "i2c.h"
#include "board.h"
#include "gpio.h"
#include "timer.h"
#include "esp32p4.h"

/* op_code del IP I2C del ESP32-S3/P4.  OJO: NO son los del ESP32 original
 * (0/1/2/3/4); Espressif los renumeró (i2c_ll.h / SVD del P4):
 *   RESTART=6, WRITE=1, READ=3, STOP=2, END=4. */
#define I2C_OP_RSTART   6U
#define I2C_OP_WRITE    1U
#define I2C_OP_READ     3U
#define I2C_OP_STOP     2U
#define I2C_OP_END      4U

#define I2C_TIMEOUT_MS  50U

/* --- I2C0 register offsets (SVD) -------------------------------- */
#define I2C_SCL_LOW_PERIOD   0x00
#define I2C_CTR              0x04
#define I2C_SR               0x08
#define I2C_TO               0x0C
#define I2C_FIFO_CONF        0x18
#define I2C_DATA             0x1C
#define I2C_INT_RAW          0x20
#define I2C_INT_CLR          0x24
#define I2C_SDA_HOLD         0x30
#define I2C_SDA_SAMPLE       0x34
#define I2C_SCL_HIGH_PERIOD  0x38
#define I2C_SCL_START_HOLD   0x40
#define I2C_SCL_RSTART_SETUP 0x44
#define I2C_SCL_STOP_HOLD    0x48
#define I2C_SCL_STOP_SETUP   0x4C
#define I2C_COMD_BASE        0x58   /* COMD0..COMD7, +4 each */

#define I2C0_REG(off)  (*(volatile uint32_t *)(I2C0_BASE + (off)))

/* CTR field masks */
#define I2C_CTR_FORCE_OUTS      0x07U       /* SDA/SCL_FORCE_OUT + SAMPLE_SCL_LEVEL */
#define I2C_CTR_RX_FULL_ACK     (1U << 3)   /* 1 = NACK al llenarse el RX FIFO */
#define I2C_CTR_MS_MODE         (1U << 4)
#define I2C_CTR_TRANS_START     (1U << 5)
#define I2C_CTR_TX_LSB_FIRST    (1U << 6)
#define I2C_CTR_RX_LSB_FIRST    (1U << 7)
#define I2C_CTR_ARBITRATION_EN  (1U << 9)
#define I2C_CTR_FSM_RST         (1U << 10)
#define I2C_CTR_CONF_UPGATE     (1U << 11)

/* TO / FIFO_CONF / INT / SR / DATA field masks */
#define I2C_TO_VALUE_MASK       0x1FU
#define I2C_TO_EN               (1U << 5)
#define I2C_FIFO_RX_RST         (1U << 12)
#define I2C_FIFO_TX_RST         (1U << 13)
#define I2C_INT_NACK            (1U << 10)
#define I2C_INT_TRANS_COMPLETE  (1U << 7)
#define I2C_INT_TIME_OUT        (1U << 8)
#define I2C_INT_ARB_LOST        (1U << 5)
#define I2C_INT_END_DETECT      (1U << 3)
#define I2C_COMD_DONE           (1U << 31)  /* bit done de COMDn (R/W/SS) */
#define I2C_SR_RXFIFO_CNT_MASK  0x3F00U
#define I2C_SR_RXFIFO_CNT_SH    8U
#define I2C_DATA_RDATA_MASK     0xFFU

/* --- HP_SYS_CLKRST offsets + field masks (SVD) ------------------- */
#define CLKRST_SOC_CLK_CTRL1     0x18
#define CLKRST_SOC_CLK_CTRL2     0x1C
#define CLKRST_PERI_CLK_CTRL10   0x40
#define CLKRST_PERI_CLK_CTRL116  0x80
#define CLKRST_REG(off)  (*(volatile uint32_t *)(HP_SYS_CLKRST_BASE + (off)))

#define CLKRST_I2C0_APB_CLK_EN   (1U << 12)     /* SOC_CLK_CTRL2   */
#define CLKRST_I2C0_SRC_SEL      (1U << 0)      /* PERI_CLK_CTRL10: 0 = XTAL */
#define CLKRST_I2C0_CLK_EN       (1U << 1)      /* PERI_CLK_CTRL10 */
#define CLKRST_I2C0_DIV_NUM      0x000003FCU    /* bits 9:2   */
#define CLKRST_I2C0_DIV_NUM_SH   2U
#define CLKRST_I2C0_DIV_NUMER    0x0003FC00U    /* bits 17:10 */
#define CLKRST_I2C0_DIV_NUMER_SH 10U
#define CLKRST_I2C0_DIV_DENOM    0x03FC0000U    /* bits 25:18 */
#define CLKRST_I2C0_DIV_DENOM_SH 18U

/* I2C0 signals in the GPIO matrix (gpio_sig_map.h). */
#define I2C0_SCL_SIG    68U
#define I2C0_SDA_SIG    69U

#define I2C_SRC_HZ      40000000U
#define I2C_BUS_HZ      100000U

static volatile uint32_t *i2c_comd(unsigned n)
{
    /* COMD0..COMD7 consecutivos de 32 bits (SVD).  Sólo importan [13:0]. */
    return (volatile uint32_t *)(I2C0_BASE + I2C_COMD_BASE + 4U * n);
}

static void i2c_wrf(uint32_t off, uint32_t mask, uint32_t shift, uint32_t val)
{
    uint32_t v = I2C0_REG(off);

    v = (v & ~mask) | ((val << shift) & mask);
    I2C0_REG(off) = v;
}

static uint32_t i2c_cmdval(uint32_t op, uint32_t nbytes, bool ack_check,
                           bool ack_val)
{
    /* i2c_ll_hw_cmd_t (P4): op_code[13:11], ack_val[10], ack_exp[9],
     * ack_en[8], byte_num[7:0].  ack_exp=0 => esperamos ACK del esclavo.
     * ack_en=1 sólo en escrituras (para detectar NACK); ack_val=1 sólo
     * en el último byte de una lectura (el maestro envía NACK). */
    return (op << 11) | ((ack_val ? 1U : 0U) << 10) | (0U << 9) |
           ((ack_check ? 1U : 0U) << 8) | (nbytes & 0xFFU);
}

static void i2c_pad(uint8_t pin)
{
    volatile uint32_t *pad =
        (volatile uint32_t *)IO_MUX_PIN_REG(IO_MUX_BASE, pin);
    uint32_t v = *pad;

    v &= ~(IO_MCU_SEL_MASK | IO_FUN_WPD | IO_FUN_DRV_MASK);
    v |= IO_MCU_SEL_GPIO | IO_FUN_IE | IO_FUN_WPU | IO_FUN_DRV_20MA;
    *pad = v;   /* func GPIO, IE, pull-up, 20 mA */
}

/* Timing del bus, calcado de i2c_ll_master_cal_bus_clk() +
 * i2c_ll_master_set_bus_timing() (ESP-IDF, esp32p4).  Unidades: ciclos de
 * i2c_sclk.  Para 40 MHz/100 kHz: half_cycle=200, wait_high=98, high=102,
 * sda_hold=50, sda_sample=100, setup=hold=200, clkm_div=1. */
static void i2c_set_bus_timing(uint32_t bus_hz)
{
    uint32_t clkm_div      = I2C_SRC_HZ / (bus_hz * 1024U) + 1U;
    uint32_t sclk_freq     = I2C_SRC_HZ / clkm_div;
    uint32_t half_cycle    = sclk_freq / bus_hz / 2U;
    uint32_t scl_wait_high = (bus_hz >= 80000U) ? (half_cycle / 2U - 2U)
                                                : (half_cycle / 4U);
    uint32_t scl_high      = half_cycle - scl_wait_high;
    uint32_t sda_hold      = half_cycle / 4U;
    uint32_t sda_sample    = half_cycle / 2U;
    uint32_t tout          = 34U - __builtin_clz(5U * half_cycle);

    i2c_wrf(CLKRST_PERI_CLK_CTRL10, CLKRST_I2C0_DIV_NUM,
            CLKRST_I2C0_DIV_NUM_SH, clkm_div - 1U);
    i2c_wrf(CLKRST_PERI_CLK_CTRL10, CLKRST_I2C0_DIV_NUMER,
            CLKRST_I2C0_DIV_NUMER_SH, 0U);
    i2c_wrf(CLKRST_PERI_CLK_CTRL10, CLKRST_I2C0_DIV_DENOM,
            CLKRST_I2C0_DIV_DENOM_SH, 0U);

    /* El TRM dice -1 en todos, pero NO en scl_high/wait_high (así la
     * frecuencia sale exacta, igual que el HAL). */
    i2c_wrf(I2C_SCL_LOW_PERIOD, 0x1FFU, 0, half_cycle - 1U);
    i2c_wrf(I2C_SCL_HIGH_PERIOD, 0x1FFU, 0, scl_high);
    i2c_wrf(I2C_SCL_HIGH_PERIOD, 0xFE00U, 9, scl_wait_high);
    i2c_wrf(I2C_SDA_HOLD, 0x1FFU, 0, sda_hold - 1U);
    i2c_wrf(I2C_SDA_SAMPLE, 0x1FFU, 0, sda_sample - 1U);
    i2c_wrf(I2C_SCL_RSTART_SETUP, 0x1FFU, 0, half_cycle - 1U);
    i2c_wrf(I2C_SCL_STOP_SETUP, 0x1FFU, 0, half_cycle - 1U);
    i2c_wrf(I2C_SCL_START_HOLD, 0x1FFU, 0, half_cycle - 1U);
    i2c_wrf(I2C_SCL_STOP_HOLD, 0x1FFU, 0, half_cycle - 1U);

    /* Timeout de línea clavada a 0 (time_out_value = 2^tout ciclos de bus).
     * El 0x1F que poníamos antes equivale a "nunca", así que un bus muerto
     * sólo se notaba por el reloj de pared, sin diferenciarlo de un NACK. */
    i2c_wrf(I2C_TO, I2C_TO_VALUE_MASK, 0, tout);
    I2C0_REG(I2C_TO) |= I2C_TO_EN;
}

/* Suelta los FIFOs y los deja desactivados (pulso, como el HAL: los bits
 * quedan armados si sólo se escriben a 1 y el FIFO se queda en reset). */
static void i2c_fifo_reset(void)
{
    I2C0_REG(I2C_FIFO_CONF) |= (I2C_FIFO_RX_RST | I2C_FIFO_TX_RST);
    I2C0_REG(I2C_FIFO_CONF) &= ~(I2C_FIFO_RX_RST | I2C_FIFO_TX_RST);
}

void i2c_init(void)
{
    uint32_t ctr;

    /* Reloj: APB (bus) + controlador (PERI_CLK_CTRL10) + fuente XTAL. */
    CLKRST_REG(CLKRST_SOC_CLK_CTRL2) |= CLKRST_I2C0_APB_CLK_EN;
    CLKRST_REG(CLKRST_PERI_CLK_CTRL10) &= ~CLKRST_I2C0_SRC_SEL; /* XTAL */
    CLKRST_REG(CLKRST_PERI_CLK_CTRL10) |= CLKRST_I2C0_CLK_EN;

    i2c_pad(I2C_SDA);
    i2c_pad(I2C_SCL);
    gpio_set_open_drain(I2C_SDA, true); /* pads open-drain (I2C) */
    gpio_set_open_drain(I2C_SCL, true);
    gpio_matrix_out(I2C_SCL, I2C0_SCL_SIG, false, false);
    gpio_matrix_out(I2C_SDA, I2C0_SDA_SIG, false, false);
    /* Entradas por la matriz: gpio_matrix_in() programa sig_in_sel, sin ese
     * bit el controlador muestrea el IOMUX y nunca ve el pad (todo NACK). */
    gpio_matrix_in(I2C_SCL, I2C0_SCL_SIG, false);
    gpio_matrix_in(I2C_SDA, I2C0_SDA_SIG, false);

    ctr = I2C0_REG(I2C_CTR);
    ctr &= ~(I2C_CTR_FORCE_OUTS | I2C_CTR_RX_FULL_ACK | I2C_CTR_TX_LSB_FIRST |
             I2C_CTR_RX_LSB_FIRST | I2C_CTR_ARBITRATION_EN);
    ctr |= I2C_CTR_MS_MODE;             /* master, open-drain (no force-out) */
    I2C0_REG(I2C_CTR) = ctr;

    i2c_set_bus_timing(I2C_BUS_HZ);
    i2c_fifo_reset();
    I2C0_REG(I2C_CTR) |= I2C_CTR_CONF_UPGATE;
}

/* Lanza cmds[0..n) y espera END_DETECT (o NACK/TIME_OUT/ARBITRATION_LOST). */
static int i2c_err = I2C_ERR_NONE;

int i2c_last_error(void)
{
    return i2c_err;
}

static bool i2c_run(const uint32_t *cmds, unsigned n)
{
    uint32_t t0 = millis();
    volatile uint32_t *last = i2c_comd(n - 1);
    unsigned i;

    i2c_err = I2C_ERR_NONE;
    I2C0_REG(I2C_INT_CLR) = 0xFFFFFFFFUL;
    I2C0_REG(I2C_CTR) |= I2C_CTR_FSM_RST;
    for (i = 0; i < n; i++)
    {
        *i2c_comd(i) = cmds[i];
    }
    I2C0_REG(I2C_CTR) |= I2C_CTR_CONF_UPGATE;
    I2C0_REG(I2C_CTR) |= I2C_CTR_TRANS_START;

    while (millis() - t0 < I2C_TIMEOUT_MS)
    {
        uint32_t raw = I2C0_REG(I2C_INT_RAW);

        if (raw & I2C_INT_NACK)
        {
            i2c_err = I2C_ERR_NACK;
        }
        else if (raw & I2C_INT_TIME_OUT)
        {
            i2c_err = I2C_ERR_BUS_TIMEOUT;
        }
        else if (raw & I2C_INT_ARB_LOST)
        {
            i2c_err = I2C_ERR_ARBITRATION;
        }
        else if ((raw & (I2C_INT_END_DETECT | I2C_INT_TRANS_COMPLETE)) ||
                 (*last & I2C_COMD_DONE))
        {
            /* Fin de transacción. end_detect solo no se ve cuando el esclavo
             * responde ACK: el HAL espera TRANS_COMPLETE (bit 7) o el bit
             * done del último COMD, y con esto el probe caducaba a los 50 ms
             * justo en las direcciones de los dispositivos pobladados. */
            I2C0_REG(I2C_INT_CLR) = 0xFFFFFFFFUL;
            return true;
        }
        else
        {
            continue;
        }
        /* Transacción muerta: sin esto los bytes que ya estaban en el TX FIFO
         * saldrían en la transacción siguiente y el escaneo se encadenaría. */
        I2C0_REG(I2C_INT_CLR) = 0xFFFFFFFFUL;
        I2C0_REG(I2C_CTR) |= I2C_CTR_FSM_RST;
        i2c_fifo_reset();
        return false;
    }
    i2c_err = I2C_ERR_STUCK;
    I2C0_REG(I2C_INT_CLR) = 0xFFFFFFFFUL;
    I2C0_REG(I2C_CTR) |= I2C_CTR_FSM_RST;
    i2c_fifo_reset();
    return false;
}

static void i2c_tx_byte(uint8_t b)
{
    I2C0_REG(I2C_DATA) = b;
}

/* Drena len bytes del RX FIFO (ya recibidos al ver END_DETECT). */
static bool i2c_rx_drain(uint8_t *data, size_t len)
{
    uint32_t t0 = millis();
    size_t i = 0;

    while (i < len && millis() - t0 < I2C_TIMEOUT_MS)
    {
        if ((I2C0_REG(I2C_SR) & I2C_SR_RXFIFO_CNT_MASK) >>
            I2C_SR_RXFIFO_CNT_SH)
        {
            data[i++] = (uint8_t)(I2C0_REG(I2C_DATA) & I2C_DATA_RDATA_MASK);
        }
    }
    return i == len;
}

bool i2c_probe(uint8_t dev)
{
    uint32_t cmds[4];

    i2c_fifo_reset();
    cmds[0] = i2c_cmdval(I2C_OP_RSTART, 0, false, false);
    i2c_tx_byte((uint8_t)((dev << 1) | 0U));
    cmds[1] = i2c_cmdval(I2C_OP_WRITE, 1, true, false);
    /* STOP: sin él la línea queda retenida tras cada sondeo y el siguiente
     * address nunca se emite (el FSM arranca con el bus ocupado). */
    cmds[2] = i2c_cmdval(I2C_OP_STOP, 0, false, false);
    cmds[3] = i2c_cmdval(I2C_OP_END, 0, false, false);
    return i2c_run(cmds, 4);
}

bool i2c_write(uint8_t dev, const uint8_t *data, size_t len)
{
    uint32_t cmds[4];
    size_t i;

    if (!data || !len || len > 31)
    {
        return false;
    }
    i2c_fifo_reset();
    cmds[0] = i2c_cmdval(I2C_OP_RSTART, 0, false, false);
    i2c_tx_byte((uint8_t)((dev << 1) | 0U));
    for (i = 0; i < len; i++)
    {
        i2c_tx_byte(data[i]);
    }
    cmds[1] = i2c_cmdval(I2C_OP_WRITE, (uint32_t)(len + 1), true, false);
    cmds[2] = i2c_cmdval(I2C_OP_STOP, 0, false, false);
    cmds[3] = i2c_cmdval(I2C_OP_END, 0, false, false);
    return i2c_run(cmds, 4);
}

bool i2c_read(uint8_t dev, uint8_t *data, size_t len)
{
    /* [RSTART][WRITE addr][READ len-1 ACK][READ 1 NACK][STOP][END] (máx 6). */
    uint32_t cmds[6];
    unsigned n = 0;

    if (!data || !len || len > 32)
    {
        return false;
    }
    i2c_fifo_reset();
    cmds[n++] = i2c_cmdval(I2C_OP_RSTART, 0, false, false);
    i2c_tx_byte((uint8_t)((dev << 1) | 1U));
    cmds[n++] = i2c_cmdval(I2C_OP_WRITE, 1, true, false);
    if (len > 1)
    {
        cmds[n++] = i2c_cmdval(I2C_OP_READ, (uint32_t)(len - 1), false, false);
    }
    cmds[n++] = i2c_cmdval(I2C_OP_READ, 1, false, true);
    cmds[n++] = i2c_cmdval(I2C_OP_STOP, 0, false, false);
    cmds[n++] = i2c_cmdval(I2C_OP_END, 0, false, false);
    if (!i2c_run(cmds, n))
    {
        return false;
    }
    return i2c_rx_drain(data, len);
}

bool i2c_write_reg8(uint8_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };

    return i2c_write(dev, buf, 2);
}

bool i2c_write_reg16(uint8_t dev, uint16_t reg, uint8_t val)
{
    uint8_t buf[3] = { (uint8_t)(reg >> 8), (uint8_t)reg, val };

    return i2c_write(dev, buf, 3);
}

bool i2c_read_regs(uint8_t dev, uint8_t reg, uint8_t *buf, size_t len)
{
    /* [RSTART][WRITE addr+reg][RSTART][WRITE addr|R][READs][STOP][END] = 8. */
    uint32_t cmds[8];
    unsigned n = 0;

    if (!buf || !len || len > 32)
    {
        return false;
    }
    i2c_fifo_reset();
    cmds[n++] = i2c_cmdval(I2C_OP_RSTART, 0, false, false);
    i2c_tx_byte((uint8_t)((dev << 1) | 0U));
    i2c_tx_byte(reg);
    cmds[n++] = i2c_cmdval(I2C_OP_WRITE, 2, true, false);
    cmds[n++] = i2c_cmdval(I2C_OP_RSTART, 0, false, false);
    i2c_tx_byte((uint8_t)((dev << 1) | 1U));
    cmds[n++] = i2c_cmdval(I2C_OP_WRITE, 1, true, false);
    if (len > 1)
    {
        cmds[n++] = i2c_cmdval(I2C_OP_READ, (uint32_t)(len - 1), false, false);
    }
    cmds[n++] = i2c_cmdval(I2C_OP_READ, 1, false, true);
    cmds[n++] = i2c_cmdval(I2C_OP_STOP, 0, false, false);
    if (n > 7)
    {
        return false;
    }
    /* END no cabe si n==7: COMD7 existe (0-7 = 8 comandos) -> añadir. */
    cmds[n++] = i2c_cmdval(I2C_OP_END, 0, false, false);
    if (n > 8)
    {
        return false;
    }
    if (!i2c_run(cmds, n))
    {
        return false;
    }
    return i2c_rx_drain(buf, len);
}

bool i2c_read_regs16(uint8_t dev, uint16_t reg, uint8_t *buf, size_t len)
{
    /* Igual que read_regs pero con dirección de 16 bits (8 comandos máx). */
    uint32_t cmds[8];
    unsigned n = 0;

    if (!buf || !len || len > 32)
    {
        return false;
    }
    i2c_fifo_reset();
    cmds[n++] = i2c_cmdval(I2C_OP_RSTART, 0, false, false);
    i2c_tx_byte((uint8_t)((dev << 1) | 0U));
    i2c_tx_byte((uint8_t)(reg >> 8));
    i2c_tx_byte((uint8_t)reg);
    cmds[n++] = i2c_cmdval(I2C_OP_WRITE, 3, true, false);
    cmds[n++] = i2c_cmdval(I2C_OP_RSTART, 0, false, false);
    i2c_tx_byte((uint8_t)((dev << 1) | 1U));
    cmds[n++] = i2c_cmdval(I2C_OP_WRITE, 1, true, false);
    if (len > 1)
    {
        cmds[n++] = i2c_cmdval(I2C_OP_READ, (uint32_t)(len - 1), false, false);
    }
    cmds[n++] = i2c_cmdval(I2C_OP_READ, 1, false, true);
    cmds[n++] = i2c_cmdval(I2C_OP_STOP, 0, false, false);
    cmds[n++] = i2c_cmdval(I2C_OP_END, 0, false, false);
    if (n > 8)
    {
        return false;
    }
    if (!i2c_run(cmds, n))
    {
        return false;
    }
    return i2c_rx_drain(buf, len);
}
