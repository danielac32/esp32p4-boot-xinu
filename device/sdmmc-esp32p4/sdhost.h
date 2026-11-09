/**
 * @file sdhost.h
 *
 * Register map, board wiring and shared state of the ESP32-P4 SDMMC
 * (SDHOST) host device.  The card's public interface is in <sdblock.h>; this
 * header is private to the host layer (device/sdmmc-esp32p4), which owns the
 * pads, the clock tree, the command engine and the IDMAC.
 *
 * Register facts checked against ESP-IDF for the ESP32-P4:
 *   soc/sdmmc_struct.h              (CTRL/CLKDIV/CLKSRC/CLKENA/CTYPE/CMD/
 *                                     STATUS/RINTSTS/BUFFIFO/rst_n fields
 *                                     and the 0x810 byte block layout)
 *   hal/esp32p4/include/hal/sdmmc_ll.h
 *                                   (clock tree order, global divider math,
 *                                     IOMUX_FUNC 0, slot 0 has no GPIO
 *                                     matrix)
 *   soc/hp_sys_clkrst_struct.h      (sdmmc_sys_clk_en, sdio_ls_* dividers)
 *   soc/lp_clkrst_struct.h          (sdio pll enables, sdmmc module reset)
 *   soc/pmu_reg.h                   (SDIO PLL power domains)
 *   esp_driver_sdmmc/src/sdmmc_host.c (per-pin IOMUX setup)
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _SDHOST_H_
#define _SDHOST_H_

#include <platform.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <board.h>
#include <esp32p4.h>
#include <sdmmc.h>

/* ------------------------------------------------------------------ */
/* Board wiring                                                        */
/* ------------------------------------------------------------------ */
/* Slot 0 is the microSD socket.  Its six lines are fixed-function SDMMC
 * pads on this chip (the slot has no GPIO matrix route), so a pad is put
 * on the bus by clearing its mcu_sel; see sdPadsInit(), which also keeps
 * DAT3 high as a plain output until the bus is really widened.
 *
 * There is no slot power GPIO: the socket VDD (and the IO supply of those
 * pads) is the on-chip LDO output VO4, switched on by sdSlotPowerOn().
 */
#define SD_PIN_CLK      SD_CLK      /* 43 */
#define SD_PIN_CMD      SD_CMD      /* 44 */
#define SD_PIN_D0       SD_D0       /* 39 */
#define SD_PIN_D1       SD_D1       /* 40 */
#define SD_PIN_D2       SD_D2       /* 41 */
#define SD_PIN_D3       SD_D3       /* 42 */

#define SD_SLOT         0U          /* this driver owns slot 0 only        */

/* ------------------------------------------------------------------ */
/* Clock plan                                                          */
/* ------------------------------------------------------------------ */
/* The SDMMC block runs from the SDIO PLL at 160 MHz.  ONE global divider
 * (HP_SYS_CLKRST.peri_clk_ctrl02) feeds the block, so it is programmed once at
 * controller init and never touched again.  Inside the block CLKDIV holds FOUR
 * independent dividers (one byte each) and CLKSRC binds each slot to one of
 * them; this driver gives each slot its own: slot 0 -> divider 0, slot 1 ->
 * divider 1 (same convention as sdmmc_ll_set_card_clock_div in the SDK).
 *
 *   f_sdio_ls = 160 MHz / SD_HOST_DIV          = 40 MHz, both slots
 *   f_card    = f_sdio_ls / (card_div ? 2 * card_div : 1)
 *
 *   identification: card_div 50 -> 400 kHz   (spec SD: <= 400 kHz)
 *   working:        card_div 1  -> 20 MHz    (HS mode, no tuning needed)
 */
#define SD_CLK_SRC_HZ       160000000UL
#define SD_HOST_DIV         4U
#define SD_HOST_HZ          (SD_CLK_SRC_HZ / SD_HOST_DIV)
#define SD_CARD_DIV_PROBE   50U
#define SD_CARD_DIV_WORK    1U

/* ------------------------------------------------------------------ */
/* SDHOST register map (one block, two slots)                          */
/* ------------------------------------------------------------------ */
#define SDHOST_CTRL        0x000
/* 0x004 is reserved on this chip: unlike the S3, the P4's SDHOST block has
 * no power-enable register; slot VDD comes from the on-chip LDO VO4. */
