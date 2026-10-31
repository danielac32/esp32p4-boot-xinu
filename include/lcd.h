/**
 * @file lcd.h
 *
 * LCD device (ST7796 320x480 over GPSPI2 on the ESP32-P4).
 *
 * The device is self-contained: the SPI2 master, the pads, the CS/DC/RST/BL
 * lines and the panel sequence all live in device/lcd-esp32p4/.  Callers
 * only use the control() commands below.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _LCD_H_
#define _LCD_H_

#include <stdbool.h>
#include <device.h>
#include <board.h>

/* Panel geometry, from the board pin map. */
#define LCD_W   LCD_WIDTH       /**< 320 pixels, portrait               */
#define LCD_H   LCD_HEIGHT      /**< 480 pixels, portrait               */

/* Cell size of the panel's built-in text: 40 columns by 30 rows. */
#define LCD_TEXT_W  8
#define LCD_TEXT_H  16

/* RGB565 colours.  The panel takes the high byte first; the driver
 * byte-swaps each pixel on its way out. */
#define RGB565(r, g, b) \
    (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

#define C_BLACK     0x0000
#define C_WHITE     0xFFFF
#define C_RED       0xF800
#define C_GREEN     0x07E0
#define C_BLUE      0x001F
#define C_YELLOW    0xFFE0
#define C_CYAN      0x07FF
#define C_MAGENTA   0xF81F
#define C_GRAY      0x8410
#define C_DARKBLUE  0x0008
#define C_AMBER     0xFDE0      /**< phosphor: 255,191,0                    */

/* control(LCD0, CMD, arg1, arg2) */
#define LCD_CTRL_BACKLIGHT 0x01 /**< arg1=0/1                            */
#define LCD_CTRL_FILL      0x02 /**< arg1=rgb565 (fills the screen)      */
#define LCD_CTRL_RECT      0x03 /**< arg1=&struct lcd_rect               */
#define LCD_CTRL_PIXEL     0x04 /**< arg1=&struct lcd_pixel              */
#define LCD_CTRL_TEXT      0x05 /**< arg1=&struct lcd_text               */
#define LCD_CTRL_BLIT      0x06 /**< arg1=&struct lcd_blit               */
#define LCD_CTRL_DEMO      0x07 /**< bars + text (bring-up pattern)      */
#define LCD_CTRL_ORIENT    0x08 /**< arg1=LCD_ROT_*                      */

/* Portrait orientations, named after the MADCTL bits they write and
 * numbered with the rotation numbers the reference drivers (Arduino_GFX,
 * Adafruit) use.  Coordinates and text always run left-to-right and
 * top-to-bottom of the 320x480 panel as the controller scans it, so a
 * rotation only moves which physical corner (0,0) lands on.  The odd
 * numbers would set MADCTL.MV, making the panel 480x320 and needing every
 * raster reordered, so they are not supported here. */
#define LCD_ROT_BGR    0   /**< MADCTL 0x08                          */
#define LCD_ROT_MY_MX  2   /**< MADCTL 0xC8: LCD_ROT_BGR turned 180  */
#define LCD_ROT_MX     4   /**< MADCTL 0x48: what the Arduino demo uses */
#define LCD_ROT_MY     6   /**< MADCTL 0x88: LCD_ROT_MX turned 180   */

/* Orientation measured on this unit: with MX and MY both cleared the panel
 * matches the viewer (0,0) at the top-left, x to the right, y down.  The
 * Arduino demo starts at LCD_ROT_MX, and the two 0x88/0x48 settings each
 * leave one axis mirrored, so the default is LCD_ROT_BGR. */
#define LCD_ROT_DEFAULT  LCD_ROT_BGR

struct lcd_rect
{
    int x, y, w, h;
    uint16_t color;
};

struct lcd_pixel
{
    int x, y;
    uint16_t color;
};

struct lcd_text
{
    const char *s;
    int x, y;
    uint16_t fg, bg;
};

struct lcd_blit
{
    int x, y, w, h;
    const uint16_t *img;
};

/* Driver functions */
devcall lcdInit(device *);
devcall lcdControl(device *, int, long, long);

#endif                          /* _LCD_H_ */
