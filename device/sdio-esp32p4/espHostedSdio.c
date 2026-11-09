/*
 * MIT License
 *
 * Copyright (c) 2025 Surya Poudel
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#if !defined(TARGET_SOC_ESP32P4)
#error "drivers/esp_hosted/esp_hosted_sdio.c currently supports ESP32-P4 only"
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "esp_bit_defs.h"
#include "esp_hosted.h"
#include "esp_hosted_sdio.h"
#include "delay.h"
#include "esp_rom_gpio.h"
#include "hal/cache_ll.h"
#include "hal/gpio_ll.h"
#include "hal/sd_types.h"
#include "hal/sdmmc_ll.h"
#include "soc/io_mux_reg.h"
#include "soc/sdmmc_periph.h"
#include "soc/sdmmc_pins.h"
#include "usb_serial.h"

/*
 * Port Xinu (W2.b).  Unica funcion del driver tocada por encima de este
 * fichero, y el unico motivo por el que existe este include: el candado del
 * controlador.  El ESP32-C6 (slot 1) y la microSD (slot 0, la que arranca el
 * FAT de Xinu) comparten el BLOQUE DesignWare de SDHOST (0x50083000): un
 * motor de comandos, una FIFO y un DMA.  Cada transaccion de aqui tiene que
 * ser atomica frente a una lectura de disco, asi que se toma el monitor
 * reentrante del kernel alrededor de todo acceso al engine.
 */
#include <sdmmc.h>

/* CUARTA DESVIACION (vease esp_hosted_sdio_setup_matrix_pin): los pads del
 * slot 1 se configuran con la receta de IO_MUX que Xinu ya valido en placa en
 * W1, y eso son estos dos headers.  IO_MUX_BASE esta en esp32p4.h y los bits
 * del pad (IO_MUX_PIN_REG / IO_MCU_SEL_* / IO_FUN_*) en gpio.h. */
#include <esp32p4.h>
#include <gpio.h>

#define ESP_HOSTED_SDIO_BLOCK_SIZE              512U
#define ESP_HOSTED_SDIO_PROBE_HOST_DIV          10U
#define ESP_HOSTED_SDIO_PROBE_CARD_DIV          20U
#define ESP_HOSTED_SDIO_CMD_TIMEOUT_LOOPS       200000U
#define ESP_HOSTED_SDIO_DATA_TIMEOUT_LOOPS      1200000U
#define ESP_HOSTED_SDIO_BUSY_TIMEOUT_LOOPS      600000U
#define ESP_HOSTED_SDIO_IO_READY_RETRIES        200U
#define ESP_HOSTED_SDIO_ATTACH_RETRIES          3U
#define ESP_HOSTED_SDIO_DATA_PATH_RETRIES       32U
#define ESP_HOSTED_SDIO_DMA_BUF_SIZE            4096U
#define ESP_HOSTED_SDIO_FRAME_BUF_SIZE          2048U
#define ESP_HOSTED_SDIO_TX_FRAME_BUF_SIZE       2048U
#define ESP_HOSTED_SDIO_TX_BUF_MAX              0x1000U
#define ESP_HOSTED_SDIO_TX_BUF_MASK             0x0FFFU
#define ESP_HOSTED_SDIO_RX_BUF_WINDOW           1536U
#define ESP_HOSTED_SDIO_RX_BYTE_MAX             0x100000U
#define ESP_HOSTED_SDIO_SLAVE_LEN_MASK          0xFFFFFU
#define ESP_HOSTED_SDIO_ADDR_MASK               0x3FFU
#define ESP_HOSTED_SDIO_FUNC_0                  0U
#define ESP_HOSTED_SDIO_FUNC_1                  1U
#define ESP_HOSTED_SDIO_SLOT0                   0U
#define ESP_HOSTED_SDIO_SLOT1                   1U
#define ESP_HOSTED_SDIO_CMD0                    0U
#define ESP_HOSTED_SDIO_CMD3                    3U
#define ESP_HOSTED_SDIO_CMD5                    5U
#define ESP_HOSTED_SDIO_CMD7                    7U
#define ESP_HOSTED_SDIO_CMD52                   52U
#define ESP_HOSTED_SDIO_CMD53                   53U
#define ESP_HOSTED_SDIO_CMD5_OCR_ARG            0x00FF8000U
#define ESP_HOSTED_SDIO_R4_READY                BIT(31)
#define ESP_HOSTED_SDIO_RCA_SHIFT               16U
#define ESP_HOSTED_SDIO_INT_NEW_PACKET          BIT(23)
#define ESP_HOSTED_SDIO_INT_START_THROTTLE      BIT(7)
#define ESP_HOSTED_SDIO_INT_STOP_THROTTLE       BIT(6)
#define ESP_HOSTED_SDIO_CCCR_FN_ENABLE          0x02U
#define ESP_HOSTED_SDIO_CCCR_FN_READY           0x03U
#define ESP_HOSTED_SDIO_CCCR_INT_ENABLE         0x04U
#define ESP_HOSTED_SDIO_CCCR_BUS_WIDTH          0x07U
#define ESP_HOSTED_SDIO_CCCR_BLKSIZEL           0x10U
#define ESP_HOSTED_SDIO_CCCR_BLKSIZEH           0x11U
#define ESP_HOSTED_SDIO_CCCR_BUS_WIDTH_4        0x02U
#define ESP_HOSTED_SDIO_CCCR_FN1_EN             BIT(1)
#define ESP_HOSTED_SDIO_CCCR_INT_MASTER_EN      BIT(0)
#define ESP_HOSTED_SDIO_SLAVE_BASE              0x3FF55000U
#define ESP_HOSTED_SDIO_HOST_TO_SLAVE_INTR      (ESP_HOSTED_SDIO_SLAVE_BASE + 0x8CU)
#define ESP_HOSTED_SDIO_SLAVE_INT_RAW_REG       (ESP_HOSTED_SDIO_SLAVE_BASE + 0x50U)
#define ESP_HOSTED_SDIO_SLAVE_INT_CLR_REG       (ESP_HOSTED_SDIO_SLAVE_BASE + 0xD4U)
#define ESP_HOSTED_SDIO_SLAVE_PACKET_LEN_REG    (ESP_HOSTED_SDIO_SLAVE_BASE + 0x60U)
#define ESP_HOSTED_SDIO_SLAVE_TOKEN_RDATA       (ESP_HOSTED_SDIO_SLAVE_BASE + 0x44U)
#define ESP_HOSTED_SDIO_SLAVE_CMD53_END_ADDR    0x1F800U
#define ESP_HOSTED_SDIO_HOST_INTR_OPEN_DATA     0U
#define ESP_HOSTED_SDIO_MORE_FRAGMENT           BIT(0)
#define ESP_HOSTED_SDIO_CMD_ERR_MASK            (SDMMC_LL_EVENT_RESP_ERR | SDMMC_LL_EVENT_RTO | SDMMC_LL_EVENT_RCRC | SDMMC_LL_EVENT_HLE | SDMMC_LL_EVENT_SBE | SDMMC_LL_EVENT_EBE)
#define ESP_HOSTED_SDIO_DATA_ERR_MASK           (SDMMC_LL_EVENT_DCRC | SDMMC_LL_EVENT_FRUN | SDMMC_LL_EVENT_HTO | SDMMC_LL_EVENT_EBE | SDMMC_LL_EVENT_SBE)
#define ESP_HOSTED_SDIO_DMA_DONE_MASK           (SDMMC_LL_EVENT_DMA_TI | SDMMC_LL_EVENT_DMA_RI | SDMMC_LL_EVENT_DMA_NI)
#define ESP_HOSTED_SDIO_DMA_ERR_MASK            (BIT(2) | BIT(4) | BIT(5) | BIT(9))
#define ESP_HOSTED_SDIO_IDSTS_DMA_FSM_SHIFT     13U
#define ESP_HOSTED_SDIO_IDSTS_DMA_FSM_MASK      0xFU
#define ESP_HOSTED_SDIO_DMA_FSM_SUSPEND         1U
#define ESP_HOSTED_SDIO_DMA_FSM_READ_REQ_WAIT   4U
#define ESP_HOSTED_SDIO_DMA_FSM_WRITE_REQ_WAIT  5U

/* Adición Xinu (DESVIACION LP_IOMUX, ver esp_hosted_sdio_setup_matrix_pin): el
 * IO_MUX de bajo poder.  DR_REG_LP_IOMUX_BASE = 0x5012B000, pad n en +0x08+4n,
 * y el bit 4 de ese registro es MUX_SEL (1 = lo manda el GPIO de bajo poder).
 * Solo cubre GPIO0..GPIO15.  Los mismos valores, medidos y validados en placa,
 * estan en sdioProbe.c (sdioPadToMatrix) y en include/sdio.h. */
#define ESP_HOSTED_SDIO_LP_IOMUX_BASE           0x5012B000UL
#define ESP_HOSTED_SDIO_LP_PAD_REG(n)           (ESP_HOSTED_SDIO_LP_IOMUX_BASE + 0x08UL + 4UL * (n))
#define ESP_HOSTED_SDIO_LP_MUX_SEL              BIT(4)
#define ESP_HOSTED_SDIO_LAST_LP_PAD             15U

#ifndef ESP_HOSTED_SDIO_DEBUG
#define ESP_HOSTED_SDIO_DEBUG 0
#endif

#if ESP_HOSTED_SDIO_DEBUG
#define ESP_HOSTED_SDIO_LOG(...) printf(__VA_ARGS__)
#else
#define ESP_HOSTED_SDIO_LOG(...) ((void)0)
#endif

#define ESP_HOSTED_SDIO_ERR(...) printf("[esp_hosted_sdio] " __VA_ARGS__)

typedef enum
{
    ESP_HOSTED_SDIO_RESP_NONE = 0,
    ESP_HOSTED_SDIO_RESP_SHORT = 1,
    ESP_HOSTED_SDIO_RESP_LONG = 2,
} esp_hosted_sdio_resp_t;

typedef struct __attribute__((packed))
{
    uint8_t if_type : 4;
    uint8_t if_num : 4;
    uint8_t flags;
    uint16_t len;
    uint16_t offset;
    uint16_t checksum;
    uint16_t seq_num;
    uint8_t throttle_cmd : 2;
    uint8_t reserved2 : 6;
    union
    {
        uint8_t reserved3;
        uint8_t hci_pkt_type;
        uint8_t priv_pkt_type;
    };
} esp_hosted_sdio_payload_header_t;

typedef struct
{
    esp_hosted_sdio_config_t config;
    bool attached;
    bool tx_throttled;
    uint16_t rca;
    uint32_t rx_byte_count;
    uint32_t tx_buf_count;
    sdmmc_desc_t dma_desc __attribute__((aligned(64)));
    uint8_t dma_bounce[ESP_HOSTED_SDIO_DMA_BUF_SIZE] __attribute__((aligned(64)));
    uint8_t frame_buf[ESP_HOSTED_SDIO_FRAME_BUF_SIZE] __attribute__((aligned(64)));
    uint8_t tx_frame[ESP_HOSTED_SDIO_TX_FRAME_BUF_SIZE] __attribute__((aligned(64)));
} esp_hosted_sdio_ctx_t;

static sdmmc_dev_t *const s_sdmmc = &SDMMC;
static esp_hosted_sdio_ctx_t s_ctx;
static uint32_t s_cmd52_int_raw_fail_count;

static size_t esp_hosted_sdio_transport_tx(const esp_hosted_frame_info_t *info,
                                           const void *payload,
                                           size_t len,
                                           void *ctx);
static void esp_hosted_sdio_transport_poll(void *ctx);

static const esp_hosted_transport_t s_transport = {
    .tx = esp_hosted_sdio_transport_tx,
    .poll = esp_hosted_sdio_transport_poll,
    .ctx = &s_ctx,
};

static inline uint32_t esp_hosted_sdio_get_events(void)
{
    return sdmmc_ll_get_interrupt_raw(s_sdmmc);
}

static inline uint32_t esp_hosted_sdio_round_up_blocks(uint32_t len)
{
    return (len + ESP_HOSTED_SDIO_BLOCK_SIZE - 1U) / ESP_HOSTED_SDIO_BLOCK_SIZE;
}

static uint16_t esp_hosted_sdio_compute_checksum(const uint8_t *buf, uint32_t len)
{
    uint32_t i;
    uint32_t checksum = 0U;

    if (buf == NULL)
    {
        return 0U;
    }

    for (i = 0; i < len; i++)
    {
        checksum += buf[i];
    }

    return (uint16_t)checksum;
}

static bool esp_hosted_sdio_wait_cmd_taken(uint32_t timeout_loops)
{
    while (timeout_loops-- > 0U)
    {
        if (s_sdmmc->cmd.start_command == 0U)
        {
            return true;
        }
    }
    return false;
}

