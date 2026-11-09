/**
 * @file lcdhw.h
 *
 * Private interface of the ESP32-P4 LCD device: the SPI2 master, the pads
 * and GPIO lines the panel needs, and the raw ST7796 panel calls.  Nothing
 * here belongs to a caller: user code talks to the device through
 * control(LCD0, LCD_CTRL_*, ...) (see <lcd.h>).
 *
 * Register facts below were checked against ESP-IDF for the ESP32-P4:
 *   register/soc/spi_reg.h            (GPSPI2 register offsets)
 *   register/soc/spi_struct.h         (CMD/CTRL/CLOCK/USER/MS_DLEN/MISC/
 *                                      CLK_GATE field bit positions)
 *   register/soc/io_mux_struct.h      (fun_wpd/fun_wpu/fun_ie/fun_drv/
 *                                      mcu_sel)
 *   register/soc/hp_sys_clkrst_reg.h  (GPSPI2 clock enables)
 *   include/soc/gpio_sig_map.h        (SPI2_CK=53, SPI2_D=55)
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _LCDHW_H_
#define _LCDHW_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <conf.h>
#include <esp32p4.h>
#include <board.h>
#include <lcd.h>
#include <monitor.h>

/**
 * Device control block, one per instance (indexed by devptr->minor).
 * @c lock serializes panel operations between threads and @c up records a
 * successful init, so control() cannot talk to a panel that never came up.
 * The SPI controller itself is bound once, from devptr->csr, in lcdInit().
 */
struct lcdctl
{
    monitor lock;
    bool    up;
    uint8_t rot;        /**< portrait rotation in effect (LCD_ROT_*)     */
};

extern struct lcdctl lcdtab[NLCD];

/* ======================================================================
 * GPIO / IO_MUX (only what this device needs)
 * ====================================================================== */

/* GPIO register offsets. */
#define P4_GPIO_OUT_W1TS        (GPIO_BASE + 0x08UL)
#define P4_GPIO_OUT_W1TC        (GPIO_BASE + 0x0CUL)
#define P4_GPIO_OUT1_W1TS       (GPIO_BASE + 0x14UL)
#define P4_GPIO_OUT1_W1TC       (GPIO_BASE + 0x18UL)
#define P4_GPIO_ENABLE_W1TS     (GPIO_BASE + 0x24UL)
#define P4_GPIO_ENABLE_W1TC     (GPIO_BASE + 0x28UL)
#define P4_GPIO_ENABLE1_W1TS    (GPIO_BASE + 0x30UL)
#define P4_GPIO_ENABLE1_W1TC    (GPIO_BASE + 0x34UL)
/* FUNCn_OUT_SEL_CFG[pin]: OUT_SEL[8:0], INV[9], OEN_SEL[10], OEN_INV[11] */
#define P4_GPIO_OUT_SEL(pin)    (GPIO_BASE + 0x558UL + 4UL * (pin))
#define P4_GPIO_OUT_SEL_MASK    0x1FFUL
#define P4_GPIO_OUT_SEL_GPIO    256UL     /* 256 = the pin follows GPIO_OUT */
#define P4_GPIO_OEN_SEL         (1UL << 10) /**< output enable = GPIO_ENABLE */

/* IO_MUX pad register n lives at base + 4 + 4*n.  Field bits: fun_wpd=7,
 * fun_wpu=8, fun_ie=9, fun_drv=10..11, mcu_sel=12..14 (1 = GPIO matrix). */
#define P4_IOPAD(pin)           (IO_MUX_BASE + 4UL + 4UL * (pin))
#define P4_IO_FUN_IE            (1UL << 9)
#define P4_IO_FUN_DRV_40MA      (3UL << 10)
#define P4_IO_MCU_SEL_MASK      (7UL << 12)
#define P4_IO_MCU_SEL_GPIO      (1UL << 12)

/* ======================================================================
 * SPI2 (GPSPI2) controller
 * ====================================================================== */