#define SDHOST_CLKDIV      0x008
#define SDHOST_CLKSRC      0x00C
#define SDHOST_CLKENA      0x010
#define SDHOST_TMOUT       0x014
#define SDHOST_CTYPE       0x018
#define SDHOST_BLKSIZ      0x01C
#define SDHOST_BYTCNT      0x020
#define SDHOST_INTMASK     0x024
#define SDHOST_CMDARG      0x028
#define SDHOST_CMD         0x02C
#define SDHOST_RESP(n)     (0x030 + (4 * (n)))
#define SDHOST_MINTSTS     0x040
#define SDHOST_RINTSTS     0x044
#define SDHOST_STATUS      0x048
#define SDHOST_FIFOTH      0x04C
#define SDHOST_CDETECT     0x050
#define SDHOST_WRTPRT      0x054
/* Bytes moved by the IDMAC since the last arm: the card side and the
 * engine side.  Equal values mean the block actually crossed memory. */
#define SDHOST_TCBCNT      0x05C
#define SDHOST_TBBCNT      0x060
#define SDHOST_DEBNCE      0x064
#define SDHOST_USRID       0x068
#define SDHOST_VERID       0x06C
#define SDHOST_HCON        0x070
#define SDHOST_UHS         0x074
#define SDHOST_RST_N       0x078
#define SDHOST_BMOD        0x080
#define SDHOST_PLDMND      0x084
#define SDHOST_DBADDR      0x088
#define SDHOST_IDSTS       0x08C
#define SDHOST_IDINTEN     0x090
#define SDHOST_DSCADDR     0x094
#define SDHOST_BUFADDR     0x098
/* The data port.  Only a CPU-driven (FIFO mode) transfer uses it; this
 * driver hands every block to the IDMAC and never touches it. */
#define SDHOST_BUFFIFO     0x200
/* Phase delays and the global clock divider live in the system controller,
 * not in this block. */
#define SDHOST_CLKEDGE     0x800

#define SD_REG(off)     (*(volatile uint32_t *)(SDHOST_BASE + (off)))

/* CTRL */
#define SD_CTRL_CONTROLLER_RESET   (1U << 0)
#define SD_CTRL_FIFO_RESET         (1U << 1)
#define SD_CTRL_DMA_RESET          (1U << 2)
#define SD_CTRL_INT_ENABLE         (1U << 4)
#define SD_CTRL_DMA_ENABLE         (1U << 5)
#define SD_CTRL_USE_INTERNAL_DMA   (1U << 25)

/* BMOD: the DMA engine of the block (shared by both slots). */
#define SD_BMOD_SW_RESET           (1U << 0)
#define SD_BMOD_FB                 (1U << 1)     /* fixed bursts           */
#define SD_BMOD_ENABLE             (1U << 7)     /* IDMAC on               */

/* IDSTS: the engine's completion and error lines; polled, never wired to
 * an interrupt here.  TI/RI/NI say the descriptor finished, NI is the
 * summary of the other two. */
#define SD_IDSTS_TI                (1U << 0)
#define SD_IDSTS_RI                (1U << 1)
#define SD_IDSTS_FBE               (1U << 2)
#define SD_IDSTS_DU                (1U << 4)
#define SD_IDSTS_CES               (1U << 5)
#define SD_IDSTS_NI                (1U << 8)
#define SD_IDSTS_AIS               (1U << 9)
#define SD_IDSTS_ALL               0x0001FFFFUL
#define SD_IDSTS_DONE_MASK         (SD_IDSTS_TI | SD_IDSTS_RI | SD_IDSTS_NI)
#define SD_IDSTS_ERR_MASK          (SD_IDSTS_FBE | SD_IDSTS_DU | SD_IDSTS_CES \
                                    | SD_IDSTS_AIS)

/* IDSTS[16:13] is the engine's state machine.  In polled use it parks
 * itself in SUSPEND (or waiting for a FIFO request) between steps and
 * only another poll demand moves it on. */
#define SD_IDSTS_FSM(s)            (((s) >> 13) & 0xFU)
#define SD_DMA_FSM_SUSPEND         1U
#define SD_DMA_FSM_RD_REQ_WAIT     4U
#define SD_DMA_FSM_WR_REQ_WAIT     5U

/* CLKDIV: one byte per slot, the divisor is 2*n and n = 0 bypasses. */
#define SD_CLKDIV_CARD0_MASK       0xFFU