/* Diagnostico de arranque (W2.b): cuando la orden interna de update-clock
 * falla, vuelca el estado del motor tal y como esta en ese instante.  No
 * cambia nada, solo lee. */
static uint32_t s_clkdbg_step;

static void esp_hosted_sdio_log_clkfail(const char *why)
{
    ESP_HOSTED_SDIO_ERR("clkupd step=%u %s cmd=0x%08lx arg=0x%08lx raw=0x%08lx "
                        "clkena=0x%08lx clksrc=0x%08lx clkdiv=0x%08lx status=0x%08lx "
                        "busy=%u start=%u\n",
                        (unsigned)s_clkdbg_step, why,
                        (unsigned long)s_sdmmc->cmd.val,
                        (unsigned long)s_sdmmc->cmdarg,
                        (unsigned long)esp_hosted_sdio_get_events(),
                        (unsigned long)s_sdmmc->clkena.val,
                        (unsigned long)s_sdmmc->clksrc.val,
                        (unsigned long)s_sdmmc->clkdiv.val,
                        (unsigned long)s_sdmmc->status.val,
                        (unsigned)s_sdmmc->status.data_busy,
                        (unsigned)s_sdmmc->cmd.start_command);
}

static bool esp_hosted_sdio_wait_reset_done(void)
{
    uint32_t wait_loops = ESP_HOSTED_SDIO_CMD_TIMEOUT_LOOPS;

    while (wait_loops-- > 0U)
    {
        if (sdmmc_ll_is_controller_reset_done(s_sdmmc) &&
            sdmmc_ll_is_fifo_reset_done(s_sdmmc) &&
            sdmmc_ll_is_dma_reset_done(s_sdmmc))
        {
            return true;
        }
    }
    return false;
}

static void esp_hosted_sdio_log_reset_state(void)
{
    ESP_HOSTED_SDIO_ERR("reset state ctrl=0x%08lx clkdiv=0x%08lx clksrc=0x%08lx clkena=0x%08lx raw=0x%08lx\n",
                        (unsigned long)s_sdmmc->ctrl.val,
                        (unsigned long)s_sdmmc->clkdiv.val,
                        (unsigned long)s_sdmmc->clksrc.val,
                        (unsigned long)s_sdmmc->clkena.val,
                        (unsigned long)esp_hosted_sdio_get_events());
}

static bool esp_hosted_sdio_wait_data_idle(void)
{
    uint32_t timeout_loops = ESP_HOSTED_SDIO_BUSY_TIMEOUT_LOOPS;

    while (timeout_loops-- > 0U)
    {
        if (!sdmmc_ll_is_card_data_busy(s_sdmmc))
        {
            return true;
        }
        delay_us(1U);
    }
    return false;
}

/*
 * TERCERA DESVIACION respecto al codigo de referencia.  La orden interna de
 * actualizacion de reloj (CMD con update_clk_reg=1).  Dos diferencias con el
 * original, ambas impuestas por lo que se ha visto en la placa:
 *
 *  1) HLE no es fatal.  Cuando la update-clock sale bien, el bloque tambien
 *     levanta HLE (bit 12 de RINTSTS).  El original lo cuenta como error de
 *     comando y aborta en la primera subida de reloj.  Xinu ya lo padebio y lo
 *     tiene resuelto desde W1 en sdClockUpdateSlot()
 *     (device/sdmmc-esp32p4/sdHw.c), validado en placa con la microSD y con el
 *     C6: se limpia HLE y se reintenta (hasta 16); lo que de verdad termina la
 *     operacion es que CMD_START se auto-limpie.
 */
#define ESP_HOSTED_SDIO_CLK_UPDATE_TRIES        16U
#define ESP_HOSTED_SDIO_CLK_UPDATE_HLE          SDMMC_LL_EVENT_HLE
#define ESP_HOSTED_SDIO_CLK_UPDATE_FATAL        \
    (SDMMC_LL_EVENT_RESP_ERR | SDMMC_LL_EVENT_RTO | SDMMC_LL_EVENT_RCRC |     \
     SDMMC_LL_EVENT_SBE | SDMMC_LL_EVENT_EBE)

static bool esp_hosted_sdio_update_clock_registers_locked(uint8_t slot)
{
    sdmmc_hw_cmd_t cmd = {0};
    uint32_t attempt;
    uint32_t timeout_loops;
    bool dat0_busy;

    cmd.card_num = slot;
    cmd.update_clk_reg = 1;
    cmd.use_hold_reg = 1;
    cmd.start_command = 1;

    /*
     * CUARTO DESVIO respecto al original, medido en placa.  STATUS bit 9 es
     * data_busy (DAT0 clavado por la tarjeta; valor por defecto 1).  La trazas
     * lo dejan claro:
     *   - sonda a 400 kHz : STATUS=0x00008906 -> data_busy=0 -> la update-clock
     *     se toma y CMD_START se auto-limpia;
     *   - subida a 20 MHz : STATUS=0x0001a306 -> data_busy=1 y aqui la misma
     *     orden no se toma nunca (raw=0x00000000, ni Command Done ni error).
     * La diferencia aparece justo despues de habilitar la funcion 1 del C6
     * (CMD52 a CCCR_FN_ENABLE / CCCR_INT_ENABLE + BLKSIZE), que es cuando el
     * firmware esp_hosted del coprocesador tiene pendencias en el camino de
     * datos y mantiene DAT0 abajo.  cmd.wait_complete=1, que el original pone
     * siempre, pide al CIU que espere a que termine la transferencia de datos
     * pendiente ANTES de servir la orden; con el C6 en ese estado la espera no
     * termina nunca, CMD_START se queda a 1 y el siguiente intento escribe CMD
     * encima -por eso aparecen los HLE (bit 12) en los reintentos-.
     *
     * update_clk_reg=1 NO envia nada a la tarjeta: solo traslada CLKDIV, CLKSRC
     * y CLKENA al dominio del reloj de la tarjeta, asi que no tiene sentido
     * esperarse a un DTO que ademas no corresponde a esta orden.  La regla que
     * sale de la medida es: primero se da margen (hasta ~0,6 s) a que la
     * tarjeta suelte DAT0 sola, que es lo normal despues de un CMD52 de
     * escritura; si aun asi sigue clavada, la update-clock se lanza con
     * wait_complete=0.  El candado sdmmcBusLock() garantiza que el slot 0 no
     * tiene ninguna transferencia en vuelo, asi que no se pisa la microSD.
     */
    dat0_busy = (s_sdmmc->status.data_busy != 0U);
    if (dat0_busy)
    {
        dat0_busy = !esp_hosted_sdio_wait_data_idle();
    }
    cmd.wait_complete = dat0_busy ? 0U : 1U;
    if (dat0_busy)
    {
        printf("[wclkb] step=%u DAT0 busy -> update-clock sin wait_complete STATUS=0x%08lx\r\n",
               (unsigned)s_clkdbg_step, (unsigned long)s_sdmmc->status.val);
    }

    for (attempt = 0U; attempt < ESP_HOSTED_SDIO_CLK_UPDATE_TRIES; attempt++)
    {
        if (!esp_hosted_sdio_wait_cmd_taken(ESP_HOSTED_SDIO_CMD_TIMEOUT_LOOPS))
        {
            /* CMD_START sigue en 1: el motor no ha tomado la orden anterior.
             * Es exactamente el caso que W1 ya trata devolviendo FALSE (ver
             * sdClockUpdateSlot): NO se resetean FIFO ni DMA aqui.  Ese reset
             * afecta al bloque COMPARTIDO (el de la microSD) y, medido en
             * placa, no destraba el CMD aparca- do: solo ensucia la trazas y
             * deja al slot 0 colgado.  Se aborta y se deja que el candado y el
             * dueno del bus decidan. */
            esp_hosted_sdio_log_clkfail("cmd-not-taken");
            return false;
        }

        sdmmc_ll_clear_interrupt(s_sdmmc, UINT32_MAX);
        sdmmc_ll_set_command_arg(s_sdmmc, 0U);
        sdmmc_ll_set_command(s_sdmmc, cmd);

        timeout_loops = ESP_HOSTED_SDIO_CMD_TIMEOUT_LOOPS;
        while (timeout_loops-- > 0U)
        {
            uint32_t raw = esp_hosted_sdio_get_events();

            if ((raw & ESP_HOSTED_SDIO_CLK_UPDATE_HLE) != 0U)
            {
                /* Esperado en una update-clock: se limpia y se vuelve a lanzar. */
                sdmmc_ll_clear_interrupt(s_sdmmc, ESP_HOSTED_SDIO_CLK_UPDATE_HLE);
                break;
            }
            if ((raw & ESP_HOSTED_SDIO_CLK_UPDATE_FATAL) != 0U)
            {
                sdmmc_ll_clear_interrupt(s_sdmmc, UINT32_MAX);
                esp_hosted_sdio_log_clkfail("err");
                return false;
            }
            if (s_sdmmc->cmd.start_command == 0U)
            {
                sdmmc_ll_clear_interrupt(s_sdmmc, UINT32_MAX);
                return true;
            }
        }

        if (timeout_loops == 0U)
        {
            esp_hosted_sdio_log_clkfail("busy-after");
            return false;
        }
    }

    ESP_HOSTED_SDIO_ERR("clock update still busy after %u tries\n",
                        (unsigned)ESP_HOSTED_SDIO_CLK_UPDATE_TRIES);
    return false;
}

/*
 * Punto de acceso 1/3 al motor compartido.  Esta orden interna (update-clock)
 * escribe CMD y lee RINTSTS igual que un comando normal, asi que tambien tiene
 * que ir bajo el candado.
 */
static bool esp_hosted_sdio_update_clock_registers(uint8_t slot)
{
    bool done;

    sdmmcBusLock();
    done = esp_hosted_sdio_update_clock_registers_locked(slot);
    sdmmcBusUnlock();
    return done;
}

static bool esp_hosted_sdio_set_clock(uint8_t slot, uint32_t host_div, uint8_t card_div)
{
    uint32_t host_khz;
    uint32_t target_khz;
    uint32_t slot_div;

    /*
     * UNICO DESVIO 2.  El original baja la frecuencia en dos etapas: un
     * divisor GLOBAL sdio_ls (host_div, sobre los 160 MHz de la SDIO PLL) y
     * luego el divisor POR SLOT CLKDIV (card_div), que divide entre 2*card_div
     * y se salta cuando vale 0.  Ese divisor global es compartido con el slot
     * 0, donde la microSD ya esta funcionando a 160/4 = 40 MHz, asi que aqui
     * el GLOBAL NO SE TOCA y la frecuencia pedida se consigue entera con el
     * divisor del slot.
     *
     * La equivalencia es directa: el original quiere
     *     f = (160000 / host_div) / (card_div ? 2 * card_div : 1)
     * y con el global fijado en 40000 kHz queda
     *     CLKDIV_slot = ceil(40000 / (2 * f))
     * redondeando HACIA ARRIBA para que el reloj salga igual o mas lento que
     * el pedido, nunca mas rapido.
     *
     * Comprobado contra los dos puntos que usa el driver:
     *   sondas  host_div=10 card_div=20 -> f=400 kHz    -> CLKDIV=50
     *   trabajo host_div=7  card_div=0  -> f=22857 kHz  -> CLKDIV=1 = 20 MHz
     * que son exactamente SD_CARD_DIV_PROBE y SD_CARD_DIV_WORK de sdhost.h,
     * los divisores con los que el slot 1 ya respondio en placa en W1.
     */
    if (host_div == 0U)
    {
        host_div = 1U;
    }
    host_khz = 160000U / host_div;
    if (host_khz == 0U)
    {
        host_khz = 1U;
    }

    target_khz = (card_div != 0U) ? (host_khz / ((uint32_t)card_div * 2U))
                                  : host_khz;
    if (target_khz == 0U)
    {
        target_khz = 1U;
    }

    slot_div = (40000U + (2U * target_khz) - 1U) / (2U * target_khz);
    if (slot_div < 1U)
    {
        slot_div = 1U;
    }
    if (slot_div > 255U)
    {
        slot_div = 255U;
    }

    /*
     * SEGUNDA MITAD del DESVIO 2, CORREGIDA EN PLACA.  CLKSRC no elige "una
     * salida del divisor global": sdhost_clksrc_reg_t enlaza cada slot con una
     * de las CUATRO divisiones internas del bloque, y la division n es el byte
     * n de CLKDIV.  Aqui soliamos poner card1 = 0 "porque con 1 el slot no
     * arrancaba", y con eso el C6 quedaba enlazado a la division de la microSD:
     * el divisor escrito para el slot 1 (clk_divider1) no goberna nada y la
     * frecuencia real del enlace era la que tuviera puesta el slot 0.  Esa es
     * la explicacion de que las CMD52 de registro funcionaran "a 400 kHz" y de
     * que la subida a 20 MHz no cambiara el comportamiento del CMD53.
     * El valor correcto es el del SDK (card1 = 1, puesto por
     * sdmmc_ll_set_card_clock_div), y aqui NO se toca.
     */
    sdmmc_ll_enable_card_clock(s_sdmmc, slot, false);
    s_clkdbg_step = 1U;
    /* Traza de arranque (W2.b): estado del motor ANTES de la primera
     * update-clock, leido de los propios registros. */
    printf("[wclock] slot=%u div=%u host=%u card=%u CLKDIV=%u CTYPE=0x%08lx "
           "CLKENA=0x%08lx STATUS=0x%08lx\r\n",
           (unsigned)slot, (unsigned)slot_div, (unsigned)host_div,
           (unsigned)card_div, (unsigned)s_sdmmc->clkdiv.val,
           (unsigned long)s_sdmmc->ctype.val,
           (unsigned long)s_sdmmc->clkena.val,
           (unsigned long)s_sdmmc->status.val);
    if (!esp_hosted_sdio_update_clock_registers(slot))
    {
        return false;
    }
    printf("[wclk1] ok CLKENA=0x%08lx STATUS=0x%08lx\r\n",
           (unsigned long)s_sdmmc->clkena.val,
           (unsigned long)s_sdmmc->status.val);

    s_clkdbg_step = 2U;
    sdmmc_ll_set_card_clock_div(s_sdmmc, slot, slot_div);
    if (!esp_hosted_sdio_update_clock_registers(slot))
    {
        return false;
    }
    printf("[wclk2] ok CLKDIV=0x%08lx CLKSRC=0x%08lx STATUS=0x%08lx\r\n",
           (unsigned long)s_sdmmc->clkdiv.val,
           (unsigned long)s_sdmmc->clksrc.val,
           (unsigned long)s_sdmmc->status.val);

    s_clkdbg_step = 3U;
    sdmmc_ll_enable_card_clock(s_sdmmc, slot, true);
    if (!esp_hosted_sdio_update_clock_registers(slot))
    {
        return false;
    }
    printf("[wclk3] ok CLKENA=0x%08lx STATUS=0x%08lx\r\n",
           (unsigned long)s_sdmmc->clkena.val,
           (unsigned long)s_sdmmc->status.val);

    sdmmc_ll_enable_card_clock_low_power(s_sdmmc, slot, true);
    return true;
}

