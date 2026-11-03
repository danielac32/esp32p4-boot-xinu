/**
 * @file sdHw.c
 *
 * SDMMC (SDHOST) hardware layer for the ESP32-P4: pads, clock tree, clock,
 * command engine and the block data phase.  This is the body of the SDMMC0
 * host device; the card protocol (identification, sector cursor, R1 policy)
 * lives one layer up in device/sdblock-esp32p4 and reaches everything here
 * through <sdmmc.h>.
 *
 * A block moves by the block's own DMA engine (the IDMAC), one descriptor
 * per block.  Both the descriptor and the block live in PSRAM pages the CPU
 * reaches through the chip's direct (uncached) external-memory window, so
 * neither direction needs a cache operation (see "The pages CPU and engine
 * share").
 *
 * Slot 0 is the microSD socket; slot 1 belongs to the ESP32-C6.  Both live
 * in one DesignWare block, so every transaction here takes the kernel's
 * shared bus lock (sdmmcBusLock(), <sdmmc.h>) and the commands say which
 * slot they mean instead of "whichever is active".  The DMA engine is part
 * of that one block too, so it is parked inside each transaction rather
 * than left running for whoever comes next.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <stdint.h>
#include <string.h>

#include <sdmmc.h>

#include "sdhost.h"

/* ------------------------------------------------------------------ */
/* Host state                                                         */
/* ------------------------------------------------------------------ */
uint32_t sd_hwerr;                  /* why the last command failed     */
uint32_t sd_clk_khz;                /* card clock once the card is up  */
uint32_t sd_host_khz = SD_HOST_HZ / 1000UL;   /* bloque: 160 MHz / SD_HOST_DIV */
/* Diagnostico de sdSetClockSlot: 0 si no fallo.  Cuando una subida de reloj
 * devuelve FALSE, sd_clkdbg = (paso<<24)|(motivo<<20)|reintentos y
 * sd_clkdbg_reg = la captura del registro en el instante del fallo (RINTSTS o
 * CMD segun el motivo).  Paso 1 = detener reloj, 2 = fijar divisor,
 * 3 = arrancar reloj.  Motivo 1 = no despeja CMD_START antes de lanzar,
 * 2 = bit de error en RINTSTS, 3 = no despeja CMD_START tras lanzar,
 * 4 = agotados los reintentos por HLE. */
uint32_t sd_clkdbg;
uint32_t sd_clkdbg_reg;
uint32_t sd_clkdbg_cmd;      /* SDHOST_CMD en el instante del fallo        */
uint32_t sd_clkdbg_status;   /* SDHOST_STATUS (inhibit) en el instante     */
uint32_t sd_clkdbg_clkena;   /* SDHOST_CLKENA en el instante del fallo     */
uint32_t sd_host_lba;               /* cursor for the SDMMC0 device    */
uint8_t sd_host_width = 1;
bool sd_host_ready = FALSE;
bool sd_host_hc = FALSE;            /* card is block addressed (SDHC)  */

/* ------------------------------------------------------------------ */
/* The pages CPU and engine share                                     */
/* ------------------------------------------------------------------ */
/* A whole page each (.sdshare, NOLOAD, in the linker script): the one
 * descriptor at the start of the first, the 512 byte block at the start of
 * the second, and nothing else in either page.
 *
 * PSRAM answers at two addresses (TRM 7.3.3.1): 0x48...... is the cached
 * path the rest of the kernel uses, and 0x88...... (this offset) is the
 * CPU's direct path to the same cells, which does not go through the
 * caches.  So the split is: the CPU touches the pages only through the
 * 0x88 view - its stores land in PSRAM at once and its loads never hit a
 * stale line - while every address handed to the IDMAC is the plain 0x48
 * one, which is the form that bus master understands.  Nothing in the data
 * path needs a cache operation.
 *
 * (The same offset over internal L2MEM, 0x8F......, does not work: this
 * chip only has the direct path for external memory.) */
extern uint8_t _sd_desc_page[];
extern uint8_t _sd_block_page[];

#define SD_CPU_UNCACHED   0x40000000UL
#define sd_desc_page      ((uint8_t *)(void *)((uintptr_t)_sd_desc_page \
                                               + SD_CPU_UNCACHED))
#define sd_block_page     ((uint8_t *)(void *)((uintptr_t)_sd_block_page \
                                               + SD_CPU_UNCACHED))
#define sd_bounce         ((uint32_t *)(void *)sd_block_page)

/* ------------------------------------------------------------------ */
/* The engine's one descriptor                                        */
/* ------------------------------------------------------------------ */
/* The ESP32-P4 IDMAC descriptor: word 0 holds the flags and the ownership
 * bit, word 1 the first buffer's length, word 2 that buffer's address and
 * word 3 the next descriptor.  The engine hands the descriptor back by
 * clearing the ownership bit.  Volatile because the engine writes it behind
 * the compiler's back. */
typedef struct
{
    uint32_t ctrl;
    uint32_t sizes;
    uint32_t buffer;
    uint32_t next;
    uint32_t reserved[12];          /* the engine reads 64 bytes          */
} sdBlockDesc;

#define SD_DESC_OWN         (1UL << 31)   /* engine owns the descriptor */
#define SD_DESC_FIRST       (1UL << 3)
#define SD_DESC_LAST        (1UL << 2)
#define SD_DESC_SECOND_ADDR (1UL << 4)    /* word 3 is a link (or the end) */
#define SD_DESC_RING_END    (1UL << 5)    /* wrap back to the fetch address */

#define sd_desc ((volatile sdBlockDesc *)(void *)sd_desc_page)

/* ------------------------------------------------------------------ */
/* Pads                                                               */
/* ------------------------------------------------------------------ */

/** Drive an already-configured GPIO output (pins 32..54 are in OUT1). */
static void sdPinWrite(uint8_t pin, bool level)
{
    uint32_t mask = 1UL << (pin - 32U);

    sdRegWrite(level ? P4_GPIO_OUT1_W1TS : P4_GPIO_OUT1_W1TC, 0, mask);
}

/** Put a slot pad on the SDMMC bus: its direct function, input on, 40 mA. */
static void sdPadToSdmmc(uint8_t pin, bool pullup)
{
    uint32_t val = sdRegRead(P4_IOPAD(pin), 0);

    val &= ~(P4_IO_MCU_SEL_MASK | P4_IO_FUN_WPD | P4_IO_FUN_WPU |
             P4_IO_FUN_DRV_MASK);
    /* mcu_sel cleared = the pad keeps its fixed SDMMC function. */
    val |= P4_IO_FUN_IE | P4_IO_FUN_DRV_40MA;
    if (pullup)
    {
        val |= P4_IO_FUN_WPU;
    }
    sdRegWrite(P4_IOPAD(pin), 0, val);
}