/* CLKSRC: two bits per slot select which divider output feeds the slot. */
#define SD_CLKSRC_CARD0_DIV0       0U

/* CLKENA */
#define SD_CLKENA_CARD0            (1U << SD_SLOT)
#define SD_CLKENA_LOW_POWER0       (1U << (16 + SD_SLOT))

/* CMD */
#define SD_CMD_INDEX_MASK          0x3FU
#define SD_CMD_RESP_EXPECT         (1U << 6)
#define SD_CMD_RESP_LONG           (1U << 7)
#define SD_CMD_RESP_CRC            (1U << 8)
#define SD_CMD_DATA_EXPECTED       (1U << 9)
#define SD_CMD_RW                  (1U << 10)   /* 1 = write to card  */
#define SD_CMD_STREAM              (1U << 11)
#define SD_CMD_SEND_AUTO_STOP      (1U << 12)
#define SD_CMD_WAIT_COMPLETE       (1U << 13)
#define SD_CMD_STOP_ABORT          (1U << 14)
#define SD_CMD_SEND_INIT           (1U << 15)
#define SD_CMD_CARD_NUM_POS        16          /* [20:16]: the slot      */
#define SD_CMD_UPDATE_CLK          (1U << 21)
#define SD_CMD_USE_HOLD            (1U << 29)
#define SD_CMD_START               (1U << 31)

/* STATUS (sdhost_status_reg_t del SDK, hw_ver3): no es el mapa DesignWare
 * clasico, son campos propios.  Decodificados en placa durante W2: el valor
 * 0x0001a306 que queda clavado cuando el update-clock se enquista significa
 * fifo_tx_watermark=1, fifo_empty=1, fsm=0 (reposo), data_3_status=1 (hay
 * tarjeta), data_busy=1 (DAT0 a nivel bajo, mantenido por el esclavo) y
 * response_index=52.  */
#define SD_STATUS_FIFO_EMPTY       (1U << 2)     /* FIFO sin datos           */
#define SD_STATUS_FIFO_FULL        (1U << 3)     /* FIFO llena               */
#define SD_STATUS_FSM_MASK         (0xFU << 4)   /* [7:4] maquina de comando */
#define SD_STATUS_FSM_POS          4
#define SD_STATUS_CARD_PRESENT     (1U << 8)     /* DAT3 al to = hay tarjeta */
#define SD_STATUS_DATA_BUSY        (1U << 9)     /* DAT0 still driven low  */
#define SD_STATUS_DAT_PATH_BUSY    (1U << 10)    /* FSM de datos ocupada     */
#define SD_STATUS_RESP_IDX_MASK    (0x3FU << 11) /* [16:11] idx ultimo resp  */
#define SD_STATUS_RESP_IDX_POS     11
#define SD_STATUS_FIFO_COUNT_MASK  (0x1FFFU << 17) /* [29:17] palabras FIFO  */
#define SD_STATUS_FIFO_COUNT_POS   17

/* RINTSTS bits, same order as INTMASK */
#define SD_IRQ_CD                  (1U << 0)
#define SD_IRQ_RESP_ERR            (1U << 1)
#define SD_IRQ_CMD_DONE            (1U << 2)
#define SD_IRQ_DATA_OVER           (1U << 3)
#define SD_IRQ_TXDR                (1U << 4)
#define SD_IRQ_RXDR                (1U << 5)
#define SD_IRQ_RCRC                (1U << 6)
#define SD_IRQ_DCRC                (1U << 7)
#define SD_IRQ_RTO                 (1U << 8)
#define SD_IRQ_DRTO                (1U << 9)
#define SD_IRQ_HTO                 (1U << 10)
#define SD_IRQ_FRUN                (1U << 11)
#define SD_IRQ_HLE                 (1U << 12)
#define SD_IRQ_SBE                 (1U << 13)
#define SD_IRQ_ACD                 (1U << 14)
#define SD_IRQ_EBE                 (1U << 15)

#define SD_IRQ_ALL                 0xFFFFFFFFUL

/* Errors that end a command phase. */
#define SD_CMD_ERR_MASK \
    (SD_IRQ_RTO | SD_IRQ_RCRC | SD_IRQ_HLE | SD_IRQ_RESP_ERR)