static bool esp_hosted_sdio_send_cmd_locked(const esp_hosted_sdio_ctx_t *ctx,
                                     uint8_t cmd_idx,
                                     uint32_t arg,
                                     esp_hosted_sdio_resp_t resp_type,
                                     bool check_crc,
                                     bool send_init,
                                     uint32_t *resp0)
{
    sdmmc_hw_cmd_t cmd = {0};
    uint32_t raw = 0;
    uint32_t timeout_loops = ESP_HOSTED_SDIO_CMD_TIMEOUT_LOOPS;

    if (!esp_hosted_sdio_wait_cmd_taken(ESP_HOSTED_SDIO_CMD_TIMEOUT_LOOPS))
    {
        return false;
    }

    sdmmc_ll_clear_interrupt(s_sdmmc, UINT32_MAX);
    sdmmc_ll_set_command_arg(s_sdmmc, arg);

    cmd.cmd_index = cmd_idx;
    cmd.card_num = ctx->config.slot;
    cmd.wait_complete = 1;
    cmd.send_init = send_init ? 1U : 0U;
    cmd.response_expect = (resp_type == ESP_HOSTED_SDIO_RESP_NONE) ? 0U : 1U;
    cmd.response_long = (resp_type == ESP_HOSTED_SDIO_RESP_LONG) ? 1U : 0U;
    cmd.check_response_crc = (check_crc && (resp_type != ESP_HOSTED_SDIO_RESP_NONE)) ? 1U : 0U;
    cmd.use_hold_reg = 1;
    cmd.start_command = 1;
    sdmmc_ll_set_command(s_sdmmc, cmd);

    if (!esp_hosted_sdio_wait_cmd_taken(ESP_HOSTED_SDIO_CMD_TIMEOUT_LOOPS))
    {
        return false;
    }

    while (timeout_loops-- > 0U)
    {
        raw = esp_hosted_sdio_get_events();
        if ((raw & ESP_HOSTED_SDIO_CMD_ERR_MASK) != 0U)
        {
            sdmmc_ll_clear_interrupt(s_sdmmc, raw);
            return false;
        }
        if ((raw & SDMMC_LL_EVENT_CMD_DONE) != 0U)
        {
            sdmmc_ll_clear_interrupt(s_sdmmc, raw);
            if (resp0 != NULL && resp_type != ESP_HOSTED_SDIO_RESP_NONE)
            {
                *resp0 = s_sdmmc->resp[0];
            }
            return true;
        }
    }

    return false;
}

/*
 * Punto de acceso 2/3 al motor compartido: todo CMD (CMD0/3/5/7, CMD52 y los
 * CMD53 de sondeo) se lanza desde aqui.  El candado es reentrante, asi que los
 * ENVOLTORIOS de mas abajo (attach/read_reg/write_reg) pueden anidar uno
 * dentro de otro sin bloquearse a si mismos.
 */
static bool esp_hosted_sdio_send_cmd(const esp_hosted_sdio_ctx_t *ctx,
                                     uint8_t cmd_idx,
                                     uint32_t arg,
                                     esp_hosted_sdio_resp_t resp_type,
                                     bool check_crc,
                                     bool send_init,
                                     uint32_t *resp0)
{
    bool done;

    sdmmcBusLock();
    done = esp_hosted_sdio_send_cmd_locked(ctx, cmd_idx, arg, resp_type,
                                           check_crc, send_init, resp0);
    sdmmcBusUnlock();
    return done;
}

static bool esp_hosted_sdio_transfer_data_locked(const esp_hosted_sdio_ctx_t *ctx,
                                          uint8_t cmd_idx,
                                          uint32_t arg,
                                          uint8_t *buffer,
                                          uint32_t len,
                                          uint32_t block_size,
                                          bool is_write)
{
    sdmmc_hw_cmd_t cmd = {0};
    uint32_t raw = 0;
    uint32_t dma_raw = 0;
    uint8_t *dma_buf = s_ctx.dma_bounce;
    uint8_t *dma_buf_nc = (uint8_t *)CACHE_LL_L2MEM_NON_CACHE_ADDR(dma_buf);
    sdmmc_desc_t *desc = &s_ctx.dma_desc;
    sdmmc_desc_t *desc_nc = (sdmmc_desc_t *)CACHE_LL_L2MEM_NON_CACHE_ADDR(desc);
    bool data_done = false;
    bool dma_done = false;
    uint32_t timeout_loops = ESP_HOSTED_SDIO_DATA_TIMEOUT_LOOPS;

    if (len == 0U || len > ESP_HOSTED_SDIO_DMA_BUF_SIZE || block_size == 0U)
    {
        return false;
    }

    if (is_write)
    {
        memcpy(dma_buf_nc, buffer, len);
    }

    if (!esp_hosted_sdio_wait_data_idle())
    {
        return false;
    }

    if (!esp_hosted_sdio_wait_cmd_taken(ESP_HOSTED_SDIO_CMD_TIMEOUT_LOOPS))
    {
        return false;
    }

    sdmmc_ll_reset_fifo(s_sdmmc);
    sdmmc_ll_reset_dma(s_sdmmc);
    if (!esp_hosted_sdio_wait_reset_done())
    {
        return false;
    }

    sdmmc_ll_init_dma(s_sdmmc);
    sdmmc_ll_enable_dma(s_sdmmc, true);
    sdmmc_ll_clear_interrupt(s_sdmmc, UINT32_MAX);
    sdmmc_ll_clear_idsts_interrupt(s_sdmmc, UINT32_MAX);
    sdmmc_ll_set_block_size(s_sdmmc, block_size);
    sdmmc_ll_set_data_transfer_len(s_sdmmc, len);
    sdmmc_ll_set_command_arg(s_sdmmc, arg);

    memset(desc_nc, 0, sizeof(*desc_nc));
    desc_nc->first_descriptor = 1;
    desc_nc->last_descriptor = 1;
    desc_nc->second_address_chained = 1;
    desc_nc->end_of_ring = 1;
    desc_nc->owned_by_idmac = 1;
    desc_nc->buffer1_size = len;
    desc_nc->buffer1_ptr = dma_buf;
    desc_nc->next_desc_ptr = NULL;
    sdmmc_ll_set_desc_addr(s_sdmmc, (uint32_t)(uintptr_t)desc);
    sdmmc_ll_poll_demand(s_sdmmc);

    cmd.cmd_index = cmd_idx;
    cmd.card_num = ctx->config.slot;
    cmd.response_expect = 1;
    cmd.check_response_crc = 1;
    cmd.data_expected = 1;
    cmd.rw = is_write ? 1U : 0U;
    cmd.wait_complete = 1;
    cmd.use_hold_reg = 1;
    cmd.transfer_mode = 0;
    cmd.start_command = 1;
    sdmmc_ll_set_command(s_sdmmc, cmd);

    if (!esp_hosted_sdio_wait_cmd_taken(ESP_HOSTED_SDIO_CMD_TIMEOUT_LOOPS))
    {
        return false;
    }

    while (timeout_loops-- > 0U)
    {
        raw = esp_hosted_sdio_get_events();
        dma_raw = sdmmc_ll_get_idsts_interrupt_raw(s_sdmmc);

        if ((raw & ESP_HOSTED_SDIO_CMD_ERR_MASK) != 0U)
        {
            sdmmc_ll_clear_interrupt(s_sdmmc, raw);
            return false;
        }
        if ((raw & ESP_HOSTED_SDIO_DATA_ERR_MASK) != 0U)
        {
            sdmmc_ll_clear_interrupt(s_sdmmc, raw);
            return false;
        }
        if ((dma_raw & ESP_HOSTED_SDIO_DMA_ERR_MASK) != 0U)
        {
            sdmmc_ll_clear_idsts_interrupt(s_sdmmc, dma_raw);
            return false;
        }
        if ((dma_raw & ESP_HOSTED_SDIO_DMA_DONE_MASK) != 0U)
        {
            dma_done = true;
            sdmmc_ll_clear_idsts_interrupt(s_sdmmc, dma_raw);
        }
        if ((raw & SDMMC_LL_EVENT_CMD_DONE) != 0U)
        {
            sdmmc_ll_clear_interrupt(s_sdmmc, SDMMC_LL_EVENT_CMD_DONE);
        }
        if ((raw & SDMMC_LL_EVENT_DATA_OVER) != 0U)
        {
            data_done = true;
            sdmmc_ll_clear_interrupt(s_sdmmc, raw);
        }

        raw = (dma_raw >> ESP_HOSTED_SDIO_IDSTS_DMA_FSM_SHIFT) & ESP_HOSTED_SDIO_IDSTS_DMA_FSM_MASK;
        if (raw == ESP_HOSTED_SDIO_DMA_FSM_SUSPEND ||
            raw == ESP_HOSTED_SDIO_DMA_FSM_READ_REQ_WAIT ||
            raw == ESP_HOSTED_SDIO_DMA_FSM_WRITE_REQ_WAIT)
        {
            sdmmc_ll_poll_demand(s_sdmmc);
        }

        if (data_done && dma_done)
        {
            if (!is_write)
            {
                memcpy(buffer, dma_buf_nc, len);
            }
            return esp_hosted_sdio_wait_data_idle();
        }
    }

    return false;
}

/*
 * Punto de acceso 3/3 al motor compartido: la fase de DATOS de un CMD53.  Es
 * la que usa la IDMAC, la FIFO y los contadores de bloques, o sea el mismo
 * camino por el que Xinu mueve sectores de la microSD.  El candado se toma a
 * la entrada y se sujeta hasta el final: una transferencias a medias no se
 * puede intercalar con una lectura de disco.
 */
static bool esp_hosted_sdio_transfer_data(const esp_hosted_sdio_ctx_t *ctx,
                                          uint8_t cmd_idx,
                                          uint32_t arg,
                                          uint8_t *buffer,
                                          uint32_t len,
                                          uint32_t block_size,
                                          bool is_write)
{
    bool done;

    sdmmcBusLock();
    done = esp_hosted_sdio_transfer_data_locked(ctx, cmd_idx, arg, buffer, len,
                                                block_size, is_write);
    sdmmcBusUnlock();
    return done;
}

static uint32_t esp_hosted_sdio_calc_host_div(uint32_t clock_khz)
{
    uint32_t host_div;

    if (clock_khz == 0U)
    {
        return 8U;
    }

    host_div = (160000U + clock_khz - 1U) / clock_khz;
    if (host_div < 1U)
    {
        host_div = 1U;
    }
    if (host_div > 255U)
    {
        host_div = 255U;
    }

    return host_div;
}

