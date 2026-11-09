/**
 * @file lcdSpi.c
 *
 * SPI2 (GPSPI2) as a write-only master for the ST7796 panel: the clock
 * gates, the pad routing, SPI mode 3, a divider-derived bit clock, and
 * 64-byte PIO bursts through W0..W15.  No DMA and no chip select: CS is a
 * GPIO the panel code owns, so a whole command or pixel run stays inside
 * one assertion of it.
 *
 * The controller comes from the device's csr (SPI2_BASE in xinu.conf), so a
 * second instance would only need a different csr.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <clock.h>
#include <platform.h>
#include <memory.h>
#include <string.h>
#include "lcdhw.h"

static uint32_t spiBase;                /* controller in use             */
static uint32_t timeoutTicks;           /* 100 ms, in clkcount() ticks   */
static size_t   spiLenLatched;          /* longitud ya latcheada con UPDATE */

#define SPIREG(off)   (*(volatile uint32_t *)(spiBase + (off)))
#define SPIWORD(n)    (*(volatile uint32_t *) \
                       (spiBase + P4_SPI2_W0 + 4UL * (n)))

/**
 * Wait for a self-clearing CMD bit (update, usr) to drop.
 * @return false if the controller did not finish within the timeout.
 */
static bool cmdBitDone(uint32_t bit)
{
    uint32_t start = clkcount();

    while ((SPIREG(P4_SPI2_CMD) & bit) != 0)
    {
        if ((clkcount() - start) > timeoutTicks)
        {
            return false;
        }
    }
    return true;
}

void lcdSpiSetClock(uint32_t hz)
{
    uint32_t div, cntn, cnth;

    if (hz == 0)
    {
        hz = LCD_SPI_HZ;
    }

    /* Reloj de bits.  Dos caminos, y el segundo es el que da los 80 MHz:
     *
     *   a) dividido: bit_clk = modulo/(clkdiv_pre+1)/(clkcnt_n+1).  El
     *      divisor interno tiene un minimo de 2, asi que este camino no
     *      pasa de modulo/2 = 40 MHz.
     *   b) directo: CLOCK.clk_equ_sysclk=1 (bit 31) hace que el reloj de
     *      bits SEA el del modulo, sin dividir.  Es la unica forma de que
     *      el ST7796 reciba 80 MHz, que es lo que pide el driver del
     *      fabricante (lcd35_board.h, kLcdPixelClockHz = 80000000).
     *
     * Se elige b) cuando el divisor pedido cae por debajo del minimo.  Los
     * contadores se dejan a 0 porque en modo directo no intervienen.
     * El registro solo entra en vigor al latchearlo con CMD.update, asi que
     * la funcion sirve igual en el arranque y para cambiar de reloj en
     * caliente desde el banco de pruebas. */
    div = (P4_SPI_MODULE_CLK + hz - 1UL) / hz;
    if (div < 2UL)
    {
        SPIREG(P4_SPI2_CLOCK) = P4_SPI_CLK_EQU_SYSCLK;
    }
    else
    {
        if (div > 64UL)
        {
            div = 64UL;
        }
        cntn = div - 1UL;               /* clkcnt_n[17:12]               */
        cnth = div / 2UL - 1UL;         /* clkcnt_h[11:6]                */

        SPIREG(P4_SPI2_CLOCK) = ((cntn & P4_SPI_CLK_CNT_N_MASK) << 12) |
                                 ((cnth & P4_SPI_CLK_CNT_H_MASK) << 6) |
                                 (cntn & P4_SPI_CLK_CNT_L_MASK);
    }

    SPIREG(P4_SPI2_CMD) = P4_SPI_CMD_UPDATE;
    (void)cmdBitDone(P4_SPI_CMD_UPDATE);
}

/**
 * Reloj de bits que el periferico esta aplicando de verdad, deducido del
 * registro (no de lo pedido).  Lo lee el banco de pruebas para no confundir
 * "puse 80" con "hay 80".
 */
uint32_t lcdSpiClockHz(void)
{
    uint32_t v = SPIREG(P4_SPI2_CLOCK);
    uint32_t div;

    if ((v & P4_SPI_CLK_EQU_SYSCLK) != 0)
    {
        return P4_SPI_MODULE_CLK;
    }
    div = ((v >> 12) & P4_SPI_CLK_CNT_N_MASK) + 1UL;
    return P4_SPI_MODULE_CLK / div;
}