/* Errors that end a data phase. */
#define SD_DATA_ERR_MASK \
    (SD_IRQ_EBE | SD_IRQ_SBE | SD_IRQ_DCRC | SD_IRQ_DRTO | SD_IRQ_HTO | \
     SD_IRQ_FRUN | SD_IRQ_HLE)

/* CTYPE: one bit pair per slot; bit 0 of the pair = 4-bit bus. */
#define SD_CTYPE_WIDTH4_CARD0      (1U << SD_SLOT)

/* rst_n: 1 = active (not held in reset); the field already powers up at 1,
 * so the only reset this driver uses is the protocol one (CMD0). */
#define SD_RST_N_ACTIVE            0x3U

/* ------------------------------------------------------------------ */
/* DMA memory and the SoC clock tree                                  */
/* ------------------------------------------------------------------ */
/* The SDHOST IDMAC is not snoop-fed: it reads and writes memory itself,
 * behind the P4's caches, so everything the engine touches has to be
 * maintained by hand.  That maintenance lives in sdHw.c next to the code
 * that needs it (L1D writeback of the descriptor before the fetch,
 * invalidate of the block before the CPU reads it). */

#define P4_CLKRST_SOC_CTRL1        (HP_SYS_CLKRST_BASE + 0x18UL)
#define P4_CLKRST_PERI_CTRL01      (HP_SYS_CLKRST_BASE + 0x34UL)
#define P4_CLKRST_PERI_CTRL02      (HP_SYS_CLKRST_BASE + 0x38UL)
#define P4_CLKRST_HP_CLK_CTRL      (LP_AON_CLKRST_BASE + 0x40UL)
#define P4_CLKRST_SDMMC_EMAC_RST   (LP_AON_CLKRST_BASE + 0x4CUL)
#define P4_PMU_IMM_HP_CK_POWER     (PMU_BASE + 0xCCUL)
#define P4_PMU_RF_PWC              (PMU_BASE + 0x15CUL)

#define P4_SDMMC_SYS_CLK_EN        (1UL << 14)   /* SOC_CLK_CTRL1   */
#define P4_SDIO_HS_MODE            (1UL << 22)   /* PERI_CLK_CTRL01 */
#define P4_SDIO_LS_SRC_SEL         (1UL << 23)   /* 0 = PLL160M     */
#define P4_SDIO_LS_CLK_EN          (1UL << 24)   /* PERI_CLK_CTRL01 */
#define P4_SDIO_LS_EDGE_UPDATE     (1UL << 8)    /* PERI_CLK_CTRL02, write trigger */
#define P4_SDIO_LS_EDGE_L_POS      9             /* [12:9]  */
#define P4_SDIO_LS_EDGE_H_POS      13            /* [16:13] */
#define P4_SDIO_LS_EDGE_N_POS      17            /* [20:17] */
#define P4_SDIO_LS_DRV_EDGE_POS    23            /* [24:23] */
#define P4_SDIO_LS_SAM_EDGE_POS    25            /* [26:25] */
#define P4_SDIO_LS_SLF_EDGE_POS    21            /* [22:21] */
#define P4_SDIO_LS_SLF_CLK_EN      (1UL << 27)
#define P4_SDIO_LS_DRV_CLK_EN      (1UL << 28)
#define P4_SDIO_LS_SAM_CLK_EN      (1UL << 29)
#define P4_EDGE_MASK               0xFUL

#define P4_SDIO_PLL0_CLK_EN        (1UL << 23)   /* HP_CLK_CTRL      */
#define P4_SDIO_PLL1_CLK_EN        (1UL << 22)
#define P4_SDIO_PLL2_CLK_EN        (1UL << 21)
#define P4_RST_EN_SDMMC            (1UL << 28)   /* SDMMC_EMAC_RST   */

#define P4_PMU_SDIO_PLL_XPD        (1UL << 25)   /* RF_PWC                 */
#define P4_PMU_XPD_SDIOPLL_I2C     (1UL << 26)   /* IMM_HP_CK_POWER        */
#define P4_PMU_XPD_SDIOPLL         (1UL << 30)
#define P4_PMU_GLOBAL_SDIOPLL_ICG  (1UL << 20)