/* Bit clock for the panel bus.  GPSPI2 runs from the 80 MHz PLL
 * (CLK_GATE.mst_clk_sel = 1; that reference clock is on out of reset, see
 * HP_SYS_CLKRST.ref_clk_ctrl1.reg_ref_80m_clk_en).
 *
 * Se pide el maximo del modulo, 80 MHz, que es exactamente lo que usa el
 * driver del fabricante para esta misma pantalla y estas mismas patas:
 * examples/arduino/libraries/Waveshare_LCD35/src/lcd35_board.h,
 * kLcdPixelClockHz = 80000000.  Por encima de 40 MHz el divisor interno del
 * periferico no llega (minimo div=2), asi que el reloj de bits se toma
 * directo del modulo con CLOCK.clk_equ_sysclk=1 (ver lcdSpi.c).  A 80 MHz un
 * frame completo son 30,7 ms de cable; a 20 MHz eran 122,9. */
#define LCD_SPI_HZ              80000000UL

#define P4_SPI2_CMD             0x00UL
#define P4_SPI2_CTRL            0x08UL
#define P4_SPI2_CLOCK           0x0CUL
#define P4_SPI2_USER            0x10UL
#define P4_SPI2_MS_DLEN         0x1CUL
#define P4_SPI2_MISC            0x20UL
#define P4_SPI2_DMA_CONF        0x30UL
#define P4_SPI2_DMA_INT_CLR     0x38UL
#define P4_SPI2_DMA_INT_RAW     0x3CUL
#define P4_SPI2_W0              0x98UL    /* W0..W15: 16 words = 64 bytes */
#define P4_SPI2_CLK_GATE        0xE8UL

#define P4_SPI_CMD_UPDATE       (1UL << 23)   /* latch CONF into the state  */
#define P4_SPI_CMD_USR          (1UL << 24)   /* start; self-clears         */

#define P4_SPI_CTRL_WR_ORDER    (3UL << 25)   /* 0 = MSB first              */
#define P4_SPI_CTRL_RD_ORDER    (3UL << 23)

#define P4_SPI_CLK_CNT_L_MASK   0x3FUL        /* [5:0]   */
#define P4_SPI_CLK_CNT_H_MASK   0x3FUL        /* [11:6]  */
#define P4_SPI_CLK_CNT_N_MASK   0x3FUL        /* [17:12] */
#define P4_SPI_CLK_DIV_PRE_MASK 0xFUL         /* [21:18] */
#define P4_SPI_CLK_EQU_SYSCLK   (1UL << 31)

#define P4_SPI_USER_DOUTDIN     (1UL << 0)
#define P4_SPI_USER_CK_OUT_EDGE (1UL << 9)
#define P4_SPI_USER_USR_MOSI    (1UL << 27)
#define P4_SPI_USER_USR_MISO    (1UL << 28)
#define P4_SPI_USER_USR_DUMMY   (1UL << 29)
#define P4_SPI_USER_USR_ADDR    (1UL << 30)
#define P4_SPI_USER_USR_CMD     (1UL << 31)
#define P4_SPI_DLEN_MASK        0x3FFFFUL     /* ms_data_bitlen[17:0]       */
#define P4_SPI_MISC_CK_IDLE     (1UL << 29)

#define P4_SPI_DMA_OUTFIFO_EMPTY (1UL << 0)   /* DMA_CONF RO: TX FIFO vacio  */
#define P4_SPI_DMA_RX_EOF_EN    (1UL << 21)   /* DMA_CONF: modo EOF RX       */
#define P4_SPI_DMA_TX_ENA       (1UL << 28)   /* DMA_CONF: habilita TX DMA  */
#define P4_SPI_DMA_AFIFO_RST    (1UL << 31)   /* DMA_CONF: reset TX DMA FIFO (WT) */

#define P4_SPI_DMA_INT_TRANS_DONE (1UL << 12) /* DMA_INT_*: transaccion fin  */

#define P4_SPI_GATE_CLK_EN      (1UL << 0)
#define P4_SPI_GATE_MST_ACTIVE  (1UL << 1)
#define P4_SPI_GATE_MST_SEL80   (1UL << 2)    /* 1 = PLL 80 MHz source      */

/* Module clock feeding the divider when CLK_GATE.mst_clk_sel = 1. */
#define P4_SPI_MODULE_CLK       80000000UL