/*
 * QUINTA DESVIACION (pulsos de reset).  El original maneja el CHIP_PU del C6
 * con la capa gpio_ll del SDK.  Este pad (GPIO54, activo por bajo) ya es del
 * driver de GPIO del kernel: W1 lo pulsa con gpio_set_dir()/gpio_write() y esa
 * es la ruta verificada en placa.  Dos librerias de GPIO sobre el mismo pad no
 * es un problema de direcciones (IO_MUX queda en 0x500E1000 en las dos, y el
 * simbolo IO_MUX del enlazado es absolute, o sea registros reales), pero si lo
 * es de dueno: gpio_set_dir() ademas fija OUT_SEL=256 y OEN_SEL=1 para que la
 * salida la gobierne GPIO_ENABLE, y eso es lo que Xinu espera.  Se usa la del
 * kernel, y el resto de gpio_ll se deja solo para lo que el kernel no expone.
 */
static void esp_hosted_sdio_setup_reset_pin(const esp_hosted_sdio_config_t *config)
{
    if (config->pin_reset < 0)
    {
        return;
    }

    gpio_set_dir((uint8_t)config->pin_reset, GPIO_OUTPUT);
    gpio_write((uint8_t)config->pin_reset, config->reset_active_low ? true : false);
    delay_ms(1U);
    gpio_write((uint8_t)config->pin_reset, config->reset_active_low ? false : true);
    delay_ms(config->reset_pulse_ms);
    gpio_write((uint8_t)config->pin_reset, config->reset_active_low ? true : false);
    delay_ms(config->post_reset_delay_ms);
}

static void esp_hosted_sdio_setup_iomux_pin(uint32_t gpio_num, bool use_pullup)
{
    gpio_ll_func_sel(&GPIO, gpio_num, SDMMC_SLOT0_FUNC);
    gpio_ll_pulldown_dis(&GPIO, gpio_num);
    gpio_ll_input_enable(&GPIO, gpio_num);
    gpio_ll_set_drive_capability(&GPIO, gpio_num, GPIO_DRIVE_CAP_3);
    if (use_pullup)
    {
        gpio_ll_pullup_en(&GPIO, gpio_num);
    }
    else
    {
        gpio_ll_pullup_dis(&GPIO, gpio_num);
    }
}

static bool esp_hosted_sdio_setup_slot0_pins(const esp_hosted_sdio_config_t *config)
{
    /* Bloqueo deliberado (misma razon que la desviacion 1): los pads del slot 0
     * son los de la microSD con la que Xinu arranca y monta FAT.  Esta funcion
     * los re-escribia con gpio_ll (funcion IOMUX nativa, pull-down apagada,
     * entrada habilitada, 40 mA y pull-up segun config), quitando por el camino
     * la configuracion que sdControllerInit() ya dej6 puesta y verificada.  El
     * dueno de esos pads es el driver SD del kernel, asi que aqui no se toca
     * ni un bit: se rechaza el slot 0 en seco. */
    ESP_HOSTED_SDIO_ERR("slot0 pads belong to the Xinu SD driver; use slot 1\n");
    return false;

    if ((config->pin_clk >= 0 && config->pin_clk != SDMMC_SLOT0_IOMUX_PIN_NUM_CLK) ||
        (config->pin_cmd >= 0 && config->pin_cmd != SDMMC_SLOT0_IOMUX_PIN_NUM_CMD) ||
        (config->pin_d0 >= 0 && config->pin_d0 != SDMMC_SLOT0_IOMUX_PIN_NUM_D0) ||
        (config->bus_width >= 4U && config->pin_d1 >= 0 && config->pin_d1 != SDMMC_SLOT0_IOMUX_PIN_NUM_D1) ||
        (config->bus_width >= 4U && config->pin_d2 >= 0 && config->pin_d2 != SDMMC_SLOT0_IOMUX_PIN_NUM_D2) ||
        (config->bus_width >= 4U && config->pin_d3 >= 0 && config->pin_d3 != SDMMC_SLOT0_IOMUX_PIN_NUM_D3))
    {
        ESP_HOSTED_SDIO_ERR("slot0 only supports native IOMUX pins\n");
        return false;
    }

    esp_hosted_sdio_setup_iomux_pin(SDMMC_SLOT0_IOMUX_PIN_NUM_CLK, false);
    esp_hosted_sdio_setup_iomux_pin(SDMMC_SLOT0_IOMUX_PIN_NUM_CMD, config->use_internal_pullups);
    esp_hosted_sdio_setup_iomux_pin(SDMMC_SLOT0_IOMUX_PIN_NUM_D0, config->use_internal_pullups);
    if (config->bus_width >= 4U)
    {
        esp_hosted_sdio_setup_iomux_pin(SDMMC_SLOT0_IOMUX_PIN_NUM_D1, config->use_internal_pullups);
        esp_hosted_sdio_setup_iomux_pin(SDMMC_SLOT0_IOMUX_PIN_NUM_D2, config->use_internal_pullups);
        esp_hosted_sdio_setup_iomux_pin(SDMMC_SLOT0_IOMUX_PIN_NUM_D3, config->use_internal_pullups);
    }

    return true;
}

/*
 * CUARTA DESVIACION (padres del slot 1).  La receta original usa
 * gpio_ll_output_enable() sobre CLK/CMD/D0..D3, que es el "GPIO_ENABLE" del
 * pad: pone el driver del P4 en la linea de forma PERMANENTE y el pad pasa a
 * ver el valor que el bloque SDHOST saca por la senal de datos, que en reposo
 * vale 0.  Resultado medido en placa: STATUS bit9 (data_busy, que es DAT0
 * invertida) clavado en 1 desde el primer instante, y con DAT0 en 0 cualquier
 * comando con wait_complete (incluida la orden interna de reloj) se aparca con
 * CMD_START=1 y congela el motor.
 *
 * Aqui se aplica la configuracion de pads que Xinu ya valid6 en ESTA placa en
 * W1 (sdioProbe.c -> sdioPadToMatrix): pad a la matriz GPIO, entrada
 * habilitada cuando hace falta, pull-up si el config lo pide, 40 mA y
 * pull-down apagado.  Lo que NO se hace es forzar el driver de salida del
 * pad; la salida la sigue gobernando la senal conectada por
 * esp_rom_gpio_connect_out_signal(), exactamente igual que en W1.
 */
static void esp_hosted_sdio_setup_matrix_pin(uint32_t gpio_num, bool output_en, bool input_en, bool pullup)
{
    volatile uint32_t *pad = (volatile uint32_t *)IO_MUX_PIN_REG(IO_MUX_BASE, gpio_num);
    uint32_t val;

    (void)output_en;    /* la salida la gobierna la senal conectada, no GPIO_ENABLE */
    (void)input_en;     /* ver comentario: el IE se pone siempre, tambien en CLK */

    /* DESVIACION LP_IOMUX (dueno del pad), y es la que congela el motor.
     * En el ESP32-P4 los pads GPIO0..GPIO15 tienen DOS duenos: el IO_MUX de
     * alto poder (el que toca el codigo de referencia) y LP_IOMUX, que decide
     * con su bit MUX_SEL quien manda el pad (1 = GPIO de bajo poder).  Medido
     * en ESTA placa despues del arranque:
     *      LP_PAD14 = 0x0000001a  (MUX_SEL=1, pull-up de bajo poder)
     *      LP_PAD15 = 0x0000041a  (MUX_SEL=1)
     * o sea que GPIO14 (DAT0 del C6) y GPIO15 (DAT1) los tiene el dominio de
     * bajo poder y NINGUNA de las escrituras del IO_MUX llega al pad: DAT0 se
     * queda clavado en 0, STATUS bit9 (data_busy) vale 1 desde el primer
     * instante y toda orden con wait_complete --incluida la update-clock
     * interna-- se aparca con CMD_START=1 y congela el bloque compartido con la
     * microSD.  Por eso `wifi up` moria en "transfer clock setup failed".  Los
     * pads 16..19 (CMD/CLK/D2/D3) no tienen registro LP --LP_IOMUX solo cubre
     * 0..15-- y por eso obedecian.  Se limpia el bit antes de tocar el IO_MUX,
     * igual que hace sdioPadToMatrix() en sdioProbe.c (validado en placa con
     * la sonda `wifi hold`). */
    if (gpio_num <= ESP_HOSTED_SDIO_LAST_LP_PAD)
    {
        volatile uint32_t *lp = (volatile uint32_t *)ESP_HOSTED_SDIO_LP_PAD_REG(gpio_num);

        *lp = *lp & ~ESP_HOSTED_SDIO_LP_MUX_SEL;
    }

    val = *pad;
    val &= ~(IO_MCU_SEL_MASK | IO_FUN_WPD | IO_FUN_WPU | IO_FUN_DRV_MASK);
    /* IO_FUN_IE siempre activo, como en W1 y como hace el propio camino del
     * slot 0 (esp_hosted_sdio_setup_iomux_pin llama a gpio_ll_input_enable sin
     * condiciones).  El pad del reloj tambien lleva entrada: es la via por la
     * que el bloque ve el card-clock de vuelta para el handshake de la
     * update-clock; sin IE el nivel de entrada del pad queda a 0. */
    val |= IO_MCU_SEL_GPIO | IO_FUN_IE | IO_FUN_DRV_40MA;
    if (pullup)
    {
        val |= IO_FUN_WPU;
    }
    *pad = val;
}

static bool esp_hosted_sdio_setup_slot1_pins(const esp_hosted_sdio_config_t *config)
{
    const sdmmc_slot_io_info_t *sig = &sdmmc_slot_gpio_sig[ESP_HOSTED_SDIO_SLOT1];

    if (config->pin_clk < 0 || config->pin_cmd < 0 || config->pin_d0 < 0)
    {
        return false;
    }
    if (config->bus_width >= 4U && (config->pin_d1 < 0 || config->pin_d2 < 0 || config->pin_d3 < 0))
    {
        return false;
    }

    esp_hosted_sdio_setup_matrix_pin((uint32_t)config->pin_clk, true, false, false);
    esp_rom_gpio_connect_out_signal((uint32_t)config->pin_clk, sig->clk, false, false);

    esp_hosted_sdio_setup_matrix_pin((uint32_t)config->pin_cmd, true, true, config->use_internal_pullups);
    esp_rom_gpio_connect_out_signal((uint32_t)config->pin_cmd, sig->cmd, false, false);
    esp_rom_gpio_connect_in_signal((uint32_t)config->pin_cmd, sig->cmd, false);

    esp_hosted_sdio_setup_matrix_pin((uint32_t)config->pin_d0, true, true, config->use_internal_pullups);
    esp_rom_gpio_connect_out_signal((uint32_t)config->pin_d0, sig->d0, false, false);
    esp_rom_gpio_connect_in_signal((uint32_t)config->pin_d0, sig->d0, false);

    if (config->bus_width >= 4U)
    {
        esp_hosted_sdio_setup_matrix_pin((uint32_t)config->pin_d1, true, true, config->use_internal_pullups);
        esp_rom_gpio_connect_out_signal((uint32_t)config->pin_d1, sig->d1, false, false);
        esp_rom_gpio_connect_in_signal((uint32_t)config->pin_d1, sig->d1, false);

        esp_hosted_sdio_setup_matrix_pin((uint32_t)config->pin_d2, true, true, config->use_internal_pullups);
        esp_rom_gpio_connect_out_signal((uint32_t)config->pin_d2, sig->d2, false, false);
        esp_rom_gpio_connect_in_signal((uint32_t)config->pin_d2, sig->d2, false);

        esp_hosted_sdio_setup_matrix_pin((uint32_t)config->pin_d3, true, true, config->use_internal_pullups);
        esp_rom_gpio_connect_out_signal((uint32_t)config->pin_d3, sig->d3, false, false);
        esp_rom_gpio_connect_in_signal((uint32_t)config->pin_d3, sig->d3, false);
    }

    return true;
}

static bool esp_hosted_sdio_setup_pins(const esp_hosted_sdio_config_t *config)
{
    if (config->slot == ESP_HOSTED_SDIO_SLOT0)
    {
        return esp_hosted_sdio_setup_slot0_pins(config);
    }
    if (config->slot == ESP_HOSTED_SDIO_SLOT1)
    {
        return esp_hosted_sdio_setup_slot1_pins(config);
    }
    return false;
}

