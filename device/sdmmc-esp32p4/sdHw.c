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

/**
 * Ask the controller to push the clock registers out to the card clock
 * generation unit.  The update command has no card index and no response;
 * the hardware sets HLE on success, which is expected and needs a retry.
 */
static bool sdClockUpdate(void)
{
    uint tries;

    for (tries = 0; tries < 16; tries++)
    {
        uint32_t t0;

        if (!sdWaitCmdTaken())
        {
            return FALSE;
        }

        SD_REG(SDHOST_RINTSTS) = SD_IRQ_ALL;
        SD_REG(SDHOST_CMDARG) = 0;
        SD_REG(SDHOST_CMD) = SD_CMD_UPDATE_CLK | SD_CMD_WAIT_COMPLETE
                             | SD_CMD_USE_HOLD | (SD_SLOT << SD_CMD_CARD_NUM_POS)
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
                return FALSE;
            }
            if (0 == (SD_REG(SDHOST_CMD) & SD_CMD_START))
            {
                SD_REG(SDHOST_RINTSTS) = SD_IRQ_ALL;
                return TRUE;
            }
            if (sdTimedOut(t0, SD_TO_CMD_MS))
            {
                return FALSE;
            }
        }
    }
    return FALSE;
}

/**
 * Set this slot's card clock divider.  The global divider that feeds both
 * slots was fixed at controller init and is not touched here.
 */
bool sdSetClock(uint8_t card_div)
{
    /* Stop the card clock before touching the dividers. */
    SD_REG(SDHOST_CLKENA) &= ~SD_CLKENA_CARD0;
    if (!sdClockUpdate())
    {
        return FALSE;
    }

    /* Slot 0 reads divider 0 (CLKSRC.card0 = 0) and divides by 2*n. */
    SD_REG(SDHOST_CLKSRC) = (SD_REG(SDHOST_CLKSRC)
                             & ~0x3U) | SD_CLKSRC_CARD0_DIV0;
    SD_REG(SDHOST_CLKDIV) = (SD_REG(SDHOST_CLKDIV)
                             & ~SD_CLKDIV_CARD0_MASK) | card_div;

    if (!sdClockUpdate())
    {
        return FALSE;
    }

    SD_REG(SDHOST_CLKENA) |= SD_CLKENA_CARD0;
    if (!sdClockUpdate())
    {
        return FALSE;
    }

    SD_REG(SDHOST_CLKENA) |= SD_CLKENA_LOW_POWER0;

    sd_clk_khz = (uint32_t)(SD_HOST_HZ /
                            ((card_div == 0) ? 1UL : (uint32_t)card_div * 2UL)
                            / 1000UL);
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
    uint32_t edge;

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

    /* Global divider: the period is (l + 1) source cycles and the high
     * phase (h + 1) of them, so SD_HOST_DIV = 4 gives 40 MHz at 50 %. */
    edge = sdRegRead(P4_CLKRST_PERI_CTRL02, 0);
    edge &= ~((P4_EDGE_MASK << P4_SDIO_LS_EDGE_L_POS)
              | (P4_EDGE_MASK << P4_SDIO_LS_EDGE_H_POS)
              | (P4_EDGE_MASK << P4_SDIO_LS_EDGE_N_POS));
    edge |= ((SD_HOST_DIV / 2 - 1) & P4_EDGE_MASK) << P4_SDIO_LS_EDGE_H_POS;
    edge |= ((SD_HOST_DIV - 1) & P4_EDGE_MASK) << P4_SDIO_LS_EDGE_L_POS;
    edge |= ((SD_HOST_DIV - 1) & P4_EDGE_MASK) << P4_SDIO_LS_EDGE_N_POS;
    sdRegWrite(P4_CLKRST_PERI_CTRL02, 0, edge);

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
 * Let a bus master reach the L2 cache block.  It comes out of reset with
 * DMA access shut (CACHE_L2_CACHE_SHUT_DMA = 1), and a master then reads
 * zeros and its writes are dropped.  ESP-IDF clears it on every boot
 * through the ROM's Cache_Enable_L2_Cache(); that call leaves this bit
 * alone here, so the register is written directly.
 */
#define SD_L2_CACHE_BASE     0x3FF10000UL   /* soc/cache_reg.h       */
#define SD_L2_CACHE_CTRL     0x270U
#define SD_L2_SHUT_DMA       (1UL << 4)

static void sdEngineBusOpen(void)
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

    sdEngineBusOpen();

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
 * Send one command and, if it expects a response, wait for it.
 *
 * \p resp is 0 (none), 1 (short, 48 bit) or 2 (long, 136 bit).
 * \p crc asks the controller to verify the response CRC, which only
 * makes sense for the commands whose responses actually carry one.
 */
bool sdSendCmd(uint idx, uint32_t arg, uint resp, bool crc, uint32_t *r0)
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
          | (SD_SLOT << SD_CMD_CARD_NUM_POS);
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