/** Drive a pad high as a plain GPIO output, off the SDMMC function. */
static void sdPadDriveHigh(uint8_t pin)
{
    uint32_t val = sdRegRead(P4_IOPAD(pin), 0);

    val &= ~(P4_IO_MCU_SEL_MASK | P4_IO_FUN_WPD | P4_IO_FUN_DRV_MASK);
    val |= P4_IO_MCU_SEL_GPIO | P4_IO_FUN_IE | P4_IO_FUN_DRV_40MA;
    sdRegWrite(P4_IOPAD(pin), 0, val);

    /* The pad follows GPIO_OUT and its output enable follows GPIO_ENABLE. */
    val = sdRegRead(P4_GPIO_OUT_SEL(pin), 0);
    val &= ~P4_GPIO_OUT_SEL_MASK;
    sdRegWrite(P4_GPIO_OUT_SEL(pin), 0,
               val | P4_GPIO_OUT_SEL_GPIO | P4_GPIO_OEN_SEL);

    sdPinWrite(pin, TRUE);                 /* level before the enable: no glitch */
    sdRegOr(P4_GPIO_ENABLE1_W1TS, 0, 1UL << (pin - 32U));
}

/**
 * Turn on LDO_VO4, the rail that feeds the microSD socket (and the IO
 * supply of the fixed-function pads 39..44).  Same order as ESP-IDF's
 * esp_ldo_acquire_channel() + set_io_voltage(3300): bypass to the 3.3 V
 * rail, software ownership, ripple suppression, then power the unit up.
 */
static void sdSlotPowerOn(void)
{
    sdRegOr(P4_PMU_LDO_VO4_ANA, 0, P4_PMU_LDO_EN_VDET);

    /* tieh=1 selects the 3.3 V rail; force_tieh_sel=1 with tieh_sel=0 makes
     * the unit follow that bit instead of the eFuse/hardware setting. */
    sdRegOr(P4_PMU_LDO_VO4_CTRL, 0, P4_PMU_LDO_TIEH_3V3 |
                                    P4_PMU_LDO_FORCE_TIEH_SEL);
    sdRegOr(P4_PMU_LDO_VO4_CTRL, 0, P4_PMU_LDO_XPD);
    udelay(10000);                    /* let VDD settle before the pads move */
}

/**
 * Bring the slot up.
 *
 * The socket is fed by the on-chip LDO VO4 (there is no power GPIO on this
 * board), so the first move is to turn that rail on.  CLK is output only;
 * CMD and DAT0..2 are the card's own lines and get a pull-up so an idle
 * bus reads high.
 *
 * DAT3 is not put on the bus yet: a card that sees DAT3 low while it is
 * powering up switches to SPI mode and never answers a native SD command,
 * so it is held high as a plain output until sdPadsWiden() hands it over
 * after ACMD6.
 */
void sdPadsInit(void)
{
    sdSlotPowerOn();

    sdPadToSdmmc(SD_PIN_CLK, FALSE);
    sdPadToSdmmc(SD_PIN_CMD, TRUE);
    sdPadToSdmmc(SD_PIN_D0, TRUE);
    sdPadToSdmmc(SD_PIN_D1, TRUE);
    sdPadToSdmmc(SD_PIN_D2, TRUE);

    sdPadDriveHigh(SD_PIN_D3);
}

/** DAT3 joins the bus once the card really runs on four data lines. */
void sdPadsWiden(void)
{
    sdPadToSdmmc(SD_PIN_D3, TRUE);
}

/**
 * Put the slot on \p width data lines.  Widening finishes the DAT3 hand
 * over the probe left pending; narrowing only changes what the controller
 * drives, the pads stay on the bus.
 */