static bool esp_hosted_sdio_hw_init(esp_hosted_sdio_ctx_t *ctx)
{
    sd_bus_width_t width = (ctx->config.bus_width >= 4U) ? SD_BUS_WIDTH_4_BIT : SD_BUS_WIDTH_1_BIT;

    /* SEPTIMA DESVIACION (orden): el original pulsa el reset y DESPUES configura
     * los pads.  Con los pads todavia sin pull-ups internas ni enrutado, el C6
     * arranca viendo CMD/DAT0 flotando y se queda con la linea clavada.  W1 hace
     * lo contrario (pads primero, reset despues) y asi el slave arranca sobre un
     * bus ya en reposo alto. */
    if (!esp_hosted_sdio_setup_pins(&ctx->config))
    {
        ESP_HOSTED_SDIO_ERR("pin setup failed\n");
        return false;
    }

    esp_hosted_sdio_setup_reset_pin(&ctx->config);

    sdmmc_ll_enable_bus_clock(s_sdmmc, true);
    sdmmc_ll_enable_sdio_pll(s_sdmmc, true);
    sdmmc_ll_select_clk_source(s_sdmmc, SDMMC_CLK_SRC_PLL160M);
    /*
     * UNICO DESVIO 1 del original (el otro esta en
     * esp_hosted_sdio_set_clock).  El bloque DesignWare es uno solo y sus dos
     * slots comparten el reloj:
     *
     *   - sdmmc_ll_set_clock_div() y sdmmc_ll_init_phase_delay() escriben el
     *     divisor GLOBAL sdio_ls (HP_SYS_CLKRST.peri_clk_ctrl02), que alimenta
     *     tambien al slot 0.  Ahi esta la microSD con la que Xinu arranca FAT
     *     y lee el filesystem, y sdControllerInit() ya la dej6 en 160 MHz / 4
     *     = 40 MHz.  No se toca: la velocidad que pide este driver se
     *     consigue por slot dentro de esp_hosted_sdio_set_clock().
     *   - sdmmc_ll_reset_controller() afirma CTRL.controller_reset, que
     *     reinicia la interfaz COMPLETA y deja al slot 0 sin controladora (su
     *     tarjeta qued6 en estado Transfer pero el host pierde la
     *     configuracion).  Tampoco se hace: el bloque ya esta vivo.
     *
     * El reset de FIFO y del DMA si se conserva: son las colas del camino de
     * datos, y todo este driver corre bajo sdmmcBusLock(), de modo que el
     * slot 0 no tiene ninguna transferencias en vuelo.
     */
    sdmmc_ll_reset_fifo(s_sdmmc);
    sdmmc_ll_reset_dma(s_sdmmc);
    if (!esp_hosted_sdio_wait_reset_done())
    {
        ESP_HOSTED_SDIO_ERR("controller reset timeout\n");
        esp_hosted_sdio_log_reset_state();
        return false;
    }

    sdmmc_ll_clear_interrupt(s_sdmmc, UINT32_MAX);
    sdmmc_ll_clear_idsts_interrupt(s_sdmmc, UINT32_MAX);
    sdmmc_ll_enable_dma(s_sdmmc, false);
    sdmmc_ll_enable_global_interrupt(s_sdmmc, false);
    sdmmc_ll_set_response_timeout(s_sdmmc, 0xFFU);
    sdmmc_ll_set_data_timeout(s_sdmmc, 0x00FFFFFFU);
    sdmmc_ll_set_card_width(s_sdmmc, ctx->config.slot, width);

    if (!esp_hosted_sdio_set_clock(ctx->config.slot,
                                   ESP_HOSTED_SDIO_PROBE_HOST_DIV,
                                   ESP_HOSTED_SDIO_PROBE_CARD_DIV))
    {
        ESP_HOSTED_SDIO_ERR("probe clock setup failed\n");
        return false;
    }

    if (!esp_hosted_sdio_send_cmd(ctx,
                                  ESP_HOSTED_SDIO_CMD0,
                                  0U,
                                  ESP_HOSTED_SDIO_RESP_NONE,
                                  false,
                                  true,
                                  NULL))
    {
        ESP_HOSTED_SDIO_ERR("CMD0 failed\n");
        return false;
    }

    return true;
}

static void esp_hosted_sdio_hw_deinit(const esp_hosted_sdio_ctx_t *ctx)
{
    sdmmc_ll_enable_dma(s_sdmmc, false);
    sdmmc_ll_enable_card_clock(s_sdmmc, ctx->config.slot, false);
    sdmmc_ll_clear_interrupt(s_sdmmc, UINT32_MAX);
    sdmmc_ll_clear_idsts_interrupt(s_sdmmc, UINT32_MAX);
    sdmmc_ll_enable_bus_clock(s_sdmmc, false);
}

static bool esp_hosted_sdio_cmd52_rw(uint8_t function,
                                     uint32_t reg,
                                     bool is_write,
                                     bool raw,
                                     uint8_t *data)
{
    uint32_t resp = 0;
    uint32_t arg;

    if (data == NULL)
    {
        return false;
    }

    arg = ((uint32_t)is_write << 31) |
          ((uint32_t)function << 28) |
          ((uint32_t)raw << 27) |
          ((reg & 0x1FFFFU) << 9) |
          (uint32_t)(*data);

    if (!esp_hosted_sdio_send_cmd(&s_ctx,
                                  ESP_HOSTED_SDIO_CMD52,
                                  arg,
                                  ESP_HOSTED_SDIO_RESP_SHORT,
                                  true,
                                  false,
                                  &resp))
    {
        if (function == ESP_HOSTED_SDIO_FUNC_1 &&
            reg == (ESP_HOSTED_SDIO_SLAVE_INT_RAW_REG & ESP_HOSTED_SDIO_ADDR_MASK) &&
            !is_write &&
            !raw)
        {
            s_cmd52_int_raw_fail_count++;
            if (s_cmd52_int_raw_fail_count == 1U ||
                (s_cmd52_int_raw_fail_count % 4096U) == 0U)
            {
                ESP_HOSTED_SDIO_ERR("CMD52 failed fn=%u reg=0x%05lx (%lu times)\n",
                                    (unsigned)function,
                                    (unsigned long)reg,
                                    (unsigned long)s_cmd52_int_raw_fail_count);
            }
        }
        else
        {
            ESP_HOSTED_SDIO_ERR("CMD52 failed fn=%u reg=0x%05lx write=%u raw=%u val=0x%02x\n",
                                (unsigned)function,
                                (unsigned long)reg,
                                (unsigned)is_write,
                                (unsigned)raw,
                                (unsigned)*data);
        }
        return false;
    }

    *data = (uint8_t)(resp & 0xFFU);
    return true;
}

static bool esp_hosted_sdio_cmd53_rw(uint8_t function,
                                     uint32_t addr,
                                     bool is_write,
                                     bool block_mode,
                                     bool op_code,
                                     uint8_t *data,
                                     uint32_t len)
{
    uint32_t count;
    uint32_t arg;
    uint32_t block_size;

    if (data == NULL || len == 0U || len > ESP_HOSTED_SDIO_DMA_BUF_SIZE)
    {
        return false;
    }

    count = block_mode ? (len / ESP_HOSTED_SDIO_BLOCK_SIZE) : (len & 0x1FFU);
    if (!block_mode && len == ESP_HOSTED_SDIO_BLOCK_SIZE)
    {
        count = 0U;
    }

    arg = ((uint32_t)is_write << 31) |
          ((uint32_t)function << 28) |
          ((uint32_t)block_mode << 27) |
          ((uint32_t)op_code << 26) |
          ((addr & 0x1FFFFU) << 9) |
          (count & 0x1FFU);

    block_size = block_mode ? ESP_HOSTED_SDIO_BLOCK_SIZE : len;
    return esp_hosted_sdio_transfer_data(&s_ctx,
                                         ESP_HOSTED_SDIO_CMD53,
                                         arg,
                                         data,
                                         len,
                                         block_size,
                                         is_write);
}

static bool esp_hosted_sdio_read_bytes(uint8_t function, uint32_t addr, uint8_t *data, uint32_t len)
{
    while (len >= ESP_HOSTED_SDIO_BLOCK_SIZE)
    {
        uint32_t chunk_blocks = esp_hosted_sdio_round_up_blocks(len);
        uint32_t chunk;

        if (chunk_blocks > (ESP_HOSTED_SDIO_DMA_BUF_SIZE / ESP_HOSTED_SDIO_BLOCK_SIZE))
        {
            chunk_blocks = ESP_HOSTED_SDIO_DMA_BUF_SIZE / ESP_HOSTED_SDIO_BLOCK_SIZE;
        }
        chunk = chunk_blocks * ESP_HOSTED_SDIO_BLOCK_SIZE;
        if (chunk > len)
        {
            chunk = (len / ESP_HOSTED_SDIO_BLOCK_SIZE) * ESP_HOSTED_SDIO_BLOCK_SIZE;
        }
        if (chunk == 0U)
        {
            break;
        }
        if (!esp_hosted_sdio_cmd53_rw(function, addr, false, true, true, data, chunk))
        {
            return false;
        }
        addr += chunk;
        data += chunk;
        len -= chunk;
    }

    while (len > 0U)
    {
        uint32_t chunk = (len > ESP_HOSTED_SDIO_BLOCK_SIZE) ? ESP_HOSTED_SDIO_BLOCK_SIZE : len;
        if (!esp_hosted_sdio_cmd53_rw(function, addr, false, false, true, data, chunk))
        {
            return false;
        }
        addr += chunk;
        data += chunk;
        len -= chunk;
    }

    return true;
}

static bool esp_hosted_sdio_write_bytes(uint8_t function, uint32_t addr, const uint8_t *data, uint32_t len)
{
    while (len >= ESP_HOSTED_SDIO_BLOCK_SIZE)
    {
        uint32_t chunk_blocks = esp_hosted_sdio_round_up_blocks(len);
        uint32_t chunk;

        if (chunk_blocks > (ESP_HOSTED_SDIO_DMA_BUF_SIZE / ESP_HOSTED_SDIO_BLOCK_SIZE))
        {
            chunk_blocks = ESP_HOSTED_SDIO_DMA_BUF_SIZE / ESP_HOSTED_SDIO_BLOCK_SIZE;
        }
        chunk = chunk_blocks * ESP_HOSTED_SDIO_BLOCK_SIZE;
        if (chunk > len)
        {
            chunk = (len / ESP_HOSTED_SDIO_BLOCK_SIZE) * ESP_HOSTED_SDIO_BLOCK_SIZE;
        }
        if (chunk == 0U)
        {
            break;
        }
        if (!esp_hosted_sdio_cmd53_rw(function, addr, true, true, true, (uint8_t *)data, chunk))
        {
            return false;
        }
        addr += chunk;
        data += chunk;
        len -= chunk;
    }

    while (len > 0U)
    {
        uint32_t chunk = (len > ESP_HOSTED_SDIO_BLOCK_SIZE) ? ESP_HOSTED_SDIO_BLOCK_SIZE : len;
        if (!esp_hosted_sdio_cmd53_rw(function, addr, true, false, true, (uint8_t *)data, chunk))
        {
            return false;
        }
        addr += chunk;
        data += chunk;
        len -= chunk;
    }

    return true;
}

static bool esp_hosted_sdio_write_padded_blocks(uint8_t function,
                                                uint32_t addr,
                                                const uint8_t *data,
                                                uint32_t actual_len)
{
    uint32_t padded_len;

    if (data == NULL || actual_len == 0U)
    {
        return false;
    }

    padded_len = esp_hosted_sdio_round_up_blocks(actual_len) * ESP_HOSTED_SDIO_BLOCK_SIZE;
    if (padded_len > sizeof(s_ctx.tx_frame))
    {
        return false;
    }

    if (padded_len != actual_len)
    {
        memset(s_ctx.tx_frame + actual_len, 0, padded_len - actual_len);
    }

    return esp_hosted_sdio_cmd53_rw(function,
                                    addr,
                                    true,
                                    true,
                                    true,
                                    (uint8_t *)data,
                                    padded_len);
}

static bool esp_hosted_sdio_read_reg_bytes(uint32_t reg, uint8_t *data, uint32_t len)
{
    uint32_t i;

    if (data == NULL || len == 0U)
    {
        return false;
    }

    reg &= ESP_HOSTED_SDIO_ADDR_MASK;
    if (len > 1U)
    {
        /*
         * Match esp-hosted reference behavior: use SDIO byte-stream register
         * access for multi-byte reads, then fallback to CMD52 loop if needed.
         */
        if (esp_hosted_sdio_cmd53_rw(ESP_HOSTED_SDIO_FUNC_1, reg, false, false, true, data, len))
        {
            return true;
        }
    }

    for (i = 0; i < len; i++)
    {
        uint8_t value = 0U;
        if (!esp_hosted_sdio_cmd52_rw(ESP_HOSTED_SDIO_FUNC_1, reg + i, false, false, &value))
        {
            return false;
        }
        data[i] = value;
    }

    return true;
}

