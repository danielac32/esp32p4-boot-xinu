/**
 * @file lcdControl.c
 *
 * LCD device control(): the whole drawing API.  Each operation runs with the
 * device lock held, because a window plus its pixel run must not interleave
 * with another thread's.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <kernel.h>
#include <device.h>
#include <lcd.h>
#include "lcdhw.h"

/**
 * @ingroup lcd
 *
 * Operate the panel.
 *
 * @param devptr  LCD device table entry
 * @param func    command (LCD_CTRL_*)
 * @param arg1    command-dependent
 * @param arg2    command-dependent (unused)
 * @return OK, or SYSERR for a bad command, a null argument or a panel that
 *          never came up
 */
devcall lcdControl(device *devptr, int func, long arg1, long arg2)
{
    struct lcdctl *lcdptr;
    const struct lcd_rect *rect;
    const struct lcd_pixel *pixel;
    const struct lcd_text *text;
    const struct lcd_blit *blit;

    (void)arg2;

    lcdptr = &lcdtab[devptr->minor];
    if (!lcdptr->up)
    {
        return SYSERR;
    }

    lock(lcdptr->lock);

    switch (func)
    {
    case LCD_CTRL_BACKLIGHT:
        lcdPanelBacklight(arg1 != 0);
        break;

    /* Plain drawing owns the panel: stop mirroring the log first so the
     * boot screen / demo / test pattern is not overwritten by the next
     * kernel line or wiped by the next scroll. */
    case LCD_CTRL_FILL:
        lcdPanelFill((uint16_t)arg1);
        break;

    case LCD_CTRL_RECT:
        rect = (const struct lcd_rect *)arg1;
        if (rect == NULL)
        {
            unlock(lcdptr->lock);
            return SYSERR;
        }
        //lcdConsoleEnable(FALSE);
        lcdPanelFillRect(rect->x, rect->y, rect->w, rect->h, rect->color);
        break;

    case LCD_CTRL_PIXEL:
        pixel = (const struct lcd_pixel *)arg1;
        if (pixel == NULL)
        {
            unlock(lcdptr->lock);
            return SYSERR;
        }
        //lcdConsoleEnable(FALSE);
        lcdPanelPixel(pixel->x, pixel->y, pixel->color);
        break;

    case LCD_CTRL_TEXT:
        text = (const struct lcd_text *)arg1;
        if (text == NULL)
        {
            unlock(lcdptr->lock);
            return SYSERR;
        }
        //lcdConsoleEnable(FALSE);
        lcdPanelText(text->s, text->x, text->y, text->fg, text->bg);
        break;

    case LCD_CTRL_BLIT:
        blit = (const struct lcd_blit *)arg1;
        if (blit == NULL)
        {
            unlock(lcdptr->lock);
            return SYSERR;
        }
        //lcdConsoleEnable(FALSE);
        lcdPanelBlit(blit->x, blit->y, blit->w, blit->h, blit->img);
        break;

    case LCD_CTRL_DEMO:
        //lcdConsoleEnable(FALSE);
        lcdPanelDemo();
        break;

    case LCD_CTRL_ORIENT:
        if (!lcdPanelOrient((uint8_t)arg1))
        {
            unlock(lcdptr->lock);
            return SYSERR;
        }
        lcdptr->rot = (uint8_t)arg1;
        break;

    /* Banco de pruebas del bus: pinta N pantallas y mide.  Se cuelga de
     * control() como el resto para que el lock del panel lo proteja de la
     * hebra de LVGL mientras corre. */
    case LCD_CTRL_SPEED:
        (void)lcdPanelSpeed((int)arg1);
        break;

    case LCD_CTRL_CLOCK:
        lcdSpiSetClock((uint32_t)arg1);
        kprintf("lcd: reloj de bits = %lu Hz (pedido %ld)\r\n",
                (unsigned long)lcdSpiClockHz(), (long)arg1);
        break;

    default:
        unlock(lcdptr->lock);
        return SYSERR;
    }

    unlock(lcdptr->lock);
    return OK;
}
