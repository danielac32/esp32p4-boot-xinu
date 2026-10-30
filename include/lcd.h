/**
 * @file lcd.h
 *
 * LCD device (ST7796 320x480 over SPI2).  The raw panel driver lives in
 * drivers/lcd/; this device initializes it and serializes the shared SPI
 * bus (monitor of the `spi` device) around each operation.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _LCD_H_
#define _LCD_H_

#include <device.h>
#include <drivers/lcd/st7796.h>

/* control(LCD0, CMD, arg1, arg2) */
#define LCD_CTRL_BACKLIGHT 0x01 /**< arg1=0/1                          */
#define LCD_CTRL_FILL      0x02 /**< arg1=rgb565 (fills screen)        */
#define LCD_CTRL_RECT      0x03 /**< arg1=&struct lcd_rect             */
#define LCD_CTRL_PIXEL     0x04 /**< arg1=&struct lcd_pixel            */
#define LCD_CTRL_TEXT      0x05 /**< arg1=&struct lcd_text             */
#define LCD_CTRL_BLIT      0x06 /**< arg1=&struct lcd_blit             */
#define LCD_CTRL_DEMO      0x07 /**< barras + texto (bring-up)         */

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