static bool esp_hosted_sdio_write_reg_bytes(uint32_t reg, const uint8_t *data, uint32_t len)
{
    uint32_t i;

    if (data == NULL || len == 0U)
    {
        return false;
    }

    reg &= ESP_HOSTED_SDIO_ADDR_MASK;
    if (len > 1U)
    {
        /*
         * Prefer SDIO byte-stream register writes for multi-byte accesses.
         */
        if (esp_hosted_sdio_cmd53_rw(ESP_HOSTED_SDIO_FUNC_1, reg, true, false, true, (uint8_t *)data, len))
        {
            return true;
        }
    }

    for (i = 0; i < len; i++)
    {
        uint8_t value = data[i];
        if (!esp_hosted_sdio_cmd52_rw(ESP_HOSTED_SDIO_FUNC_1, reg + i, true, false, &value))
        {
            return false;
        }
    }

    return true;
}

static bool esp_hosted_sdio_set_block_size(uint8_t function, uint16_t value)
{
    uint32_t offset = (uint32_t)function * 0x100U;
    uint8_t low = (uint8_t)(value & 0xFFU);
    uint8_t high = (uint8_t)(value >> 8);

    if (!esp_hosted_sdio_cmd52_rw(ESP_HOSTED_SDIO_FUNC_0,
                                  offset + ESP_HOSTED_SDIO_CCCR_BLKSIZEL,
                                  true,
                                  false,
                                  &low))
    {
        return false;
    }
    if (!esp_hosted_sdio_cmd52_rw(ESP_HOSTED_SDIO_FUNC_0,
                                  offset + ESP_HOSTED_SDIO_CCCR_BLKSIZEH,
                                  true,
                                  false,
                                  &high))
    {
        return false;
    }
    return true;
}

static bool esp_hosted_sdio_get_tx_buffer_num(uint32_t *tx_num)
{
    uint32_t token = 0;

    if (tx_num == NULL)
    {
        return false;
    }
    if (!esp_hosted_sdio_read_reg_bytes(ESP_HOSTED_SDIO_SLAVE_TOKEN_RDATA,
                                        (uint8_t *)&token,
                                        sizeof(token)))
    {
        return false;
    }

    token = (token >> 16) & ESP_HOSTED_SDIO_TX_BUF_MASK;
    token = (token + ESP_HOSTED_SDIO_TX_BUF_MAX - s_ctx.tx_buf_count) % ESP_HOSTED_SDIO_TX_BUF_MAX;
    *tx_num = token;
    return true;
}

static bool esp_hosted_sdio_get_len_from_slave(uint32_t *rx_size)
{
    uint32_t len = 0;
    uint32_t temp;

    if (rx_size == NULL)
    {
        return false;
    }
    if (!esp_hosted_sdio_read_reg_bytes(ESP_HOSTED_SDIO_SLAVE_PACKET_LEN_REG,
                                        (uint8_t *)&len,
                                        sizeof(len)))
    {
        return false;
    }

    len &= ESP_HOSTED_SDIO_SLAVE_LEN_MASK;
    if (len >= s_ctx.rx_byte_count)
    {
        len = (len + ESP_HOSTED_SDIO_RX_BYTE_MAX - s_ctx.rx_byte_count) % ESP_HOSTED_SDIO_RX_BYTE_MAX;
    }
    else
    {
        temp = ESP_HOSTED_SDIO_RX_BYTE_MAX - s_ctx.rx_byte_count;
        len = temp + len;
    }

    *rx_size = len;
    return true;
}

static bool esp_hosted_sdio_generate_slave_intr(uint8_t intr_no)
{
    uint8_t intr_mask = BIT(intr_no);
    return esp_hosted_sdio_write_reg_bytes(ESP_HOSTED_SDIO_HOST_TO_SLAVE_INTR, &intr_mask, sizeof(intr_mask));
}

static bool esp_hosted_sdio_card_init(esp_hosted_sdio_ctx_t *ctx)
{
    uint32_t resp = 0;
    uint8_t ioe = 0;
    uint8_t ior = 0;
    uint8_t ie = 0;
    uint8_t bus_width = 0;
    uint16_t i;

    if (!esp_hosted_sdio_send_cmd(ctx,
                                  ESP_HOSTED_SDIO_CMD5,
                                  0U,
                                  ESP_HOSTED_SDIO_RESP_SHORT,
                                  false,
                                  false,
                                  &resp))
    {
        ESP_HOSTED_SDIO_ERR("CMD5 probe failed\n");
        return false;
    }

    for (i = 0; i < ESP_HOSTED_SDIO_IO_READY_RETRIES; i++)
    {
        if (!esp_hosted_sdio_send_cmd(ctx,
                                      ESP_HOSTED_SDIO_CMD5,
                                      ESP_HOSTED_SDIO_CMD5_OCR_ARG,
                                      ESP_HOSTED_SDIO_RESP_SHORT,
                                      false,
                                      false,
                                      &resp))
        {
            return false;
        }
        if ((resp & ESP_HOSTED_SDIO_R4_READY) != 0U)
        {
            break;
        }
        delay_ms(1U);
    }
    if (i >= ESP_HOSTED_SDIO_IO_READY_RETRIES)
    {
        ESP_HOSTED_SDIO_ERR("CMD5 ready timeout\n");
        return false;
    }

    if (!esp_hosted_sdio_send_cmd(ctx,
                                  ESP_HOSTED_SDIO_CMD3,
                                  0U,
                                  ESP_HOSTED_SDIO_RESP_SHORT,
                                  true,
                                  false,
                                  &resp))
    {
        ESP_HOSTED_SDIO_ERR("CMD3 failed\n");
        return false;
    }
    ctx->rca = (uint16_t)(resp >> ESP_HOSTED_SDIO_RCA_SHIFT);
    if (ctx->rca == 0U)
    {
        ESP_HOSTED_SDIO_ERR("invalid RCA\n");
        return false;
    }

    if (!esp_hosted_sdio_send_cmd(ctx,
                                  ESP_HOSTED_SDIO_CMD7,
                                  ((uint32_t)ctx->rca) << ESP_HOSTED_SDIO_RCA_SHIFT,
                                  ESP_HOSTED_SDIO_RESP_SHORT,
                                  true,
                                  false,
                                  NULL))
    {
        ESP_HOSTED_SDIO_ERR("CMD7 failed\n");
        return false;
    }

    if (!esp_hosted_sdio_cmd52_rw(ESP_HOSTED_SDIO_FUNC_0,
                                  ESP_HOSTED_SDIO_CCCR_FN_ENABLE,
                                  false,
                                  false,
                                  &ioe))
    {
        return false;
    }
    ioe |= ESP_HOSTED_SDIO_CCCR_FN1_EN;
    if (!esp_hosted_sdio_cmd52_rw(ESP_HOSTED_SDIO_FUNC_0,
                                  ESP_HOSTED_SDIO_CCCR_FN_ENABLE,
                                  true,
                                  false,
                                  &ioe))
    {
        return false;
    }
    if (!esp_hosted_sdio_cmd52_rw(ESP_HOSTED_SDIO_FUNC_0,
                                  ESP_HOSTED_SDIO_CCCR_FN_ENABLE,
                                  false,
                                  false,
                                  &ioe))
    {
        return false;
    }
    ESP_HOSTED_SDIO_LOG("[esp_hosted_sdio] fn_enable=0x%02x\n", (unsigned)ioe);

    for (i = 0; i < ESP_HOSTED_SDIO_IO_READY_RETRIES; i++)
    {
        if (!esp_hosted_sdio_cmd52_rw(ESP_HOSTED_SDIO_FUNC_0,
                                      ESP_HOSTED_SDIO_CCCR_FN_READY,
                                      false,
                                      false,
                                      &ior))
        {
            delay_ms(10U);
            continue;
        }
        if ((ior & ESP_HOSTED_SDIO_CCCR_FN1_EN) != 0U)
        {
            ESP_HOSTED_SDIO_LOG("[esp_hosted_sdio] fn_ready=0x%02x\n", (unsigned)ior);
            break;
        }
        delay_ms(10U);
    }
    if (i >= ESP_HOSTED_SDIO_IO_READY_RETRIES)
    {
        ESP_HOSTED_SDIO_ERR("function 1 ready timeout\n");
        return false;
    }

    if (!esp_hosted_sdio_cmd52_rw(ESP_HOSTED_SDIO_FUNC_0,
                                  ESP_HOSTED_SDIO_CCCR_INT_ENABLE,
                                  false,
                                  false,
                                  &ie))
    {
        return false;
    }
    ie |= ESP_HOSTED_SDIO_CCCR_INT_MASTER_EN | ESP_HOSTED_SDIO_CCCR_FN1_EN;
    if (!esp_hosted_sdio_cmd52_rw(ESP_HOSTED_SDIO_FUNC_0,
                                  ESP_HOSTED_SDIO_CCCR_INT_ENABLE,
                                  true,
                                  false,
                                  &ie))
    {
        return false;
    }

    if (!esp_hosted_sdio_cmd52_rw(ESP_HOSTED_SDIO_FUNC_0,
                                  ESP_HOSTED_SDIO_CCCR_BUS_WIDTH,
                                  false,
                                  false,
                                  &bus_width))
    {
        return false;
    }
    if (ctx->config.bus_width >= 4U)
    {
        bus_width = (uint8_t)((bus_width & ~0x3U) | ESP_HOSTED_SDIO_CCCR_BUS_WIDTH_4);
        if (!esp_hosted_sdio_cmd52_rw(ESP_HOSTED_SDIO_FUNC_0,
                                      ESP_HOSTED_SDIO_CCCR_BUS_WIDTH,
                                      true,
                                      false,
                                      &bus_width))
        {
            return false;
        }
        sdmmc_ll_set_card_width(s_sdmmc, ctx->config.slot, SD_BUS_WIDTH_4_BIT);
    }
    else
    {
        bus_width = (uint8_t)(bus_width & ~0x3U);
        if (!esp_hosted_sdio_cmd52_rw(ESP_HOSTED_SDIO_FUNC_0,
                                      ESP_HOSTED_SDIO_CCCR_BUS_WIDTH,
                                      true,
                                      false,
                                      &bus_width))
        {
            return false;
        }
        sdmmc_ll_set_card_width(s_sdmmc, ctx->config.slot, SD_BUS_WIDTH_1_BIT);
    }

    if (!esp_hosted_sdio_set_block_size(ESP_HOSTED_SDIO_FUNC_0, ESP_HOSTED_SDIO_BLOCK_SIZE) ||
        !esp_hosted_sdio_set_block_size(ESP_HOSTED_SDIO_FUNC_1, ESP_HOSTED_SDIO_BLOCK_SIZE))
    {
        ESP_HOSTED_SDIO_ERR("block size config failed\n");
        return false;
    }

    if (!esp_hosted_sdio_set_clock(ctx->config.slot,
                                   esp_hosted_sdio_calc_host_div(ctx->config.clock_khz),
                                   0U))
    {
        ESP_HOSTED_SDIO_ERR("transfer clock setup failed\n");
        return false;
    }

    return true;
}

static bool esp_hosted_sdio_is_zero_padding(const uint8_t *data, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i++)
    {
        if (data[i] != 0U)
        {
            return false;
        }
    }
    return true;
}

static void esp_hosted_sdio_submit_rx_frame(const esp_hosted_sdio_payload_header_t *header,
                                            const uint8_t *payload,
                                            uint16_t len)
{
    esp_hosted_frame_info_t info = {0};

    info.if_type = (esp_hosted_if_t)header->if_type;
    info.if_num = header->if_num;
    info.flags = header->flags;
    info.payload_len = len;
    info.payload_offset = header->offset;
    info.checksum = header->checksum;
    info.seq_num = header->seq_num;
    info.throttle_cmd = header->throttle_cmd;
    info.hci_pkt_type = header->hci_pkt_type;
    info.priv_pkt_type = header->priv_pkt_type;

    if (info.if_type == ESP_HOSTED_IF_SERIAL ||
        info.if_type == ESP_HOSTED_IF_HCI ||
        info.if_type == ESP_HOSTED_IF_PRIV)
    {
        ESP_HOSTED_SDIO_LOG("[esp_hosted_sdio] rx if=%u ifnum=%u len=%u flags=0x%02x seq=%u aux=0x%02x\n",
                            (unsigned)info.if_type,
                            (unsigned)info.if_num,
                            (unsigned)len,
                            (unsigned)info.flags,
                            (unsigned)info.seq_num,
                            (unsigned)((info.if_type == ESP_HOSTED_IF_HCI) ? info.hci_pkt_type : info.priv_pkt_type));

        /* Volcado de los bytes que suben del esclavo.  Es el equivalente del
         * dump de tx: permite ver el TLV/protobuf EXACTO que el NCP escribe, y
         * con ello su esquema real (numeros de campo del sobre Rpc) en lugar de
         * suponerlo.  Se limita a 40 bytes por trama. */
        {
            uint32_t rx_i;
            uint32_t rx_n = ((uint32_t)len < 40U) ? (uint32_t)len : 40U;

            ESP_HOSTED_SDIO_LOG("[esp_hosted_sdio] rx buf(%u):", (unsigned)len);
            for (rx_i = 0U; rx_i < rx_n; rx_i++)
                ESP_HOSTED_SDIO_LOG(" %02x", payload[rx_i]);
            ESP_HOSTED_SDIO_LOG("\n");
        }
    }

    (void)esp_hosted_rx_submit(&info, payload, len);
}

