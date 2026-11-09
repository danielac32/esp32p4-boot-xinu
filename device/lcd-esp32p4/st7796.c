/**
 * @file st7796.c
 *
 * ST7796 320x480 panel: reset, init sequence and the drawing primitives the
 * device exposes through control().  Init commands come from
 * GFX_Library_for_Arduino (waveshareteam, Apache-2.0) as used by Waveshare's
 * P4 demo: COLMOD 16-bit, vendor commands F0/B4/B6/E8/C1/C2/C5, gamma
 * tables E0/E1, SLPOUT, INVON (the panel is IPS), DISPON, and MADCTL for
 * the portrait orientation in effect (see lcdPanelOrient()).
 *
 * The panel sits on the SPI2 master in lcdSpi.c.  Chip select and data/command
 * are GPIOs owned here: CS stays asserted for a whole command, its parameter
 * list, or a full pixel run, and DC is switched between them.  There is no
 * framebuffer: every call writes straight to the controller's RAM.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <clock.h>
#include <kernel.h>          /* kprintf del banco de pruebas */
#include <timer.h>           /* millis()                    */
#include <memory.h>
#include "lcdhw.h"
#include "font8x16.h"

#define CMD_SLPOUT  0x11
#define CMD_DISPON  0x29
#define CMD_CASET   0x2A
#define CMD_RASET   0x2B
#define CMD_RAMWR   0x2C
#define CMD_MADCTL  0x36
#define CMD_COLMOD  0x3A
#define CMD_INVON   0x21

/* MADCTL (MY|MX|BGR) for the four portrait orientations, indexed by
 * rotation/2: 0 -> 0x08, 2 -> 0xC8, 4 -> 0x48, 6 -> 0x88.  The odd
 * rotation numbers set MV, which swaps the panel's scan axes and would
 * need every raster reordered, so they are not offered.  MX and MY alone
 * only reverse the direction the controller walks the window, so the byte
 * stream stays in normal raster order for all four. */
static const uint8_t madctlPortrait[4] = { 0x08, 0xC8, 0x48, 0x88 };

/** Command byte (DC low). */
static bool panelCmd(uint8_t cmd)
{
    bool ok;

    lcdPinWrite(LCD_DC, false);
    lcdPinWrite(LCD_CS, false);
    ok = lcdSpiWrite(&cmd, 1);
    lcdPinWrite(LCD_CS, true);
    return ok;
}

/** Parameter/pixel bytes (DC high). */
static bool panelData(const uint8_t *data, size_t len)
{
    bool ok;

    lcdPinWrite(LCD_DC, true);
    lcdPinWrite(LCD_CS, false);
    ok = lcdSpiWrite(data, len);
    lcdPinWrite(LCD_CS, true);
    return ok;
}

static bool panelCmdData(uint8_t cmd, const uint8_t *data, size_t len)
{
    bool ok = panelCmd(cmd);

    if (ok && len > 0)
    {
        ok = panelData(data, len);
    }
    return ok;
}