/* ------------------------------------------------------------------ */
/* On-chip IO LDO that feeds the microSD slot (VDD = LDO_VO4)          */
/* ------------------------------------------------------------------ */
/* The P4 has four software-controlled external LDO units, VO1..VO4.  Each
 * one is a (mode, rail) pair of PMU.ext_ldo[] registers, and the pairing is
 * not the obvious one: VO1->[0], VO2->[3], VO3->[1], VO4->[4] (this is the
 * table ESP-IDF's ldo_ll uses).  With a 4-word stride starting at 0x1B8,
 * VO4 sits at 0x1D8 (control) and 0x1DC (analog).
 *
 * The SD card runs at 3.3 V, which is the LDO input rail itself, so the
 * unit is put in bypass (tieh = 1: "tieh: 0 = Vref * Mul, 1 = 3.3 V") and
 * software ownership is forced so the tieh bit is honoured
 * (force_tieh_sel = 1, tieh_sel = 0).
 */
#define P4_PMU_LDO_VO4_CTRL        (PMU_BASE + 0x1D8UL)
#define P4_PMU_LDO_VO4_ANA         (PMU_BASE + 0x1DCUL)
/* ctrl fields: force_tieh_sel=7, xpd=8, tieh_sel=9..11, pos/neg delay
 * 12..13, tieh=14 */
#define P4_PMU_LDO_FORCE_TIEH_SEL  (1UL << 7)
#define P4_PMU_LDO_XPD             (1UL << 8)
#define P4_PMU_LDO_TIEH_3V3        (1UL << 14)
/* ana fields: mul=23..25, en_vdet=26 (ripple suppression), en_cur_lim=27,
 * dref=28..31; dref/mul are ignored while bypassing to the rail. */
#define P4_PMU_LDO_EN_VDET         (1UL << 26)

/* ------------------------------------------------------------------ */
/* GPIO / IO_MUX (only what this device needs)                         */
/* ------------------------------------------------------------------ */
#define P4_GPIO_OUT1_W1TS          (GPIO_BASE + 0x14UL)
#define P4_GPIO_OUT1_W1TC          (GPIO_BASE + 0x18UL)
#define P4_GPIO_ENABLE1_W1TS       (GPIO_BASE + 0x30UL)
#define P4_GPIO_ENABLE1_W1TC       (GPIO_BASE + 0x34UL)
/* FUNCn_OUT_SEL_CFG[pin]: OUT_SEL[8:0], INV[9], OEN_SEL[10] */
#define P4_GPIO_OUT_SEL(pin)       (GPIO_BASE + 0x558UL + 4UL * (pin))
#define P4_GPIO_OUT_SEL_MASK       0x1FFUL
#define P4_GPIO_OUT_SEL_GPIO       256UL         /* pin follows GPIO_OUT */
#define P4_GPIO_OEN_SEL            (1UL << 10)

/* IO_MUX pad register n lives at base + 4 + 4*n.  Field bits: fun_wpd=7,
 * fun_wpu=8, fun_ie=9, fun_drv=10..11, mcu_sel=12..14.  mcu_sel 0 is the
 * pad's direct (fixed) function, which for pins 39..44 is SDMMC. */
#define P4_IOPAD(pin)              (IO_MUX_BASE + 4UL + 4UL * (pin))
#define P4_IO_FUN_WPD              (1UL << 7)
#define P4_IO_FUN_WPU              (1UL << 8)
#define P4_IO_FUN_IE               (1UL << 9)
#define P4_IO_FUN_DRV_MASK         (3UL << 10)
#define P4_IO_FUN_DRV_40MA         (3UL << 10)
#define P4_IO_MCU_SEL_MASK         (7UL << 12)
#define P4_IO_MCU_SEL_GPIO         (1UL << 12)

/* Timeouts, in milliseconds. */
#define SD_TO_CMD_MS               250
#define SD_TO_DATA_MS              2000
#define SD_TO_RESET_MS             500
#define SD_TO_BUSY_MS              1000

/* ------------------------------------------------------------------ */
/* Host state (defined in sdHw.c)                                      */
/* ------------------------------------------------------------------ */
extern uint32_t sd_hwerr;           /* why the last transaction failed */
extern uint32_t sd_clk_khz;         /* card clock in kHz               */
extern uint32_t sd_host_khz;        /* reloj global del bloque, en kHz  */
extern uint32_t sd_clkdbg;          /* sdSetClockSlot failure: (paso<<24)|
                                     * (motivo<<20)|reintentos, 0 si OK     */