bool sdSetWidth(uint width)
{
    if (4U == width)
    {
        sdPadsWiden();
        SD_REG(SDHOST_CTYPE) |= SD_CTYPE_WIDTH4_CARD0;
        sd_host_width = 4;
        return TRUE;
    }
    if (1U == width)
    {
        SD_REG(SDHOST_CTYPE) &= ~SD_CTYPE_WIDTH4_CARD0;
        sd_host_width = 1;
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Clock                                                              */
/* ------------------------------------------------------------------ */

/** Wait for the controller to finish taking the command register. */
static bool sdWaitCmdTaken(void)
{
    uint32_t t0 = sdNow();

    while (SD_REG(SDHOST_CMD) & SD_CMD_START)
    {
        if (sdTimedOut(t0, SD_TO_CMD_MS))
        {
            return FALSE;
        }
    }
    return TRUE;
}

/** Snapshot the controller state so a failed clock update can be diagnosed. */
static void sdClockDbgCapture(uint32_t step, uint32_t reason, uint32_t tries,
                              uint32_t reg)
{
    sd_clkdbg        = (step << 24) | (reason << 20) | tries;
    sd_clkdbg_reg    = reg;
    sd_clkdbg_cmd    = SD_REG(SDHOST_CMD);
    sd_clkdbg_status = SD_REG(SDHOST_STATUS);
    sd_clkdbg_clkena = SD_REG(SDHOST_CLKENA);
}

/**
 * Ask the controller to push the clock registers out to the card clock
 * generation unit for \p slot.  The update command carries the slot index in
 * CMD[20:16] but has no argument and no response; the hardware sets HLE on
 * success, which is expected and needs a retry.  \p step labels which update of
 * a sdSetClockSlotEx this is (1 detener, 2 divisor, 3 arrancar, 5 live), only
 * for the failure diagnostic in sd_clkdbg.
 *
 * \p waitComplete es la decision delicada.  CMD[13]=1 dice "espera a que la
 * transferencia de datos anterior termine" antes de cambiar el reloj, y el
 * WAIT_COMPLETE se queda mirando data_busy (STATUS[9], que es el valor
 * muestreado de card_data[0]).  Un slave esp_hosted deja DAT0 abajo durante la
 * identificacion, y ese muestreo solo se refresca con el reloj del slot
 * corriendo: con CLKENA bajado el bit queda clavado y el update no termina
 * nunca -- CMD_START no se auto-limpia y a partir de ahi TODO comando del slot
 * falla (el enquistamiento medido: STATUS=0x0001a306, RINTSTS HLE).  Con
 * waitComplete=0 el update se lanza de una vez; es lo correcto entre
 * transacciones, porque el update_clk no envia nada a la tarjeta.
 */
static bool sdClockUpdateSlot(uint32_t slot, uint32_t step, bool waitComplete)
{
    uint tries;

    for (tries = 0; tries < 16; tries++)
    {
        uint32_t t0;

        if (!sdWaitCmdTaken())
        {
            sdClockDbgCapture(step, 1U, tries, SD_REG(SDHOST_RINTSTS));
            return FALSE;
        }

        SD_REG(SDHOST_RINTSTS) = SD_IRQ_ALL;
        SD_REG(SDHOST_CMDARG) = 0;
        SD_REG(SDHOST_CMD) = SD_CMD_UPDATE_CLK
                             | (waitComplete ? SD_CMD_WAIT_COMPLETE : 0U)
                             | SD_CMD_USE_HOLD
                             | ((slot & 0x1FU) << SD_CMD_CARD_NUM_POS)
                             | SD_CMD_START;

        t0 = sdNow();
        for (;;)
        {
            uint32_t raw = SD_REG(SDHOST_RINTSTS);

            if (raw & SD_IRQ_HLE)
            {
                /* Expected for a clock update: retry. */
                SD_REG(SDHOST_RINTSTS) = SD_IRQ_HLE;
                break;
            }
            if (raw & SD_CMD_ERR_MASK)
            {
                SD_REG(SDHOST_RINTSTS) = raw;
                sdClockDbgCapture(step, 2U, tries, raw);
                return FALSE;
            }
            if (0 == (SD_REG(SDHOST_CMD) & SD_CMD_START))
            {
                SD_REG(SDHOST_RINTSTS) = SD_IRQ_ALL;
                return TRUE;
            }
            if (sdTimedOut(t0, SD_TO_CMD_MS))
            {
                sdClockDbgCapture(step, 3U, tries, SD_REG(SDHOST_RINTSTS));
                return FALSE;
            }
        }
    }
    /* HLE raised on all 16 tries: the clock update never settled. */
    sdClockDbgCapture(step, 4U, 16U, SD_REG(SDHOST_RINTSTS));
    return FALSE;
}

/**
 * Change \p slot's card clock divider.  El divisor global del bloque se fija en
 * sdSdioClockInit() y solo lo mueve quien llame a sdSetHostDiv(); aqui se usa su
 * valor actual (sd_host_khz) para reportar la velocidad resultante.
 *
 * CLKSRC no es una seleccion de "salidas del divisor global": sdhost_clksrc_reg_t
 * (soc/sdmmc_struct.h hw_ver3) enlaza cada slot con una de las CUATRO divisiones
 * internas del bloque, y la division n es el byte n de CLKDIV (clk_divider0
 * [7:0], clk_divider1 [15:8], ...).  Por eso el valor correcto del campo de un
 * slot es SU PROPIO numero: slot 0 -> 0, slot 1 -> 1 (igual que hace el SDK en
 * sdmmc_ll_set_card_clock_div, que fuerza clksrc.card1 = 1 para el slot 1).
 * Poner el campo a 0 en el slot 1 lo enlaza a la division de la microSD: los
 * bytes 1 de CLKDIV se escriben pero no gobiernan nada, y el C6 queda a merced
 * del reloj que la SD tenga puesto en ese momento.
 *
 * Los demas campos por-slot: CLKENA bit \p slot (enable) y bit 16+\p slot
 * (low power).
 *
 * \p stopClock /\p waitComplete dan las tres variantes publicas:
 *   - parada + espera: la secuencia clasica, la que us6 siempre el slot 0.
 *   - sin parar + espera: por si el busy se drena muestreando con el reloj vivo.
 *   - sin parar + sin espera: la unica segura con un slave que mantiene DAT0
 *     abajo, porque no deja el reloj apagado esperando un bit que ya no se
 *     muestrea (ver sdClockUpdateSlot).
 */
static bool sdSetClockSlotEx(uint32_t slot, uint8_t card_div, bool stopClock,
                             bool waitComplete)
{
    uint32_t ena     = 1UL << slot;
    uint32_t lpena   = 1UL << (16 + slot);
    uint32_t divmask = 0xFFUL << (8 * slot);
    uint32_t srcmask = 0x3UL << (2 * slot);

    sd_clkdbg = 0;

    if (stopClock)
    {
        /* Stop this slot's card clock before touching the dividers. */
        SD_REG(SDHOST_CLKENA) &= ~ena;
        if (!sdClockUpdateSlot(slot, 1U, waitComplete))
        {
            sd_hwerr = SD_HWERR_TIMEOUT | SD_REG(SDHOST_STATUS);
            return FALSE;
        }
    }
    else
    {
        /* Dejar el reloj del slot puesto para que la linea DAT0 siga
         * muestrandose mientras el controlador ejecuta el update. */
        SD_REG(SDHOST_CLKENA) |= ena;
    }

    /* Enlazar el slot con su propia division interna y escribir esa division
     * (divide entre 2*n; n = 0 la salta). */
    SD_REG(SDHOST_CLKSRC) = (SD_REG(SDHOST_CLKSRC) & ~srcmask)
                            | ((slot & 3UL) << (2 * slot));
    SD_REG(SDHOST_CLKDIV) = (SD_REG(SDHOST_CLKDIV) & ~divmask)
                            | ((uint32_t)card_div << (8 * slot));

    if (!sdClockUpdateSlot(slot, 2U, waitComplete))
    {
        sd_hwerr = SD_HWERR_TIMEOUT | SD_REG(SDHOST_STATUS);
        return FALSE;
    }

    SD_REG(SDHOST_CLKENA) |= ena;
    if (!sdClockUpdateSlot(slot, 3U, waitComplete))
    {
        sd_hwerr = SD_HWERR_TIMEOUT | SD_REG(SDHOST_STATUS);
        return FALSE;
    }

    SD_REG(SDHOST_CLKENA) |= lpena;

    sd_clk_khz = (uint32_t)(sd_host_khz * 1000UL /
                            ((card_div == 0) ? 1UL : (uint32_t)card_div * 2UL)
                            / 1000UL);
    return TRUE;
}

bool sdSetClockSlot(uint32_t slot, uint8_t card_div)
{
    return sdSetClockSlotEx(slot, card_div, TRUE, TRUE);
}

/** Set slot 0's card clock divider (this driver's own slot). */
bool sdSetClock(uint8_t card_div)
{
    return sdSetClockSlot(SD_SLOT, card_div);
}

bool sdSetClockSlotLive(uint32_t slot, uint8_t card_div)
{
    return sdSetClockSlotEx(slot, card_div, FALSE, TRUE);
}

bool sdSetClockSlotNoWait(uint32_t slot, uint8_t card_div)
{
    return sdSetClockSlotEx(slot, card_div, FALSE, FALSE);
}

/**
 * Imprimir el estado del camino de un slot, descifrado campo a campo desde el
 * propio STATUS (sdhost_status_reg_t).  Lo que importa saber en cada paso de
 * una secuencia SDIO es data_busy: con DAT0 afirmado por el esclavo, el comando
 * interno "update clock" (y cualquier comando con wait_complete) queda
 * inhibido, y si ademas el reloj del slot esta parado ese busy no se muestrea
 * jamas.  CLKDIV/CLKSRC se imprimen crudos para ver a que division interna
 * (byte de CLKDIV) esta enlazado el slot en ese instante.
 */
void sdBusState(const char *tag, uint32_t slot)
{
    uint32_t st = SD_REG(SDHOST_STATUS);
    uint32_t dv = SD_REG(SDHOST_CLKDIV);
    uint32_t cs = SD_REG(SDHOST_CLKSRC);

    kprintf("  %s STATUS=0x%08x fifo_empty=%u fifo_count=%u fsm=%u "
            "presente=%u data_busy=%u dat_path=%u ultimo_cmd=%u CLKENA=0x%08x"
            " | CLKDIV%u=%u CLKSRC%u=%u\r\n",
            tag, st,
            (st & SD_STATUS_FIFO_EMPTY) ? 1U : 0U,
            (st & SD_STATUS_FIFO_COUNT_MASK) >> SD_STATUS_FIFO_COUNT_POS,
            (st & SD_STATUS_FSM_MASK) >> SD_STATUS_FSM_POS,
            (st & SD_STATUS_CARD_PRESENT) ? 1U : 0U,
            (st & SD_STATUS_DATA_BUSY) ? 1U : 0U,
            (st & SD_STATUS_DAT_PATH_BUSY) ? 1U : 0U,
            (st & SD_STATUS_RESP_IDX_MASK) >> SD_STATUS_RESP_IDX_POS,
            SD_REG(SDHOST_CLKENA),
            (unsigned)slot, (unsigned)((dv >> (8 * slot)) & 0xFFU),
            (unsigned)slot, (unsigned)((cs >> (2 * slot)) & 3U));
}

/**
 * Fijar el DIVISOR GLOBAL sdio_ls (HP_SYS_CLKRST.peri_clk_ctrl02), el reloj
 * base del que salen los dos slots.  El periodo es (l + 1) ciclos de la fuente
 * y la fase alta (h + 1) de ellos, con los tres campos de 4 bits: o sea que
 * solo se puede dividir entre 1 y 16, y el minimo del bloque es 160/16 = 10 MHz.
 *
 * Por que es una funcion y no una constante del init: el driver de referencia
 * del C6 NUNCA usa el divisor por-slot durante una fase de datos -- baja el
 * divisor GLOBAL y deja card_div=0 (salto) para la velocidad de trabajo
 * (host_div=7 -> 22,857 MHz exactos).  Asi el reloj que muestrea el CIU es el
 * mismo que sale por la pata, que es lo que suponen las fases drv/sam/slf.
 * Con nuestro slot 1 enrutado por la matriz GPIO --que anade retardo en los dos
 * sentidos-- esa coherencia puede ser justamente lo que falte, asi que poder
 * mover el global desde la sonda es el experimento.  El init del host lo deja
 * en SD_HOST_DIV y la sonda lo restaura al terminar.
 */
bool sdSetHostDiv(uint32_t div)
{
    uint32_t edge;

    if (div < 1UL || div > 16UL)
    {
        return FALSE;
    }

    edge = sdRegRead(P4_CLKRST_PERI_CTRL02, 0);
    edge &= ~((P4_EDGE_MASK << P4_SDIO_LS_EDGE_L_POS)
              | (P4_EDGE_MASK << P4_SDIO_LS_EDGE_H_POS)
              | (P4_EDGE_MASK << P4_SDIO_LS_EDGE_N_POS));
    edge |= ((div / 2UL - 1UL) & P4_EDGE_MASK) << P4_SDIO_LS_EDGE_H_POS;
    edge |= ((div - 1UL) & P4_EDGE_MASK) << P4_SDIO_LS_EDGE_L_POS;
    edge |= ((div - 1UL) & P4_EDGE_MASK) << P4_SDIO_LS_EDGE_N_POS;
    sdRegWrite(P4_CLKRST_PERI_CTRL02, 0, edge);

    /* The divider and phase fields only take effect on an update pulse. */
    sdRegOr(P4_CLKRST_PERI_CTRL02, 0, P4_SDIO_LS_EDGE_UPDATE);
    sdRegClear(P4_CLKRST_PERI_CTRL02, 0, P4_SDIO_LS_EDGE_UPDATE);
    udelay(10);

    sd_host_khz = (uint32_t)(SD_CLK_SRC_HZ / div / 1000UL);
    return TRUE;
}

/**
 * Power the SDIO PLL and route it to the SDMMC block, then fix the global
 * divider shared by both slots.  Everything is set-or-left: the PLL and the
 * enables are idempotent, and the divider keeps whatever value this driver
 * chose for the whole session.
 */
static bool sdSdioClockInit(void)
{
    sdRegOr(P4_CLKRST_SOC_CTRL1, 0, P4_SDMMC_SYS_CLK_EN);

    /* The PLL needs its power domain, its ICG and its three output gates
     * in this order (ESP-IDF sdmmc_ll_enable_sdio_pll). */
    sdRegOr(P4_PMU_RF_PWC, 0, P4_PMU_SDIO_PLL_XPD);
    sdRegOr(P4_PMU_IMM_HP_CK_POWER, 0, P4_PMU_XPD_SDIOPLL_I2C
                                       | P4_PMU_XPD_SDIOPLL
                                       | P4_PMU_GLOBAL_SDIOPLL_ICG);
    sdRegOr(P4_CLKRST_HP_CLK_CTRL, 0, P4_SDIO_PLL0_CLK_EN
                                      | P4_SDIO_PLL1_CLK_EN
                                      | P4_SDIO_PLL2_CLK_EN);
    udelay(200);

    /* Source = PLL160M (sel 0), low-speed branch enabled, high-speed bypass
     * off because the divider below is what sets the block clock. */
    sdRegClear(P4_CLKRST_PERI_CTRL01, 0, P4_SDIO_LS_SRC_SEL | P4_SDIO_HS_MODE);
    sdRegOr(P4_CLKRST_PERI_CTRL01, 0, P4_SDIO_LS_CLK_EN);

    /* Global divider: SD_HOST_DIV = 4 gives 40 MHz at 50 % duty. */
    sdSetHostDiv(SD_HOST_DIV);

    /* Sampling and output phases: drive the outgoing clock/data one phase
     * late and sample on the unperturbed edge, which is what an HS-mode
     * card on short board traces expects. */
    sdRegOr(P4_CLKRST_PERI_CTRL02, 0, P4_SDIO_LS_DRV_CLK_EN
                                      | P4_SDIO_LS_SAM_CLK_EN
                                      | P4_SDIO_LS_SLF_CLK_EN);
    sdRegClear(P4_CLKRST_PERI_CTRL02, 0,
               (3UL << P4_SDIO_LS_SAM_EDGE_POS) | (3UL << P4_SDIO_LS_SLF_EDGE_POS));
    sdRegWrite(P4_CLKRST_PERI_CTRL02, 0,
               (sdRegRead(P4_CLKRST_PERI_CTRL02, 0)
                & ~(3UL << P4_SDIO_LS_DRV_EDGE_POS))
               | (1UL << P4_SDIO_LS_DRV_EDGE_POS));

    /* The divider and phase fields only take effect on an update pulse. */
    sdRegOr(P4_CLKRST_PERI_CTRL02, 0, P4_SDIO_LS_EDGE_UPDATE);
    sdRegClear(P4_CLKRST_PERI_CTRL02, 0, P4_SDIO_LS_EDGE_UPDATE);
    udelay(10);

    return TRUE;
}

/** Full controller reset; the reset bits clear themselves. */
static bool sdResetController(void)
{
    uint32_t t0;

    SD_REG(SDHOST_CTRL) = SD_CTRL_CONTROLLER_RESET | SD_CTRL_FIFO_RESET
                          | SD_CTRL_DMA_RESET;

    t0 = sdNow();
    while (SD_REG(SDHOST_CTRL) & (SD_CTRL_CONTROLLER_RESET
                                  | SD_CTRL_FIFO_RESET | SD_CTRL_DMA_RESET))
    {
        if (sdTimedOut(t0, SD_TO_RESET_MS))
        {
            return FALSE;
        }
    }
    return TRUE;
}

/**
 * Reset de protocolo del BLOQUE entero, forzado.  Hace falta despues de una
 * fase de datos muerta (un CMD53 del slot 1 al que el esclavo no contesto):
 * mientras no se hace, STATUS.data_state_mc_busy queda en 1 y ese bit inhibe el
 * "update clock" de CUALQUIER slot, o sea que el slot 0 no puede ni cambiar de
 * reloj y la microSD no vuelve hasta reiniciar la placa.  No lo cubre
 * sdControllerInit(), que solo resetea si los bits de CTRL vienen ya alzados --
 * y un fallo los deja bajados--, y tampoco los reseteos parciales de sdDataAbort
 * (FIFO y DMA cada uno por su lado), que dejan la maquina de estados de datos
 * donde estaba.  Los tres bits se auto-limpian; si alguno se queda, es que el
 * bloque no tiene reloj y ya no hay nada que hacer por software.
 */
bool sdBlockReset(void)
{
    return sdResetController();
}

/**
 * Let a bus master reach the L2 cache block.  It comes out of reset with
 * DMA access shut (CACHE_L2_CACHE_SHUT_DMA = 1), and a master then reads
 * zeros and its writes are dropped.  ESP-IDF clears it on every boot
 * through the ROM's Cache_Enable_L2_Cache(); that call leaves this bit
 * alone here, so the register is written directly.
 */
#define SD_L2_CACHE_BASE     0x3FF10000UL   /* soc/cache_reg.h       */
#define SD_L2_CACHE_CTRL     0x270U
#define SD_L2_SHUT_DMA       (1UL << 4)

static void __attribute__((unused)) sdEngineBusOpen(void)
{
    sdRegClear(SD_L2_CACHE_BASE, SD_L2_CACHE_CTRL, SD_L2_SHUT_DMA);
}

/**
 * Bring the SDHOST block up and configure it: clocks, no interrupts
 * (everything is polled) and the DMA engine soft-reset and parked.
 *
 * The soft reset clears the WHOLE block, slot 1 (the ESP32-C6) included,
 * so it only happens when the block is still sitting in reset - if another
 * driver already brought it up, its slot stays untouched.
 */
bool sdControllerInit(void)
{
    uint32_t t0;

    /* [BISECCION temporal] sin abrir el camino DMA del L2: es lo unico que el
     * arranque de HEAD no hace y pisa la linea 0x4ff0fc80. */
    /* sdEngineBusOpen(); */

    if (!sdSdioClockInit())
    {
        return FALSE;
    }

    if (SD_REG(SDHOST_CTRL) & (SD_CTRL_CONTROLLER_RESET | SD_CTRL_FIFO_RESET
                               | SD_CTRL_DMA_RESET))
    {
        if (!sdResetController())
        {
            return FALSE;
        }
    }

    /* Clear everything and stay quiet: this driver polls RINTSTS. */
    SD_REG(SDHOST_RINTSTS) = SD_IRQ_ALL;
    SD_REG(SDHOST_INTMASK) = 0;
    SD_REG(SDHOST_CTRL) &= ~(SD_CTRL_INT_ENABLE | SD_CTRL_DMA_ENABLE
                             | SD_CTRL_USE_INTERNAL_DMA);

    /* Park the engine the block shares with slot 1: soft reset it, take the
     * stale descriptor status away, and leave its interrupts disabled.  Each
     * transfer arms what it needs and parks it again on the way out. */
    SD_REG(SDHOST_BMOD) = SD_BMOD_SW_RESET;
    t0 = sdNow();
    while (SD_REG(SDHOST_BMOD) & SD_BMOD_SW_RESET)
    {
        if (sdTimedOut(t0, SD_TO_RESET_MS))
        {
            return FALSE;
        }
    }
    SD_REG(SDHOST_IDSTS) = SD_IDSTS_ALL;
    SD_REG(SDHOST_IDINTEN) = 0;

    /* FIFOTH is left at its reset value, as ESP-IDF leaves it: PBL in BMOD
     * mirrors its MSIZE field, and the engine's default burst arithmetic
     * moves a block without help. */

    /* Timeouts wide open; the driver enforces its own limits. */
    SD_REG(SDHOST_TMOUT) = 0xFFU | (0x00FFFFFFUL << 8);

    /* Leave the card out of hardware reset (the field powers up at 1; slot
     * VDD is the LDO VO4 rail, already on from sdPadsInit). */
    SD_REG(SDHOST_RST_N) = SD_RST_N_ACTIVE;

    /* Power-on delay for the card (74 clocks at 400 kHz), then 1-bit bus
     * for identification. */
    t0 = sdNow();
    while (!sdTimedOut(t0, 20))
    {
        /* spin */
    }
    SD_REG(SDHOST_CTYPE) = 0;

    sd_host_width = 1;
    sd_host_lba = 0;
    sd_host_ready = TRUE;
    return TRUE;
}

/* ------------------------------------------------------------------ */

/* Command engine                                                     */
/* ------------------------------------------------------------------ */

/**
 * Send one command to \p slot and, if it expects a response, wait for it.
 * The slot is named by CMD[20:16]; the two slots share one block, so this is
 * how the microSD driver (slot 0) and the ESP32-C6 SDIO probe (slot 1) reach
 * the same command engine.
 *
 * \p resp is 0 (none), 1 (short, 48 bit) or 2 (long, 136 bit).
 * \p crc asks the controller to verify the response CRC, which only
 * makes sense for the commands whose responses actually carry one.
 * \p send_init sets CMD.SEND_INIT: the controller performs the SD/SDIO
 * power-up sequence (>=74 clocks on this slot with CMD high) before the
 * command, which is how ESP-IDF resets an SDIO slave on its CMD0.
 */
bool sdSendCmdSlot(uint32_t slot, uint idx, uint32_t arg, uint resp, bool crc,
                   bool send_init, uint32_t *r0)
{
    uint32_t cmd;
    uint32_t t0;

    if (!sdWaitCmdTaken())
    {
        sd_hwerr = SD_HWERR_INHIBIT | SD_REG(SDHOST_RINTSTS);
        return FALSE;
    }

    SD_REG(SDHOST_RINTSTS) = SD_IRQ_ALL;
    SD_REG(SDHOST_CMDARG) = arg;

    cmd = (uint32_t)(idx & SD_CMD_INDEX_MASK)
          | SD_CMD_WAIT_COMPLETE | SD_CMD_USE_HOLD
          | ((slot & 0x1FU) << SD_CMD_CARD_NUM_POS);
    if (send_init)
    {
        cmd |= SD_CMD_SEND_INIT;
    }
    if (resp != 0)
    {
        cmd |= SD_CMD_RESP_EXPECT;
    }
    if (resp == 2)
    {
        cmd |= SD_CMD_RESP_LONG;
    }
    if (crc && resp != 0)
    {
        cmd |= SD_CMD_RESP_CRC;
    }
    SD_REG(SDHOST_CMD) = cmd | SD_CMD_START;

    if (!sdWaitCmdTaken())
    {
        sd_hwerr = SD_HWERR_INHIBIT | SD_REG(SDHOST_RINTSTS);
        return FALSE;
    }

    t0 = sdNow();
    for (;;)
    {
        uint32_t raw = SD_REG(SDHOST_RINTSTS);

        if (raw & SD_CMD_ERR_MASK)
        {
            SD_REG(SDHOST_RINTSTS) = raw;
            sd_hwerr = SD_HWERR_ERR | (raw & SD_CMD_ERR_MASK);
            return FALSE;
        }
        if (raw & SD_IRQ_CMD_DONE)
        {
            SD_REG(SDHOST_RINTSTS) = raw;
            if (r0 != NULL && resp != 0)
            {
                *r0 = SD_REG(SDHOST_RESP(0));
            }
            sd_hwerr = 0;
            return TRUE;
        }
        if (sdTimedOut(t0, SD_TO_CMD_MS))
        {
            sd_hwerr = SD_HWERR_TIMEOUT | SD_REG(SDHOST_RINTSTS);
            SD_REG(SDHOST_RINTSTS) = SD_IRQ_ALL;
            return FALSE;
        }
    }
}

/** Send one command on slot 0 (this driver's own slot). */
bool sdSendCmd(uint idx, uint32_t arg, uint resp, bool crc, uint32_t *r0)
{
    return sdSendCmdSlot(SD_SLOT, idx, arg, resp, crc, FALSE, r0);
}

/**
 * The SDHOST block register base as a word-indexable pointer.  The SDIO probe
 * in device/sdio-esp32p4 needs the few per-slot fields the command engine
 * does not parameterise (CTYPE width, CLKENA, CLKDIV) and reaches them through
 * here rather than duplicating the private register map.
 */
volatile uint32_t *sdHostRegBase(void)
{
    return (volatile uint32_t *)(uintptr_t)SDHOST_BASE;
}

/* ------------------------------------------------------------------ */

/* Data phase                                                         */
/* ------------------------------------------------------------------ */

/**
 * Reset the data FIFO.  The reset bit self-clears once the FIFO is empty
 * again; the engine must not be running at the time.
 */
static bool sdFifoReset(void)
{
    uint32_t t0;

    SD_REG(SDHOST_CTRL) |= SD_CTRL_FIFO_RESET;
    t0 = sdNow();
    while (SD_REG(SDHOST_CTRL) & SD_CTRL_FIFO_RESET)
    {
        if (sdTimedOut(t0, SD_TO_RESET_MS))
        {
            return FALSE;
        }
    }
    return TRUE;
}

/**
 * Reset the engine's own state (descriptor pointer, FSM).  Like the FIFO
 * reset, the bit clears itself; a transfer that gave up can leave the FSM
 * parked in SUSPEND, and this is what lifts it.
 */
static bool sdDmaReset(void)
{
    uint32_t t0;

    SD_REG(SDHOST_CTRL) |= SD_CTRL_DMA_RESET;
    t0 = sdNow();
    while (SD_REG(SDHOST_CTRL) & SD_CTRL_DMA_RESET)
    {
        if (sdTimedOut(t0, SD_TO_RESET_MS))
        {
            return FALSE;
        }
    }
    return TRUE;
}

/** Wait until the card lets go of DAT0. */static bool sdWaitDataIdle(void)
{
    uint32_t t0 = sdNow();

    while (SD_REG(SDHOST_STATUS) & SD_STATUS_DATA_BUSY)
    {
        if (sdTimedOut(t0, SD_TO_DATA_MS))
        {
            return FALSE;
        }
    }
    return TRUE;
}

/**
 * Move one 512 byte block between the bounce page and the card with \p cmd,
 * through the block's DMA engine.
 *
 * The ring is a single descriptor pointing at the bounce page: this block,
 * all 512 bytes, first and last, and the ring ends there.  The engine is
 * armed before the command, so it is already waiting for the data when the
 * card starts.  Direction is the command's own bit: the engine follows it.
 * It reports two things when it is done: RI (a received descriptor) or TI (a
 * sent one) in IDSTS, and DATA_OVER in RINTSTS when the card has finished
 * with the bus.
 *
 * High capacity cards are addressed by block number, the older byte
 * addressed ones by byte offset.
 */
static bool sdDmaBlock(uint8_t cmd, uint32_t lba, bool write)
{
    uint32_t cmdval;
    uint32_t raw;
    uint32_t ids;
    uint32_t done;
    uint32_t t0;

    sd_hwerr = 0;
    done = write ? SD_IDSTS_TI : SD_IDSTS_RI;

    /* Hand the one descriptor to the engine: this block, in the bounce
     * page, and no descriptor after it. */
    sd_desc->ctrl = SD_DESC_OWN | SD_DESC_FIRST | SD_DESC_LAST
                    | SD_DESC_SECOND_ADDR | SD_DESC_RING_END;
    sd_desc->sizes = SD_BLOCK_SIZE;
    sd_desc->buffer = (uint32_t)(uintptr_t)_sd_block_page;
    sd_desc->next = 0;

    /* Quiesce the data path: no card activity, no half-done engine. */
    if (!sdWaitDataIdle() || !sdWaitCmdTaken()
        || !sdFifoReset() || !sdDmaReset())
    {
        sd_hwerr = SD_HWERR_INHIBIT | SD_REG(SDHOST_STATUS);
        return FALSE;
    }

    /* Arm the engine: descriptor fetch on, its own interrupts on (RI/TI is
     * how completion is heard), fixed bursts, then the poll demand that
     * actually makes it read the descriptor. */
    SD_REG(SDHOST_CTRL) |= SD_CTRL_DMA_ENABLE;
    SD_REG(SDHOST_BMOD) = 0;
    SD_REG(SDHOST_BMOD) = SD_BMOD_SW_RESET;
    SD_REG(SDHOST_IDINTEN) = SD_IDSTS_RI | SD_IDSTS_TI | SD_IDSTS_NI;
    SD_REG(SDHOST_CTRL) |= SD_CTRL_DMA_ENABLE | SD_CTRL_USE_INTERNAL_DMA;
    SD_REG(SDHOST_BMOD) = SD_BMOD_ENABLE | SD_BMOD_FB;

    SD_REG(SDHOST_RINTSTS) = SD_IRQ_ALL;
    SD_REG(SDHOST_IDSTS) = SD_IDSTS_ALL;
    SD_REG(SDHOST_BLKSIZ) = SD_BLOCK_SIZE;
    SD_REG(SDHOST_BYTCNT) = SD_BLOCK_SIZE;
    SD_REG(SDHOST_CMDARG) = sd_host_hc ? lba : (lba * SD_BLOCK_SIZE);
    SD_REG(SDHOST_DBADDR) = (uint32_t)(uintptr_t)_sd_desc_page;
    SD_REG(SDHOST_PLDMND) = 1;

    cmdval = (uint32_t)(cmd & SD_CMD_INDEX_MASK)
             | SD_CMD_RESP_EXPECT | SD_CMD_RESP_CRC | SD_CMD_DATA_EXPECTED
             | SD_CMD_WAIT_COMPLETE | SD_CMD_USE_HOLD
             | (SD_SLOT << SD_CMD_CARD_NUM_POS);
    if (write)
    {
        cmdval |= SD_CMD_RW;
    }
    SD_REG(SDHOST_CMD) = cmdval | SD_CMD_START;

    if (!sdWaitCmdTaken())
    {
        sd_hwerr = SD_HWERR_INHIBIT | SD_REG(SDHOST_RINTSTS);
        SD_REG(SDHOST_BMOD) = 0;
        return FALSE;
    }

    /* The command completes first; the engine then moves the block. */
    t0 = sdNow();
    for (;;)
    {
        raw = SD_REG(SDHOST_RINTSTS);

        if (raw & SD_CMD_ERR_MASK)
        {
            SD_REG(SDHOST_RINTSTS) = raw;
            sd_hwerr = SD_HWERR_ERR | (raw & SD_CMD_ERR_MASK);
            SD_REG(SDHOST_BMOD) = 0;
            return FALSE;
        }
        if (raw & SD_IRQ_CMD_DONE)
        {
            uint32_t r1 = SD_REG(SDHOST_RESP(0));

            /* Keep DATA_OVER if it already arrived. */
            SD_REG(SDHOST_RINTSTS) = raw & ~SD_IRQ_DATA_OVER;
            if ((r1 & SD_R1_ERR_MASK) != 0)
            {
                /* The card refused the transfer (write protected, out of
                 * range, or not ready): no data phase will happen. */
                sd_hwerr = SD_HWERR_ERR | (r1 & SD_R1_ERR_MASK);
                SD_REG(SDHOST_BMOD) = 0;
                return FALSE;
            }
            break;
        }
        if (sdTimedOut(t0, SD_TO_CMD_MS))
        {
            SD_REG(SDHOST_RINTSTS) = SD_IRQ_ALL;
            sd_hwerr = SD_HWERR_TIMEOUT | raw;
            SD_REG(SDHOST_BMOD) = 0;
            return FALSE;
        }
    }

    /* Then the block: RI/TI says the descriptor finished, DATA_OVER that
     * the card is done with the bus. */
    t0 = sdNow();
    for (;;)
    {
        ids = SD_REG(SDHOST_IDSTS);
        raw = SD_REG(SDHOST_RINTSTS);

        if ((ids & SD_IDSTS_ERR_MASK) != 0 || (raw & SD_DATA_ERR_MASK) != 0)
        {
            sd_hwerr = SD_HWERR_DMA | ids | (raw & SD_DATA_ERR_MASK);
            break;
        }
        if ((ids & done) != 0 && (raw & SD_IRQ_DATA_OVER) != 0)
        {
            SD_REG(SDHOST_RINTSTS) = raw;
            SD_REG(SDHOST_IDSTS) = ids;
            SD_REG(SDHOST_BMOD) = 0;
            SD_REG(SDHOST_CTRL) &= ~(SD_CTRL_DMA_ENABLE
                                     | SD_CTRL_USE_INTERNAL_DMA);
            SD_REG(SDHOST_IDINTEN) = 0;
            /* The card must have let go of the bus before the next command
             * (a write keeps DAT0 low while it programs the block). */
            if (!sdWaitDataIdle())
            {
                sd_hwerr = SD_HWERR_TIMEOUT | SD_REG(SDHOST_STATUS);
                return FALSE;
            }
            return TRUE;
        }
        if (sdTimedOut(t0, SD_TO_DATA_MS))
        {
            sd_hwerr = SD_HWERR_TIMEOUT | ids;
            break;
        }
    }


    /* Either a descriptor error or a stuck data phase: park the engine and
     * take the data path down so the next transfer starts clean. */
    SD_REG(SDHOST_BMOD) = 0;
    SD_REG(SDHOST_CTRL) &= ~(SD_CTRL_DMA_ENABLE | SD_CTRL_USE_INTERNAL_DMA);
    SD_REG(SDHOST_IDINTEN) = 0;
    SD_REG(SDHOST_RINTSTS) = SD_IRQ_ALL;
    return FALSE;
}

/**
 * Read one 512 byte block with CMD17 into sd_bounce.
 *
 * The caller holds the SDMMC bus lock: the command and its data phase are
 * one transaction, and slot 1 must not slip between them.
 */
bool sdReadBlock(uint32_t lba)
{
    return sdDmaBlock(SD_CMD17_READ_SINGLE, lba, FALSE);
}

/** Write one block (CMD24) with whatever sdBlockCopyIn() staged. */
bool sdWriteBlock(uint32_t lba)
{
    return sdDmaBlock(SD_CMD24_WRITE_SINGLE, lba, TRUE);
}

/** Copy \p bytes out of the block the data phase just filled. */
void sdBlockCopyOut(void *dst, uint bytes)
{
    memcpy(dst, sd_bounce, bytes);
}

/** Copy \p bytes into the block the next write will send. */
void sdBlockCopyIn(const void *src, uint bytes)
{
    memcpy(sd_bounce, src, bytes);
}

/**
 * Devolver el camino de datos del bloque a un estado neutro cuando una
 * transferencia del slot 1 falla a mitad.
 *
 * El IDMA, el FIFO y el RINTSTS son recursos del BLOCK, no del slot: si un
 * CMD53 del C6 se va por timeout dejando el motor armed o la FIFO con restos,
 * el siguiente CMD17 de la microSD (slot 0) hereda ese estado y falla.  Medido
 * en placa: tras un CMD53 enquistado, 'ls' devolvia el directorio vacio porque
 * el primer bloque del directorio raiz no llegaba.  Por eso el abort hace aqui
 * lo mismo que la entrada de sdDmaBlock/sdTransferSlot: apagar la DMA
 * interna, resetear FIFO e IDMAC y limpiar todas las banderas.
 */
static void sdDataAbort(void)
{
    SD_REG(SDHOST_BMOD) = 0;
    SD_REG(SDHOST_CTRL) &= ~(SD_CTRL_DMA_ENABLE | SD_CTRL_USE_INTERNAL_DMA);
    SD_REG(SDHOST_IDINTEN) = 0;
    SD_REG(SDHOST_RINTSTS) = SD_IRQ_ALL;
    (void)sdFifoReset();
    (void)sdDmaReset();
}

/**
 * Fase de datos de un comando SDIO sobre \p slot, por la misma rebote de 512
 * bytes y el mismo IDMA que sdDmaBlock, pero con el comando, el argumento, el
 * tamano de bloque y el numero de bytes puestos por el llamante (CMD53 de la
 * funcion 1 del C6).  Es el sdDmaBlock del slot 0 generalizado:
 *
 *   - no reinterpreta la respuesta como R1 de tarjeta (SDIO responde R5/R6);
 *     aqui CMD_DONE solo confirma que el comando se acepto.
 *   - no aplica la transformacion LBA (el argumento CMD53 ya viene armado).
 *   - admite bytcnt <= 512 (byte-mode de una pasada; la rebote es de un sector).
 *
 * El llamante sostiene el candado del bus y prepara la rebote: para escribir
 * coloca los bytes con sdBlockCopyIn() antes de llamar; para leer los saca con
 * sdBlockCopyOut() despues de que devuelva TRUE.  \p blksiz es el tamano de
 * bloque del controlador (byte-mode: igual que bytcnt); \p write=TRUE manda
 * CMD.RW (host -> dispositivo).
 */
bool sdTransferSlot(uint32_t slot, uint idx, uint32_t arg, uint32_t bytcnt,
                    uint32_t blksiz, bool write)
{
    uint32_t cmdval;
    uint32_t raw;
    uint32_t ids;
    uint32_t done;
    uint32_t t0;

    if (bytcnt == 0 || bytcnt > SD_BLOCK_SIZE || blksiz == 0)
    {
        sd_hwerr = SD_HWERR_ERR;
        return FALSE;
    }

    sd_hwerr = 0;
    done = write ? SD_IDSTS_TI : SD_IDSTS_RI;

    /* Un solo descriptor: estos bytcnt bytes en la rebote, y la cadena acaba
     * aqui.  La direccion la entiende el motor (0x48 PSRAM), la CPU ve la
     * rebote por la ventana directa sin caché (0x88). */
    sd_desc->ctrl = SD_DESC_OWN | SD_DESC_FIRST | SD_DESC_LAST
                    | SD_DESC_SECOND_ADDR | SD_DESC_RING_END;
    sd_desc->sizes = bytcnt;
    sd_desc->buffer = (uint32_t)(uintptr_t)_sd_block_page;
    sd_desc->next = 0;

    /* Limpieza previa del camino de datos.  Depende del slot:
     *   - Slot 0 (microSD): exige bus libre ANTES de armar.  Es el camino que
     *     monta la FAT en el arranque; aqui el slave nunca mantiene DAT0
     *     afirmado, asi que la espera es barata y su resultado vinculante.
     *   - Slot 1 (esp_hosted en el C6): el slave mantiene DAT0 a bajo mientras
     *     tiene un paquete C2H sin leer, y eso es data_busy en el STATUS.  Si
     *     aqui se exigiera bus libre, todo CMD53 abortaria antes de lanzarse
     *     (chicken-and-egg: solo un CMD53 que lea la FIFO consume el paquete y
     *     libera DAT0).  Por eso en el slot 1 el wait de entrada es lo mejor
     *     posible (se ignora su resultado): el reset de FIFO y DMA que sigue
     *     limpia el camino del host de todas formas.  El gate del FINAL es
     *     estricto en ambos slots: es lo que impide dar por buena una
     *     transferencia que en realidad no movio dato. */
    if (slot == 1U)
    {
        (void)sdWaitDataIdle();
        if (!sdWaitCmdTaken() || !sdFifoReset() || !sdDmaReset())
        {
            sd_hwerr = SD_HWERR_INHIBIT | SD_REG(SDHOST_STATUS);
            return FALSE;
        }
    }
    else
    {
        if (!sdWaitDataIdle() || !sdWaitCmdTaken()
            || !sdFifoReset() || !sdDmaReset())
        {
            sd_hwerr = SD_HWERR_INHIBIT | SD_REG(SDHOST_STATUS);
            return FALSE;
        }
    }

    /* Armar el motor: fetch de descriptor, sus interrupciones (RI/TI es como
     * se escucha el fin), burst fijo, y el poll demand que lo pone a leer el
     * descriptor. */
    SD_REG(SDHOST_CTRL) |= SD_CTRL_DMA_ENABLE;
    SD_REG(SDHOST_BMOD) = 0;
    SD_REG(SDHOST_BMOD) = SD_BMOD_SW_RESET;
    SD_REG(SDHOST_IDINTEN) = SD_IDSTS_RI | SD_IDSTS_TI | SD_IDSTS_NI;
    SD_REG(SDHOST_CTRL) |= SD_CTRL_DMA_ENABLE | SD_CTRL_USE_INTERNAL_DMA;
    SD_REG(SDHOST_BMOD) = SD_BMOD_ENABLE | SD_BMOD_FB;

    SD_REG(SDHOST_RINTSTS) = SD_IRQ_ALL;
    SD_REG(SDHOST_IDSTS) = SD_IDSTS_ALL;
    SD_REG(SDHOST_BLKSIZ) = blksiz;
    SD_REG(SDHOST_BYTCNT) = bytcnt;
    SD_REG(SDHOST_CMDARG) = arg;
    SD_REG(SDHOST_DBADDR) = (uint32_t)(uintptr_t)_sd_desc_page;
    SD_REG(SDHOST_PLDMND) = 1;

    cmdval = (uint32_t)(idx & SD_CMD_INDEX_MASK)
             | SD_CMD_RESP_EXPECT | SD_CMD_RESP_CRC | SD_CMD_DATA_EXPECTED
             | SD_CMD_WAIT_COMPLETE | SD_CMD_USE_HOLD
             | ((slot & 0x1FU) << SD_CMD_CARD_NUM_POS);
    if (write)
    {
        cmdval |= SD_CMD_RW;
    }
    SD_REG(SDHOST_CMD) = cmdval | SD_CMD_START;

    if (!sdWaitCmdTaken())
    {
        sd_hwerr = SD_HWERR_INHIBIT | SD_REG(SDHOST_RINTSTS);
        sdDataAbort();
        return FALSE;
    }

    /* El comando se acepta primero (CMD_DONE, sin comprobar R1: es SDIO); el
     * motor mueve el dato despues. */
    t0 = sdNow();
    for (;;)
    {
        raw = SD_REG(SDHOST_RINTSTS);

        if (raw & SD_CMD_ERR_MASK)
        {
            SD_REG(SDHOST_RINTSTS) = raw;
            sd_hwerr = SD_HWERR_ERR | (raw & SD_CMD_ERR_MASK);
            sdDataAbort();
            return FALSE;
        }
        if (raw & SD_IRQ_CMD_DONE)
        {
            SD_REG(SDHOST_RINTSTS) = raw & ~SD_IRQ_DATA_OVER;
            break;
        }
        if (sdTimedOut(t0, SD_TO_CMD_MS))
        {
            SD_REG(SDHOST_RINTSTS) = SD_IRQ_ALL;
            sd_hwerr = SD_HWERR_TIMEOUT | raw;
            sdDataAbort();
            return FALSE;
        }
    }

    /* Después el bloque: RI/TI dice que el descriptor terminó; DATA_OVER que
     * el dispositivo ya soltó el bus. */
    t0 = sdNow();
    for (;;)
    {
        ids = SD_REG(SDHOST_IDSTS);
        raw = SD_REG(SDHOST_RINTSTS);

        if ((ids & SD_IDSTS_ERR_MASK) != 0 || (raw & SD_DATA_ERR_MASK) != 0)
        {
            sd_hwerr = SD_HWERR_DMA | ids | (raw & SD_DATA_ERR_MASK);
            break;
        }
        if ((ids & done) != 0 && (raw & SD_IRQ_DATA_OVER) != 0)
        {
            SD_REG(SDHOST_RINTSTS) = raw;
            SD_REG(SDHOST_IDSTS) = ids;
            SD_REG(SDHOST_BMOD) = 0;
            SD_REG(SDHOST_CTRL) &= ~(SD_CTRL_DMA_ENABLE
                                     | SD_CTRL_USE_INTERNAL_DMA);
            SD_REG(SDHOST_IDINTEN) = 0;
            if (!sdWaitDataIdle())
            {
                sd_hwerr = SD_HWERR_TIMEOUT | SD_REG(SDHOST_STATUS);
                return FALSE;
            }
            return TRUE;
        }
        /* Relanzar el motor si se quedó suspendido esperando un pedido de
         * FIFO (igual que el poll-demand del driver de referencia). */
        {
            uint32_t fsm = SD_IDSTS_FSM(ids);

            if (fsm == SD_DMA_FSM_SUSPEND || fsm == SD_DMA_FSM_RD_REQ_WAIT
                || fsm == SD_DMA_FSM_WR_REQ_WAIT)
            {
                SD_REG(SDHOST_PLDMND) = 1;
            }
        }
        if (sdTimedOut(t0, SD_TO_DATA_MS))
        {
            sd_hwerr = SD_HWERR_TIMEOUT | ids;
            break;
        }
    }

    /* El camino de datos es del bloque, no del slot: dejarlo neutro para que
     * la microSD (slot 0) no herede el motor a medio armar. */
    sdDataAbort();
    return FALSE;
}