/* HP_SYS_CLKRST enables for GPSPI2. */
#define P4_CLKRST_SOC_CTRL1     (HP_SYS_CLKRST_BASE + 0x18UL)
#define P4_CLKRST_SOC_CTRL2     (HP_SYS_CLKRST_BASE + 0x1CUL)
#define P4_CLKRST_PERI_CTRL116  (HP_SYS_CLKRST_BASE + 0x80UL)
#define P4_CLK_GPSPI2_SYS       (1UL << 0)    /* SOC_CLK_CTRL1  */
#define P4_CLK_GPSPI2_APB       (1UL << 19)   /* SOC_CLK_CTRL2  */
#define P4_CLK_GPSPI2_HS        (1UL << 3)    /* PERI_CLK_CTRL116 */
#define P4_CLK_GPSPI2_MST       (1UL << 20)   /* PERI_CLK_CTRL116 */

/* GPIO matrix signal indices (gpio_sig_map.h). */
#define P4_SIG_SPI2_CK          53U
#define P4_SIG_SPI2_D           55U

/* Largest single transaction: the controller has 16 data words. */
#define LCD_SPI_MAX_CHUNK       64U

/* ======================================================================
 * SPI master (lcdSpi.c)
 * ====================================================================== */

/**
 * Bring up an SPI controller as a write-only master for the panel bus.
 * @param base  controller base address (the device's csr, e.g. SPI2_BASE)
 * @param hz    requested bit clock; rounded down to what the divider can do
 */
void lcdSpiInit(uint32_t base, uint32_t hz);

/** Cambia el reloj de bits en caliente (hz=0 vuelve a LCD_SPI_HZ). */
void lcdSpiSetClock(uint32_t hz);

/** Reloj de bits que el periferico aplica de verdad, segun su registro. */
uint32_t lcdSpiClockHz(void);

/**
 * Push a buffer on the bus, MSB first, in bursts of at most 64 bytes.
 * The caller owns chip select: it must be asserted for the whole transfer.
 * @return true when every burst completed, false on controller timeout.
 */
bool lcdSpiWrite(const uint8_t *data, size_t len);

/* ======================================================================
 * Panel lines (lcdGpio.c)
 * ====================================================================== */

/** Switch a pad's IO_MUX to the GPIO matrix (input enable, 40 mA drive). */
void lcdPadToGpio(uint8_t pin);

/**
 * Route a peripheral output signal through the GPIO matrix onto a pad.
 * @param oenFromEnable  true: the pad's output enable follows GPIO_ENABLE
 * (plain GPIO use);  false: the peripheral governs it (SPI clock/MOSI).
 */
void lcdMatrixOut(uint8_t pin, uint32_t sig, bool oenFromEnable);

/** Configure a pad as a plain GPIO output and drive it to @p level. */
void lcdPinSetup(uint8_t pin, bool level);

/** Drive an already-configured GPIO output. */
void lcdPinWrite(uint8_t pin, bool level);

/* ======================================================================
 * ST7796 panel (st7796.c)
 * ====================================================================== */

/** Reset + init sequence + backlight on.  Requires lcdSpiInit() first. */
bool lcdPanelInit(void);

/** Write MADCTL for one of the four portrait rotations (see <lcd.h>). */
bool lcdPanelOrient(uint8_t rot);

void lcdPanelBacklight(bool on);
void lcdPanelFill(uint16_t color);
void lcdPanelFillRect(int x, int y, int w, int h, uint16_t color);
void lcdPanelPixel(int x, int y, uint16_t color);
void lcdPanelText(const char *s, int x, int y, uint16_t fg, uint16_t bg);
void lcdPanelBlit(int x, int y, int w, int h, const uint16_t *img);
void lcdPanelDemo(void);

/**
 * Medida del camino de pixeles: pinta \p iters pantallas completas y saca por
 * consola los ms por frame y los kB/s que salen por el cable.  No es un
 * adorno: separa el coste del SPI (bytes/frame) del coste del CPU (empaquetar
 * y hacer byte-swap), que es lo que hay que saber antes de tocar el reloj o
 * de meter DMA.  Devuelve los ms que costo un frame (0 si no pudo dibujar).
 */
uint32_t lcdPanelSpeed(int iters);

#endif                          /* _LCDHW_H_ */