extern uint32_t sd_clkdbg_reg;      /* RINTSTS capturado en ese fallo       */
extern uint32_t sd_clkdbg_cmd;      /* SDHOST_CMD capturado en ese fallo    */
extern uint32_t sd_clkdbg_status;   /* SDHOST_STATUS (inhibit) en el fallo  */
extern uint32_t sd_clkdbg_clkena;   /* SDHOST_CLKENA en el fallo            */
extern uint32_t sd_host_lba;        /* cursor for the SDMMC0 device    */
extern uint8_t sd_host_width;       /* data lines driven (1 or 4)      */
extern bool sd_host_ready;          /* controller initialised          */
extern bool sd_host_hc;             /* card addressed by block number  */

/**
 * Traza de la fase de datos (diagnostico del CMD53 del slot 1).  Apagada por
 * defecto; la enciende el comando `wifi dma`.  Mientras esta a 1, sdTransferSlot
 * vuelca en cada punto de la secuencia los registros del motor, el descriptor,
 * el R5 del comando y el nivel de las patas del slot 1, todo leido del
 * periferico.  sd_xfer_r5 guarda RESP0..RESP3 del ultimo CMD_DONE.
 */
extern uint32_t sd_xfer_trace;
extern uint32_t sd_xfer_r5[4];
extern uint32_t sd_xfer_no_wait;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static inline uint32_t sdRegRead(uint32_t base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}

static inline void sdRegWrite(uint32_t base, uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(base + off) = val;
}

static inline void sdRegOr(uint32_t base, uint32_t off, uint32_t bits)
{
    *(volatile uint32_t *)(base + off) |= bits;
}

static inline void sdRegClear(uint32_t base, uint32_t off, uint32_t bits)
{
    *(volatile uint32_t *)(base + off) &= ~bits;
}

/**
 * Copy the 136-bit response the controller just received.  The hardware
 * stores the words bottom-up (RESP(0) holds the least significant 32 bits),
 * so read them in reverse to get the register as the card numbers it.
 */
static inline void sdLongResp(uint32_t *dst)
{
    dst[0] = SD_REG(SDHOST_RESP(3));
    dst[1] = SD_REG(SDHOST_RESP(2));
    dst[2] = SD_REG(SDHOST_RESP(1));
    dst[3] = SD_REG(SDHOST_RESP(0));
}

/* ------------------------------------------------------------------ */
/* Hardware layer (sdHw.c)                                             */
/* ------------------------------------------------------------------ */
void sdPadsInit(void);

/** Hand DAT3 over to the controller now that the card is on four lines. */
void sdPadsWiden(void);

/** Put the controller on \p width data lines (1 or 4). */
bool sdSetWidth(uint width);

bool sdControllerInit(void);

/**
 * Forzar el reset de protocolo del bloque (CTRL: controller + FIFO + DMA) y
 * esperar a que los tres bits se auto-limpien.  Es la UNICA cosa que saca
 * reposada la maquina de estados de datos cuando una fase de datos muere a
 * medias (CMD53 sin respuesta del esclavo): con STATUS.data_state_mc_busy en 1
 * el "update clock" queda inhibido para los DOS slots, asi que ni la microSD
 * puede cambiar de reloj.  sdControllerInit no lo hace si los bits de CTRL
 * vienen bajados, que es justo como los deja un fallo.
 */
bool sdBlockReset(void);

/**
 * Fijar el divisor GLOBAL sdio_ls (1..16) del que salen los dos slots: 4 deja
 * 40 MHz (lo que usa la microSD) y 7 deja 22,857 MHz (lo que usa el driver de
 * referencia del C6 con card_div=0).  Actualiza sd_host_khz, que es la base de
 * la que informa sdSetClockSlot().  Devuelve FALSE si \p div se sale del rango
 * de los campos de 4 bits.
 */
bool sdSetHostDiv(uint32_t div);

bool sdSetClock(uint8_t card_div);
bool sdSendCmd(uint idx, uint32_t arg, uint resp, bool crc, uint32_t *r0);

/* Slot-parameterised entry points (the microSD driver uses slot 0; the ESP32-C6
 * SDIO probe uses slot 1).  sdSetClock()/sdSendCmd() above are thin wrappers
 * that pick SD_SLOT, so their behaviour on slot 0 is unchanged. */
bool sdSetClockSlot(uint32_t slot, uint8_t card_div);