static bool esp_hosted_sdio_drain_rx_bytes(uint32_t total_len)
{
    uint32_t addr = ESP_HOSTED_SDIO_SLAVE_CMD53_END_ADDR - total_len;

    while (total_len > 0U)
    {
        uint32_t chunk = total_len;
        if (chunk > sizeof(s_ctx.frame_buf))
        {
            chunk = sizeof(s_ctx.frame_buf);
        }
        if (!esp_hosted_sdio_read_bytes(ESP_HOSTED_SDIO_FUNC_1, addr, s_ctx.frame_buf, chunk))
        {
            return false;
        }
        addr += chunk;
        total_len -= chunk;
    }

    return true;
}

static void esp_hosted_sdio_parse_rx_frames(uint8_t *data, uint32_t len)
{
    uint32_t cursor = 0U;

    while (cursor + sizeof(esp_hosted_sdio_payload_header_t) <= len)
    {
        esp_hosted_sdio_payload_header_t *header = (esp_hosted_sdio_payload_header_t *)(data + cursor);
        uint32_t payload_offset = header->offset;
        uint32_t payload_len = header->len;
        uint32_t frame_len = payload_offset + payload_len;

        if (payload_len == 0U && esp_hosted_sdio_is_zero_padding(data + cursor, len - cursor))
        {
            return;
        }

        if (frame_len == 0U ||
            payload_offset != sizeof(esp_hosted_sdio_payload_header_t) ||
            cursor + frame_len > len ||
            payload_len > ESP_HOSTED_RX_PAYLOAD_MAX)
        {
            if ((header->flags & ESP_HOSTED_SDIO_MORE_FRAGMENT) != 0U)
            {
                ESP_HOSTED_SDIO_ERR("fragmented rx frame is not reassembled yet\n");
            }
            else
            {
                ESP_HOSTED_SDIO_ERR("invalid rx frame len=%u offset=%u total=%u\n",
                                    (unsigned)payload_len,
                                    (unsigned)payload_offset,
                                    (unsigned)len);
            }
            return;
        }

        esp_hosted_sdio_submit_rx_frame(header, data + cursor + payload_offset, (uint16_t)payload_len);
        cursor += frame_len;
    }
}

static size_t esp_hosted_sdio_transport_tx(const esp_hosted_frame_info_t *info,
                                           const void *payload,
                                           size_t len,
                                           void *ctx)
{
    esp_hosted_sdio_ctx_t *sdio = (esp_hosted_sdio_ctx_t *)ctx;
    esp_hosted_sdio_payload_header_t *header;
    uint32_t tx_buf_num = 0;
    uint32_t frame_len;
    uint32_t buf_needed;

    if (sdio == NULL || info == NULL || (!sdio->attached) || (payload == NULL && len != 0U))
    {
        return 0U;
    }
    if (len > ESP_HOSTED_RX_PAYLOAD_MAX)
    {
        return 0U;
    }
    if (sdio->tx_throttled &&
        (info->if_type == ESP_HOSTED_IF_STA || info->if_type == ESP_HOSTED_IF_AP || info->if_type == ESP_HOSTED_IF_ETH))
    {
        return 0U;
    }

    header = (esp_hosted_sdio_payload_header_t *)sdio->tx_frame;
    memset(header, 0, sizeof(*header));
    header->if_type = (uint8_t)info->if_type;
    header->if_num = info->if_num;
    header->flags = info->flags;
    header->len = (uint16_t)len;
    header->offset = (uint16_t)sizeof(*header);
    header->checksum = 0U;
    header->seq_num = info->seq_num;
    header->throttle_cmd = info->throttle_cmd;
    if (info->if_type == ESP_HOSTED_IF_HCI)
    {
        header->hci_pkt_type = info->hci_pkt_type;
    }
    /* DESVIACION DEL ORIGINAL (medida en placa).  hci_pkt_type y priv_pkt_type
     * son el MISMO byte de la cabecera de 12 (la union del struct), y el codigo
     * de referencia solo lo rellena cuando el canal es HCI.  Para PRIV deja el
     * 0 del memset, de modo que la respuesta del host al EVENT_INIT del esclavo
     * --que debe ir marcada con priv_pkt_type 0x33=EVENT, igual que la que él
     * sube-- salia con 0x00 y el slave no la podia clasificar.  Medido:
     *      tx if=5 ifnum=0 len=17 ... aux=0x00   (aux es el byte de la union)
     * Se rellena tambien en PRIV; el camino RX ya lo leia al reves
     * (info.priv_pkt_type = header->priv_pkt_type) y por eso el C6 si nos lo
     * llego a marcar 0x33. */
    if (info->if_type == ESP_HOSTED_IF_PRIV)
    {
        header->priv_pkt_type = info->priv_pkt_type;
    }

    if (len != 0U)
    {
        memcpy(sdio->tx_frame + sizeof(*header), payload, len);
    }
    frame_len = (uint32_t)(sizeof(*header) + len);
    if (frame_len > sizeof(sdio->tx_frame))
    {
        ESP_HOSTED_SDIO_ERR("tx frame too large len=%u max=%u\n",
                            (unsigned)frame_len,
                            (unsigned)sizeof(sdio->tx_frame));
        return 0U;
    }

    header->checksum = esp_hosted_sdio_compute_checksum(sdio->tx_frame, frame_len);

    if (info->if_type == ESP_HOSTED_IF_SERIAL ||
        info->if_type == ESP_HOSTED_IF_HCI ||
        info->if_type == ESP_HOSTED_IF_PRIV)
    {
        ESP_HOSTED_SDIO_LOG("[esp_hosted_sdio] tx if=%u ifnum=%u len=%u flags=0x%02x seq=%u aux=0x%02x\n",
                            (unsigned)info->if_type,
                            (unsigned)info->if_num,
                            (unsigned)len,
                            (unsigned)info->flags,
                            (unsigned)info->seq_num,
                            (unsigned)((info->if_type == ESP_HOSTED_IF_HCI) ? header->hci_pkt_type : header->priv_pkt_type));
        /* Volcado de los bytes que se van a escribir en el FIFO del esclavo
         * (cabecera de 12 + payload), ya con la suma de verificacion calculada.
         * El esclavo consume nuestros creditos pero no devuelve nada por el
         * canal SERIAL, y desde el host no hay forma de saber si el rechazo es
         * de transporte (bytes corruptos / checksum) o de contenido (TLV +
         * protobuf).  Aqui queda registrado lo que el C6 tendria que estar
         * leyendo, byte a byte.  Detras de ESP_HOSTED_SDIO_DEBUG porque con la
         * pila TCP/IP encima esto imprime en cada trama y vuelve ilegible la
         * consola (y ademas frena el sondeo del bus). */
#if ESP_HOSTED_SDIO_DEBUG
        {
            uint32_t dump_i;
            uint32_t dump_n = (frame_len < 40U) ? frame_len : 40U;

            printf("[esp_hosted_sdio] tx buf(%u):", (unsigned)frame_len);
            for (dump_i = 0U; dump_i < dump_n; dump_i++)
            {
                printf(" %02x", sdio->tx_frame[dump_i]);
            }
            printf("\n");
        }
#endif
    }

    buf_needed = (frame_len + ESP_HOSTED_SDIO_RX_BUF_WINDOW - 1U) / ESP_HOSTED_SDIO_RX_BUF_WINDOW;
    if (!esp_hosted_sdio_get_tx_buffer_num(&tx_buf_num) || tx_buf_num < buf_needed)
    {
        return 0U;
    }

    if (!esp_hosted_sdio_write_padded_blocks(ESP_HOSTED_SDIO_FUNC_1,
                                             ESP_HOSTED_SDIO_SLAVE_CMD53_END_ADDR - frame_len,
                                             sdio->tx_frame,
                                             frame_len))
    {
        ESP_HOSTED_SDIO_ERR("tx write failed len=%u\n", (unsigned)frame_len);
        return 0U;
    }

    sdio->tx_buf_count = (sdio->tx_buf_count + buf_needed) % ESP_HOSTED_SDIO_TX_BUF_MAX;
    return len;
}

static void esp_hosted_sdio_transport_poll(void *ctx)
{
    esp_hosted_sdio_ctx_t *sdio = (esp_hosted_sdio_ctx_t *)ctx;
    uint32_t interrupts = 0;
    uint32_t len_from_slave = 0;
    uint32_t poll_budget = 4U;

    if (sdio == NULL || !sdio->attached)
    {
        return;
    }

    while (poll_budget-- > 0U)
    {
        if (!esp_hosted_sdio_read_reg_bytes(ESP_HOSTED_SDIO_SLAVE_INT_RAW_REG,
                                            (uint8_t *)&interrupts,
                                            sizeof(interrupts)))
        {
            return;
        }
        if (interrupts == 0U)
        {
            return;
        }

        (void)esp_hosted_sdio_write_reg_bytes(ESP_HOSTED_SDIO_SLAVE_INT_CLR_REG,
                                              (const uint8_t *)&interrupts,
                                              sizeof(interrupts));

        if ((interrupts & ESP_HOSTED_SDIO_INT_START_THROTTLE) != 0U)
        {
            sdio->tx_throttled = true;
        }
        if ((interrupts & ESP_HOSTED_SDIO_INT_STOP_THROTTLE) != 0U)
        {
            sdio->tx_throttled = false;
        }
        if ((interrupts & ESP_HOSTED_SDIO_INT_NEW_PACKET) == 0U)
        {
            continue;
        }

        if (!esp_hosted_sdio_get_len_from_slave(&len_from_slave) || len_from_slave == 0U)
        {
            return;
        }

        if (len_from_slave > sizeof(sdio->frame_buf))
        {
            ESP_HOSTED_SDIO_ERR("rx frame too large len=%u, dropping\n", (unsigned)len_from_slave);
            if (!esp_hosted_sdio_drain_rx_bytes(len_from_slave))
            {
                return;
            }
            sdio->rx_byte_count = (sdio->rx_byte_count + len_from_slave) % ESP_HOSTED_SDIO_RX_BYTE_MAX;
            continue;
        }

        if (!esp_hosted_sdio_read_bytes(ESP_HOSTED_SDIO_FUNC_1,
                                        ESP_HOSTED_SDIO_SLAVE_CMD53_END_ADDR - len_from_slave,
                                        sdio->frame_buf,
                                        len_from_slave))
        {
            ESP_HOSTED_SDIO_ERR("rx read failed len=%u\n", (unsigned)len_from_slave);
            return;
        }

        sdio->rx_byte_count = (sdio->rx_byte_count + len_from_slave) % ESP_HOSTED_SDIO_RX_BYTE_MAX;
        esp_hosted_sdio_parse_rx_frames(sdio->frame_buf, len_from_slave);
    }
}