/** Set the write window, then open it with RAMWR (leaves CS high). */
static bool panelWindow(int x0, int y0, int x1, int y1)
{
    uint8_t d[4] = {
        (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1
    };
    bool ok = panelCmdData(CMD_CASET, d, 4);

    d[0] = (uint8_t)(y0 >> 8);
    d[1] = (uint8_t)y0;
    d[2] = (uint8_t)(y1 >> 8);
    d[3] = (uint8_t)y1;
    ok = panelCmdData(CMD_RASET, d, 4) && ok;
    ok = panelCmd(CMD_RAMWR) && ok;
    return ok;
}

/**
 * Run of identical pixels, window already open.  Stops at the first failed
 * burst instead of keep pushing onto a desynchronised bus.
 */
static bool panelPush(uint16_t color, uint32_t count)
{
    uint8_t chunk[LCD_SPI_MAX_CHUNK];
    uint32_t pixels;
    size_t left, n;
    bool ok = true;
    unsigned i;

    for (i = 0; i < LCD_SPI_MAX_CHUNK; i += 2)
    {
        chunk[i] = (uint8_t)(color >> 8);
        chunk[i + 1] = (uint8_t)color;
    }

    lcdPinWrite(LCD_DC, true);
    lcdPinWrite(LCD_CS, false);
    while (count > 0)
    {
        /* CS must stay low for the whole run, so the burst loop is inside
         * the assertion; 32 pixels = one 64-byte controller buffer. */
        pixels = (count > 32UL) ? 32UL : count;
        left = pixels * 2UL;
        while (left > 0)
        {
            n = (left > LCD_SPI_MAX_CHUNK) ? LCD_SPI_MAX_CHUNK : left;
            if (!lcdSpiWrite(chunk, n))
            {
                ok = false;
                count = 0;
                break;
            }
            left -= n;
        }
        count -= pixels;
    }
    lcdPinWrite(LCD_CS, true);
    return ok;
}

void lcdPanelBacklight(bool on)
{
    lcdPinWrite(LCD_BL, on);
}

bool lcdPanelInit(void)
{
    /* Gamma tables exactly as Arduino_GFX ships them for the ST7796: the
     * leading 0xF0/0xE0 are data bytes of the gamma registers, not
     * addresses. */
    static const uint8_t gammaE0[14] = {
        0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F, 0x54,
        0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B
    };
    static const uint8_t gammaE1[14] = {
        0xE0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2B, 0x43,
        0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B
    };
    static const uint8_t b6[3] = { 0x80, 0x22, 0x3B };
    static const uint8_t e8[8] = {
        0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33
    };
    uint8_t v;

    /* CS idle high, DC low, backlight off, RST idle high. */
    lcdPinSetup(LCD_CS, true);
    lcdPinSetup(LCD_DC, false);
    lcdPinSetup(LCD_BL, false);
    lcdPinSetup(LCD_RST, true);

    mdelay(100);
    lcdPinWrite(LCD_RST, false);
    mdelay(120);
    lcdPinWrite(LCD_RST, true);
    mdelay(120);

    /* Every step is checked: a failed burst aborts the init rather than keep
     * talking to a panel that lost the byte stream. */
    v = 0x55;
    if (!panelCmdData(CMD_COLMOD, &v, 1))
    {
        return false;
    }
    v = 0xC3;
    if (!panelCmdData(0xF0, &v, 1))
    {
        return false;
    }
    v = 0x96;
    if (!panelCmdData(0xF0, &v, 1))
    {
        return false;
    }
    v = 0x01;
    if (!panelCmdData(0xB4, &v, 1))
    {
        return false;
    }
    if (!panelCmdData(0xB6, b6, 3))
    {
        return false;
    }
    if (!panelCmdData(0xE8, e8, 8))
    {
        return false;
    }
    v = 0x06;
    if (!panelCmdData(0xC1, &v, 1))
    {
        return false;
    }
    v = 0xA7;
    if (!panelCmdData(0xC2, &v, 1))
    {
        return false;
    }
    v = 0x18;
    if (!panelCmdData(0xC5, &v, 1))
    {
        return false;
    }
    if (!panelCmdData(0xE0, gammaE0, 14))
    {
        return false;
    }
    if (!panelCmdData(0xE1, gammaE1, 14))
    {
        return false;
    }
    v = 0x3C;
    if (!panelCmdData(0xF0, &v, 1))
    {
        return false;
    }
    v = 0x69;
    if (!panelCmdData(0xF0, &v, 1))
    {
        return false;
    }
    if (!panelCmd(CMD_SLPOUT))
    {
        return false;
    }
    mdelay(120);
    if (!panelCmd(0x38))          /* NORSET: plain SPI, no quad mode      */
    {
        return false;
    }
    if (!panelCmd(CMD_DISPON))
    {
        return false;
    }
    mdelay(120);

    /* The board's panel is an IPS one: the reference drivers call
     * invertDisplay(false) with ips=true, which lands as INVON. */
    if (!panelCmd(CMD_INVON))
    {
        return false;
    }

    if (!lcdPanelOrient(LCD_ROT_DEFAULT))
    {
        return false;
    }

    /* The reset pulse rewires the controller but leaves the old frame in the
     * panel's GRAM, so without this the panel would come up showing whatever
     * the previous session drew.  Costs ~250 ms of boot time. */
    lcdPanelFill(C_BLACK);

    lcdPinWrite(LCD_BL, true);
    return true;
}

/**
 * @ingroup lcd
 *
 * Rotate the panel to one of the four portrait orientations.
 *
 * @param rot  reference-driver rotation number: 0, 2, 4 or 6
 * @return true when MADCTL was written, false for an unsupported number
 *          (odd = MV, which this driver does not support) or a bus timeout
 */
bool lcdPanelOrient(uint8_t rot)
{
    uint8_t madctl;

    if ((rot & 1) != 0 || rot > 6)
    {
        return false;
    }
    madctl = madctlPortrait[rot >> 1];
    return panelCmdData(CMD_MADCTL, &madctl, 1);
}

/** Clip a rectangle to the panel; w or h becomes 0 when it is off-screen. */
static void rectClamp(int *x, int *y, int *w, int *h)
{
    if (*x >= LCD_W || *y >= LCD_H || *w <= 0 || *h <= 0)
    {
        *w = 0;
        return;
    }
    if (*x < 0)
    {
        *w += *x;
        *x = 0;
    }
    if (*y < 0)
    {
        *h += *y;
        *y = 0;
    }
    if (*x + *w > LCD_W)
    {
        *w = LCD_W - *x;
    }
    if (*y + *h > LCD_H)
    {
        *h = LCD_H - *y;
    }
}

void lcdPanelFill(uint16_t color)
{
    if (panelWindow(0, 0, LCD_W - 1, LCD_H - 1))
    {
        panelPush(color, (uint32_t)LCD_W * LCD_H);
    }
}

void lcdPanelFillRect(int x, int y, int w, int h, uint16_t color)
{
    rectClamp(&x, &y, &w, &h);
    if (w <= 0 || h <= 0)
    {
        return;
    }
    if (panelWindow(x, y, x + w - 1, y + h - 1))
    {
        panelPush(color, (uint32_t)w * h);
    }
}

void lcdPanelPixel(int x, int y, uint16_t color)
{
    uint8_t px[2];

    if (x < 0 || y < 0 || x >= LCD_W || y >= LCD_H)
    {
        return;
    }
    px[0] = (uint8_t)(color >> 8);
    px[1] = (uint8_t)color;
    if (panelWindow(x, y, x, y))
    {
        panelData(px, 2);
    }
}

void lcdPanelText(const char *s, int x, int y, uint16_t fg, uint16_t bg)
{
    uint8_t glyph[FONT_W * FONT_H * 2];
    unsigned ch;
    int gi, col, row;

    if (s == NULL)
    {
        return;
    }

    while (*s != '\0')
    {
        ch = (unsigned char)*s++;
        if (ch < 32 || ch > 126)
        {
            ch = '?';
        }

        /* One 8x16 cell, row-major, bit 7 of each row being the leftmost
         * pixel; the cell is expanded to RGB565 and sent as a single
         * windowed burst. */
        gi = 0;
        for (row = 0; row < FONT_H; row++)
        {
            uint8_t bits = font8x16[ch - 32][row];

            for (col = 0; col < FONT_W; col++)
            {
                uint16_t c = ((bits & (0x80U >> col)) != 0) ? fg : bg;

                glyph[gi++] = (uint8_t)(c >> 8);
                glyph[gi++] = (uint8_t)c;
            }
        }

        if (x + FONT_W <= LCD_W && y + FONT_H <= LCD_H)
        {
            if (!panelWindow(x, y, x + FONT_W - 1, y + FONT_H - 1))
            {
                return;
            }
            if (!panelData(glyph, sizeof(glyph)))
            {
                return;
            }
        }

        x += FONT_W;
        if (x + FONT_W > LCD_W)
        {
            x = 0;
            y += FONT_H;
        }
    }
}

void lcdPanelBlit(int x, int y, int w, int h, const uint16_t *img)
{
    uint8_t tmp[LCD_SPI_MAX_CHUNK];
    const uint16_t *src;
    int xx = x, yy = y, ww = w, hh = h;
    uint32_t left, i;
    int r;

    if (img == NULL)
    {
        return;
    }

    /* img is a packed w*h RGB565 area (a drawing buffer, native uint16_t).
     * The panel wants the high byte first, so each pixel is swapped on the
     * way out. */
    rectClamp(&xx, &yy, &ww, &hh);
    if (ww <= 0 || hh <= 0 || x < 0 || y < 0)
    {
        return;
    }
    if (!panelWindow(xx, yy, xx + ww - 1, yy + hh - 1))
    {
        return;
    }

    /* Camino PIO. */
    lcdPinWrite(LCD_DC, true);
    lcdPinWrite(LCD_CS, false);
    for (r = 0; r < hh; r++)
    {
        src = img + (yy - y + r) * w + (xx - x);
        left = (uint32_t)ww;
        while (left > 0)
        {
            uint32_t n = (left > 32UL) ? 32UL : left;

            for (i = 0; i < n; i++)
            {
                uint16_t v = src[i];

                tmp[2 * i] = (uint8_t)(v >> 8);
                tmp[2 * i + 1] = (uint8_t)v;
            }
            if (!lcdSpiWrite(tmp, n * 2UL))
            {
                lcdPinWrite(LCD_CS, true);
                return;
            }
            src += n;
            left -= n;
        }
    }
    lcdPinWrite(LCD_CS, true);
}

void lcdPanelDemo(void)
{
    static const uint16_t bars[8] = {
        C_WHITE, C_YELLOW, C_CYAN, C_GREEN,
        C_MAGENTA, C_RED, C_BLUE, C_BLACK
    };
    int i;

    /* Three 16-pixel text lines are kept clear at the bottom. */
    lcdPanelFill(C_BLACK);
    for (i = 0; i < 8; i++)
    {
        lcdPanelFillRect(i * (LCD_W / 8), 0, LCD_W / 8, LCD_H - 52, bars[i]);
    }
    lcdPanelText("xinu on ESP32-P4", 8, LCD_H - 48, C_GREEN, C_BLACK);
    lcdPanelText("ST7796 320x480 SPI2", 8, LCD_H - 32, C_CYAN, C_BLACK);
    lcdPanelText("baremetal, no ESP-IDF", 8, LCD_H - 16, C_GRAY, C_BLACK);
}

/**
 * Medida del camino de pixeles (banco de pruebas del bus).
 *
 * Pinta \p iters pantallas completas alternando dos colores: cada pasada
 * abre ventana, manda CASET/RASET/RAMWR y empuja LCD_W*LCD_H pixeles de 16
 * bits.  Con eso se mide lo que de verdad importa para la fluidez:
 *
 *   - ms/frame  = tiempo de pared de un frame completo.
 *   - kB/s      = bytes de pixel que salen por el cable cada segundo, que
 *                 es lo que fija el reloj del SPI (320*480*2 bytes) y lo que
 *                 sirve para comparar 20, 40 u 80 MHz sin ambiguedad.
 *
 * Se mide con millis() y no con micros() porque un frame a 20 MHz pasa de
 * 100 ms y micros() (32 bits a 360 MHz) da la vuelta cada 11,9 s.
 *
 * Devuelve los ms de un frame (0 si el panel no respondio a la ventana).
 */
uint32_t lcdPanelSpeed(int iters)
{
    uint32_t t0, t1, ms;
    uint32_t bytes;
    uint16_t color = C_BLACK;
    int i;

    if (iters <= 0)
    {
        iters = 10;
    }

    /* Primer frame fuera del cronometro: deja el bus en marcha y descarta
     * el coste de la primera ventana. */
    lcdPanelFill(C_BLACK);

    t0 = millis();
    for (i = 0; i < iters; i++)
    {
        color ^= 0xFFFF;
        lcdPanelFill(color);
    }
    t1 = millis();

    ms = t1 - t0;
    if (ms == 0)
    {
        ms = 1;
    }
    bytes = (uint32_t)iters * (uint32_t)LCD_W * (uint32_t)LCD_H * 2UL;

    kprintf("lcd: %d frames en %lu ms -> %lu ms/frame, "
            "%lu kB/s (%lu.%lu MB/s) @ %lu Hz\r\n",
            iters, (unsigned long)ms, (unsigned long)(ms / (uint32_t)iters),
            (unsigned long)(bytes / ms),
            (unsigned long)(bytes / ms / 1000UL),
            (unsigned long)((bytes / ms) % 1000UL) / 100UL,
            (unsigned long)lcdSpiClockHz());

    return ms / (uint32_t)iters;
}