/**
 * Cambiar el divisor del slot SIN bajar su reloj: escribe CLKDIV/CLKSRC con
 * CLKENA mantenido y lanza el unico update-clock.  La via normal
 * (sdSetClockSlot) detiene el reloj antes del update, y si el esclavo tiene
 * DAT0 afirmado (data_busy=1) el comando interno queda inhibido con el reloj
 * ya parado: el busy no se muestrea y CMD_START no se auto-limpia nunca.
 */
bool sdSetClockSlotLive(uint32_t slot, uint8_t card_div);
/**
 * Cambiar el divisor del slot SIN bajar su reloj y SIN esperar el "complete" de
 * la transferencia anterior (CMD[13]=0).  Con un slave esp_hosted que mantiene
 * DAT0 abajo, la secuencia clasica se enqista: sdSetClockSlot apaga CLKENA y
 * luego lanza el update con wait_complete, el busy deja de muestrearse porque
 * el reloj esta parado, CMD_START no se auto-limpia y todo comando posterior
 * del slot falla.  Medido en placa en W2: data_busy=1 desde el CMD5 y la
 * subida de reloj clavada con STATUS=0x0001a306.
 */
bool sdSetClockSlotNoWait(uint32_t slot, uint8_t card_div);
bool sdSendCmdSlot(uint32_t slot, uint idx, uint32_t arg, uint resp, bool crc,
                   bool send_init, uint32_t *r0);

/**
 * Volcar descifrado el STATUS del bloque (fifo/fsm/data_busy) y el DIVISOR Y LA
 * FUENTE por-slot realmente enlazados.  Las sondas SDIO lo llaman paso a paso
 * porque el dato que decide el orden de una secuencia es data_busy: el esclavo
 * esp_hosted afirma DAT0 mientras tiene un paquete C2H sin leer o una
 * interrupcion pendiente, y con DAT0 abajo el comando interno "update clock"
 * queda inhibido.
 */
void sdBusState(const char *tag, uint32_t slot);

/** The SDHOST block register base as a word-indexable pointer (for the
 *  per-slot fields the command engine does not parameterise). */
volatile uint32_t *sdHostRegBase(void);

/**
 * One CMD17 block transfer through the bounce page.  The caller must hold
 * the SDMMC bus lock; the block lands in the bounce page for the caller to
 * copy out with sdBlockCopyOut().
 */
bool sdReadBlock(uint32_t lba);

/** One CMD24 block transfer of whatever sdBlockCopyIn() staged. */
bool sdWriteBlock(uint32_t lba);

/** Copy \p bytes out of the block the last read filled. */
void sdBlockCopyOut(void *dst, uint bytes);

/** Copy \p bytes into the block the next write will send. */
void sdBlockCopyIn(const void *src, uint bytes);

/**
 * Fase de datos de un comando SDIO (CMD53) sobre \p slot, por el mismo IDMA y
 * la misma rebote de 512 bytes que sdReadBlock/sdWriteBlock, pero con comando,
 * argumento, tamano de bloque y numero de bytes puestos por el llamante.  No
 * interpreta la respuesta como R1 (SDIO es R5/R6) ni hace la transformacion
 * LBA.  bytcnt <= 512.  El llamante sostiene el candado del bus y prepara la
 * rebote (sdBlockCopyIn antes de un write, sdBlockCopyOut despues de un read).
 */
bool sdTransferSlot(uint32_t slot, uint idx, uint32_t arg, uint32_t bytcnt,
                    uint32_t blksiz, bool write);

/**
 * Fase de datos del mismo CMD53 pero en MODO PIO: el IDMAC apagada (CTRL[5] y
 * CTRL[25] a 0, BMOD e IDINTEN a 0), sin descriptor ni DBADDR, y con el CPU
 * sacando/metiendo palabras de 32 bits por BUFFIFO (0x200).  Sirve para separar
 * "el esclavo no condujo ningun bit" (sd_pio_words=0) de "el dato llego al
 * bloque y el IDMAC no lo sacaba" (sd_pio_words>0).  Escribe/lee \p buf
 * directamente; no usa la rebote del slot 0.  bytcnt <= 512.
 */
bool sdTransferPio(uint32_t slot, uint idx, uint32_t arg, uint8_t *buf,
                   uint32_t bytcnt, uint32_t blksiz, bool write);

extern uint32_t sd_pio_words;         /* palabras servidas por el ultimo PIO */
extern uint32_t sd_pio_bytes;         /* bytes correspondientes               */

#endif                          /* _SDHOST_H_ */