void esp_hosted_sdio_get_default_config(esp_hosted_sdio_config_t *config)
{
    if (config == NULL)
    {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->slot = ESP_HOSTED_SDIO_SLOT1;
    /* OCTAVA DESVIACION (ancho de bus en el arranque).  El original pone
     * CTYPE a 4 bits antes de hablar con la tarjeta.  W1 deja el slot 1 en
     * 1 bit durante toda la identificacion (CTYPE=0x00000001: solo el slot 0
     * en 4 bits) y con eso el reloj de 400 kHz y el CMD5 salen bien.  Con
     * CTYPE en 4 bits y la tarjeta aun en modo 1-bit, el MC del host ve las
     * lineas D1..D3 sin driver y el data_busy no se suelta: es justo el punto
     * donde la update-clock se aparca.  El ancho se_negocia despues, por
     * registro del propio slave, como hace el flujo esp_hosted. */
    config->bus_width = 1U;
    config->clock_khz = 25000U;
    config->pin_clk = 18;
    config->pin_cmd = 19;
    config->pin_d0 = 14;
    config->pin_d1 = 15;
    config->pin_d2 = 16;
    config->pin_d3 = 17;
    config->pin_reset = 54;
    config->reset_active_low = true;
    /* SEXTA DESVIACION (valores de placa, no de la referencia).  Dos datos
     * medidos con W1 en ESTA placa:
     *   - use_internal_pullups = true: en esta Waveshare no hay pull-ups
     *     externas en CMD/D0 del slot 1.  Sin pull-up, con el C6 aun sin
     *     responder, DAT0 flota a bajo y STATUS bit9 (data_busy) queda en 1;
     *     como la orden interna de update-clock lleva wait_complete, el motor
     *     aparca el comando (CMD_START no se auto-limpia) y se congela el
     *     bloque COMPARTIDO.  W1 siempre encendio sus pull-ups internas y por
     *     eso el slot 1 arrancaba.
     *   - reset_pulse_ms = 100: W1 baja CHIP_PU 100 ms; con los 10 ms del
     *     original el C6 no quedaba bien reiniciado. */
    config->reset_pulse_ms = 100U;
    config->post_reset_delay_ms = 1500U;
    config->use_internal_pullups = true;
}

bool esp_hosted_sdio_attach(const esp_hosted_sdio_config_t *config)
{
    esp_hosted_sdio_config_t cfg;
    uint32_t attempt;

    if (config == NULL)
    {
        esp_hosted_sdio_get_default_config(&cfg);
        config = &cfg;
    }

    if (s_ctx.attached)
    {
        esp_hosted_sdio_detach();
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_cmd52_int_raw_fail_count = 0U;
    s_ctx.config = *config;

    if (s_ctx.config.bus_width != 1U && s_ctx.config.bus_width != 4U)
    {
        ESP_HOSTED_SDIO_ERR("only 1-bit and 4-bit SDIO are supported\n");
        return false;
    }

    for (attempt = 0; attempt < ESP_HOSTED_SDIO_ATTACH_RETRIES; attempt++)
    {
        ESP_HOSTED_SDIO_LOG("[esp_hosted_sdio] attach attempt=%lu slot=%u width=%u clk=%lu kHz pullups=%u\n",
                            (unsigned long)(attempt + 1U),
                            (unsigned)s_ctx.config.slot,
                            (unsigned)s_ctx.config.bus_width,
                            (unsigned long)s_ctx.config.clock_khz,
                            (unsigned)s_ctx.config.use_internal_pullups);

        if (!esp_hosted_sdio_hw_init(&s_ctx))
        {
            ESP_HOSTED_SDIO_ERR("host init failed attempt=%lu\n", (unsigned long)(attempt + 1U));
            esp_hosted_sdio_hw_deinit(&s_ctx);
            delay_ms(20U);
            continue;
        }
        if (!esp_hosted_sdio_card_init(&s_ctx))
        {
            ESP_HOSTED_SDIO_ERR("card init failed attempt=%lu\n", (unsigned long)(attempt + 1U));
            esp_hosted_sdio_hw_deinit(&s_ctx);
            delay_ms(20U);
            continue;
        }
        break;
    }
    if (attempt >= ESP_HOSTED_SDIO_ATTACH_RETRIES)
    {
        return false;
    }

    if (esp_hosted_is_initialized())
    {
        if (!esp_hosted_set_transport(&s_transport))
        {
            ESP_HOSTED_SDIO_ERR("failed to set transport\n");
            esp_hosted_sdio_hw_deinit(&s_ctx);
            return false;
        }
    }
    else if (!esp_hosted_init(&s_transport))
    {
        ESP_HOSTED_SDIO_ERR("failed to initialize hosted transport\n");
        esp_hosted_sdio_hw_deinit(&s_ctx);
        return false;
    }

    s_ctx.attached = true;

    if (!esp_hosted_sdio_generate_slave_intr(ESP_HOSTED_SDIO_HOST_INTR_OPEN_DATA))
    {
        ESP_HOSTED_SDIO_ERR("failed to open slave data path\n");
        esp_hosted_sdio_detach();
        return false;
    }

    ESP_HOSTED_SDIO_LOG("[esp_hosted_sdio] attached slot=%u width=%u clk=%u kHz\n",
                        (unsigned)s_ctx.config.slot,
                        (unsigned)s_ctx.config.bus_width,
                        (unsigned)s_ctx.config.clock_khz);
    return true;
}

void esp_hosted_sdio_detach(void)
{
    if (esp_hosted_is_initialized())
    {
        (void)esp_hosted_set_transport(NULL);
    }

    if (s_ctx.attached)
    {
        esp_hosted_sdio_hw_deinit(&s_ctx);
    }
    memset(&s_ctx, 0, sizeof(s_ctx));
}

bool esp_hosted_sdio_is_attached(void)
{
    return s_ctx.attached;
}

bool esp_hosted_sdio_read_reg(uint32_t reg, void *data, size_t len)
{
    if (!s_ctx.attached || (data == NULL && len != 0U) || len > UINT32_MAX)
    {
        return false;
    }
    return esp_hosted_sdio_read_reg_bytes(reg, (uint8_t *)data, (uint32_t)len);
}

bool esp_hosted_sdio_write_reg(uint32_t reg, const void *data, size_t len)
{
    if (!s_ctx.attached || (data == NULL && len != 0U) || len > UINT32_MAX)
    {
        return false;
    }
    return esp_hosted_sdio_write_reg_bytes(reg, (const uint8_t *)data, (uint32_t)len);
}

/* ==========================================================================
 *
 * Adición de Xinu (W2.c).  Esto es lo único escrito a mano en el fichero; todo
 * lo de arriba es copia verbatim de
 * drivers/esp_hosted/esp_hosted_sdio.c del proyecto bare-metal de referencia.
 *
 * No escribe ni un registro: sólo lee.  Existe por dos reglas del owner:
 *  1) lo que sale por consola tiene que estar leído del propio periférico, no
 *     de un caché del driver, y
 *  2) las direcciones que usa el DMA tienen que verse en la consola.
 *
 * Y sirve para la prueba de convivencia: el slot 0 (microSD/FAT) y el slot 1
 * (C6) comparten el bloque SDHOST de 0x50083000 y, fijarse, hasta el DIVISOR
 * de reloj: CLKDIV tiene clk_divider0 y clk_divider1 en el MISMO registro de
 * 32 bits, y el reloj padre sdio_ls es global.  Volcarlo aquí es la forma de
 * comprobar que levantar el C6 no movió la configuración con la que Xinu
 * arranca la SD.
 * ========================================================================== */

void esp_hosted_sdio_diag_print(void)
{
    uint32_t packet_len;
    uint32_t token;
    uint32_t int_raw;
    uint32_t edge_l;
    uint32_t gdiv;

    if (!s_ctx.attached)
    {
        printf("esp_hosted_sdio: no adjunto (slot 1 inactivo)\n");
        return;
    }

    /* Reloj padre compartido.  HP_SYS_CLKRST.reg_sdio_ls_* es la division del
     * PLL SDIO de 160 MHz que alimenta a los DOS slots; el driver nunca lo
     * toca (desviacion 1), asi que edge_l debe seguir valiendo 3
     * (SD_HOST_DIV=4) y el resultado 40000 kHz, igual que al arrancar. */
    edge_l = HP_SYS_CLKRST.peri_clk_ctrl02.reg_sdio_ls_clk_edge_l;
    gdiv = edge_l + 1U;
    printf("[wdiag] sdio_ls global: edge_l=%lu div=%lu -> %lu kHz (160000/div)\n",
           (unsigned long)edge_l, (unsigned long)gdiv,
           gdiv != 0U ? (unsigned long)(160000U / gdiv) : 0U);

    /* CLKDIV, entero, leido del bloque: bits[7:0] son el divisor del slot 0 y
     * bits[15:8] el del slot 1.  El reloj de tarjeta es padre/(2*n), y n=0
     * significa sin dividir. */
    printf("[wdiag] SDMMC.CLKDIV=0x%08lx  slot0(n)=%lu slot1(n)=%lu\n",
           (unsigned long)SDMMC.clkdiv.val,
           (unsigned long)SDMMC.clkdiv.clk_divider0,
           (unsigned long)SDMMC.clkdiv.clk_divider1);
    printf("[wdiag] SDMMC.CLKENA=0x%08lx (cclk_enable bits[1:0])  CLKSRC=0x%08lx (card0=%lu card1=%lu)\n",
           (unsigned long)SDMMC.clkena.val, (unsigned long)SDMMC.clksrc.val,
           (unsigned long)SDMMC.clksrc.card0, (unsigned long)SDMMC.clksrc.card1);
    printf("[wdiag] SDMMC.STATUS=0x%08lx  INTMASK=0x%08lx  RINTSTS=0x%08lx  MINTSTS=0x%08lx\n",
           (unsigned long)SDMMC.status.val, (unsigned long)SDMMC.intmask.val,
           (unsigned long)SDMMC.rintsts.val, (unsigned long)SDMMC.mintsts.val);
    printf("[wdiag] RCA del C6 = 0x%04x\n", (unsigned)s_ctx.rca);

    /* Paginas compartidas con el DMA del motor SDHOST.  Son .bss normal de
     * HP_L2MEM (0x4FF.....); la columna "nc" es el alias sin caché
     * (+0x40000000) con el que realmente trabaja el DMA y el CPU a la vez,
     * que es por lo que no hace falta ninguna operacion de caché.  Las
     * paginas de la SD (.sdshare) y las de audio quedan en PSRAM y no se
     * tocan: aqui no hay ninguna direccion 0x48..... */
    printf("[wdiag] desc DMA      cached=0x%08lx  nc=0x%08lx  (64 B)\n",
           (unsigned long)&s_ctx.dma_desc,
           (unsigned long)CACHE_LL_L2MEM_NON_CACHE_ADDR(&s_ctx.dma_desc));
    printf("[wdiag] dma_bounce    cached=0x%08lx  nc=0x%08lx  (%u B)\n",
           (unsigned long)&s_ctx.dma_bounce[0],
           (unsigned long)CACHE_LL_L2MEM_NON_CACHE_ADDR(&s_ctx.dma_bounce[0]),
           (unsigned)ESP_HOSTED_SDIO_DMA_BUF_SIZE);
    printf("[wdiag] frame_buf RX  cached=0x%08lx  nc=0x%08lx  (%u B)\n",
           (unsigned long)&s_ctx.frame_buf[0],
           (unsigned long)CACHE_LL_L2MEM_NON_CACHE_ADDR(&s_ctx.frame_buf[0]),
           (unsigned)ESP_HOSTED_SDIO_FRAME_BUF_SIZE);
    printf("[wdiag] tx_frame TX   cached=0x%08lx  nc=0x%08lx  (%u B)\n",
           (unsigned long)&s_ctx.tx_frame[0],
           (unsigned long)CACHE_LL_L2MEM_NON_CACHE_ADDR(&s_ctx.tx_frame[0]),
           (unsigned)ESP_HOSTED_SDIO_TX_FRAME_BUF_SIZE);

    /* Ventana de la funcion 1 del slave, leida con CMD53 de verdad: es la
     * prueba de que el camino de datos sigue vivo despues de subir el reloj.
     * PACKET_LEN dice cuanto tiene preparado el C6 para subir y TOKEN_RDATA
     * cuanto buffer de TX le queda al host. */
    packet_len = 0U;
    token = 0U;
    int_raw = 0U;
    if (esp_hosted_sdio_read_reg(ESP_HOSTED_SDIO_SLAVE_PACKET_LEN_REG, &packet_len, sizeof(packet_len)))
    {
        printf("[wdiag] fn1 PACKET_LEN = 0x%08lx (%lu bytes pendientes de subir)\n",
               (unsigned long)packet_len,
               (unsigned long)(packet_len & ESP_HOSTED_SDIO_SLAVE_LEN_MASK));
    }
    else
    {
        printf("[wdiag] fn1 PACKET_LEN: fallo CMD53\n");
    }

    if (esp_hosted_sdio_read_reg(ESP_HOSTED_SDIO_SLAVE_TOKEN_RDATA, &token, sizeof(token)))
    {
        printf("[wdiag] fn1 TOKEN_RDATA = 0x%08lx (creditos TX = %lu)\n",
               (unsigned long)token,
               (unsigned long)((token >> 16) & ESP_HOSTED_SDIO_TX_BUF_MASK));
    }
    else
    {
        printf("[wdiag] fn1 TOKEN_RDATA: fallo CMD53\n");
    }

    if (esp_hosted_sdio_read_reg(ESP_HOSTED_SDIO_SLAVE_INT_RAW_REG, &int_raw, sizeof(int_raw)))
    {
        printf("[wdiag] fn1 INT_RAW   = 0x%08lx%s%s%s\n",
               (unsigned long)int_raw,
               (int_raw & ESP_HOSTED_SDIO_INT_NEW_PACKET) ? " NEW_PACKET" : "",
               (int_raw & ESP_HOSTED_SDIO_INT_START_THROTTLE) ? " START_THROTTLE" : "",
               (int_raw & ESP_HOSTED_SDIO_INT_STOP_THROTTLE) ? " STOP_THROTTLE" : "");
    }
    else
    {
        printf("[wdiag] fn1 INT_RAW: fallo CMD53\n");
    }

    printf("[wdiag] cola RX: pendientes=%lu descartados=%lu\n",
           (unsigned long)esp_hosted_rx_pending(),
           (unsigned long)esp_hosted_rx_dropped());
}