void lcdSpiInit(uint32_t base, uint32_t hz)
{
    uint32_t user;


    spiBase = base;
    timeoutTicks = platform.clkfreq / 10UL;
    spiLenLatched = 0;                  /* MS_DLEN sin latchear todavia  */

    /* GPSPI2 clocks: source, APB, high-speed and module clock. */
    *(volatile uint32_t *)P4_CLKRST_SOC_CTRL1 |= P4_CLK_GPSPI2_SYS;
    *(volatile uint32_t *)P4_CLKRST_SOC_CTRL2 |= P4_CLK_GPSPI2_APB;
    *(volatile uint32_t *)P4_CLKRST_PERI_CTRL116 |= (P4_CLK_GPSPI2_HS |
                                                     P4_CLK_GPSPI2_MST);

    /* SCLK and MOSI: pad on the matrix, output enable driven by the
     * controller so the line is released between bursts. */
    lcdPadToGpio(LCD_SCLK);
    lcdPadToGpio(LCD_MOSI);
    lcdMatrixOut(LCD_SCLK, P4_SIG_SPI2_CK, false);
    lcdMatrixOut(LCD_MOSI, P4_SIG_SPI2_D, false);

    /* Module clock = 80 MHz PLL, register clock gate on. */
    SPIREG(P4_SPI2_CLK_GATE) |= (P4_SPI_GATE_CLK_EN |
                                 P4_SPI_GATE_MST_ACTIVE |
                                 P4_SPI_GATE_MST_SEL80);

    /* Reloj de bits (ver lcdSpiSetClock). */
    lcdSpiSetClock(hz);

    /* SPI mode 3 (CPOL=1, CPHA=1), MSB first: that is what the board's
     * reference drivers force for this panel (Arduino_GFX sets
     * _override_datamode = SPI_MODE3 in Arduino_ST7796::begin()).  In the
     * GPSPI2 field names, and per hal/esp32p4/spi_ll.h's mode table, that
     * is ck_idle_edge = 1 with ck_out_edge = 0. */
    SPIREG(P4_SPI2_CTRL) = 0;
    SPIREG(P4_SPI2_MISC) |= P4_SPI_MISC_CK_IDLE;

    user = SPIREG(P4_SPI2_USER);
    user &= ~(P4_SPI_USER_CK_OUT_EDGE | P4_SPI_USER_DOUTDIN |
              P4_SPI_USER_USR_CMD | P4_SPI_USER_USR_ADDR |
              P4_SPI_USER_USR_DUMMY | P4_SPI_USER_USR_MISO);
    user |= P4_SPI_USER_USR_MOSI;
    SPIREG(P4_SPI2_USER) = user;

    SPIREG(P4_SPI2_CMD) = P4_SPI_CMD_UPDATE;
    cmdBitDone(P4_SPI_CMD_UPDATE);
}

/**
 * One burst of at most LCD_SPI_MAX_CHUNK bytes.  Byte i of the stream lives
 * at byte offset i of the concatenated W0..W15 array: the controller walks
 * the buffer little-endian word by word while each byte leaves MSB first
 * (wr_bit_order = 0).  This is the same packing the ESP-IDF master driver
 * uses (spi_ll_write_buffer(): memcpy(&word, &buf[x/8], 4) into data_buf).
 *
 * MS_DLEN es un registro de configuracion y solo entra en vigor al
 * latchearlo con CMD.update.  El handshake del UPDATE cuesta decenas de
 * microsegundos (se sincroniza con el dominio del reloj de bits), asi que
 * hacerlo por cada rafaga de 64 bytes hunde el bus: medido en la placa,
 * 4800 UPDATE por frame costaban 262 ms a 20 MHz cuando el cable solo
 * tarda 123 ms.  Aqui se latchea solo cuando cambia la longitud; en una
 * tirada de pixeles todas las rafagas miden 64 y el UPDATE se paga una
 * vez.  Es lo mismo que hace el driver de ESP-IDF (spi_ll_apply_config).
 */
static bool spiSetLen(size_t len)
{
    uint32_t start = clkcount();

    SPIREG(P4_SPI2_MS_DLEN) = (SPIREG(P4_SPI2_MS_DLEN) &
                               ~P4_SPI_DLEN_MASK) | (len * 8UL - 1UL);
    SPIREG(P4_SPI2_CMD) = P4_SPI_CMD_UPDATE;
    while ((SPIREG(P4_SPI2_CMD) & P4_SPI_CMD_UPDATE) != 0)
    {
        if ((clkcount() - start) > timeoutTicks)
        {
            return false;
        }
    }
    spiLenLatched = len;
    return true;
}

static bool spiBurst(const uint8_t *data, size_t len)
{
    uint32_t word[16];
    uint32_t start = clkcount();
    size_t i;
    uint32_t nwords;

    if (len == 0 || len > LCD_SPI_MAX_CHUNK)
    {
        return false;
    }

    for (i = 0; i < 16; i++)
    {
        word[i] = 0;
    }
    for (i = 0; i < len; i++)
    {
        word[i / 4] |= (uint32_t)data[i] << (8UL * (i % 4));
    }
    nwords = (uint32_t)((len + 3UL) / 4UL);

    if (len != spiLenLatched && !spiSetLen(len))
    {
        return false;
    }

    for (i = 0; i < nwords; i++)
    {
        SPIWORD(i) = word[i];
    }

    SPIREG(P4_SPI2_CMD) = P4_SPI_CMD_USR;
    while ((SPIREG(P4_SPI2_CMD) & P4_SPI_CMD_USR) != 0)
    {
        if ((clkcount() - start) > timeoutTicks)
        {
            return false;
        }
    }
    return true;
}

bool lcdSpiWrite(const uint8_t *data, size_t len)
{
    size_t n;

    if (data == NULL)
    {
        return false;
    }

    while (len > 0)
    {
        n = (len > LCD_SPI_MAX_CHUNK) ? LCD_SPI_MAX_CHUNK : len;
        if (!spiBurst(data, n))
        {
            return false;
        }
        data += n;
        len -= n;
    }
    return true;
}


