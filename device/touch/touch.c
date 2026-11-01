/**
 * @file touch.c
 *
 * FT6336 driver.  Register map is the FT5x06 compatible one Waveshare's own
 * library uses: TD_STATUS at 0x02 carries the number of reported points in
 * its low nibble, and each point takes six bytes (XH XL YH YL WEIGHT MISC)
 * starting at 0x03.
 *
 * Every bus access goes through the I2C0 device descriptor, so this driver
 * never touches the raw i2c functions and never handles the bus monitor:
 * i2cControl() serializes the shared bus around each transaction.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <clock.h>
#include <device.h>
#include <gpio.h>
#include <i2c.h>
#include <board.h>
#include "touch.h"

#define FT_TD_STATUS        0x02U   /* low nibble: points reported         */
#define FT_POINT_STRIDE     6U      /* XH XL YH YL WEIGHT MISC, from 0x03  */

/* Boot-time settling for the controller after reset, in milliseconds. */
#define FT_RST_HOLD_MS      20U
#define FT_RST_SETTLE_MS    50U

static touch_type_t touchCtrl = TOUCH_NONE;
static uint8_t touchAddr;

bool touch_init(void)
{
    touchCtrl = TOUCH_NONE;
    touchAddr = 0;

    /* The module brings no pull-up on RST, so the pad stays low across a
     * reboot and the controller never acknowledges 0x38 until it is pulsed. */
    gpio_set_dir(TP_RST, GPIO_OUTPUT);
    gpio_write(TP_RST, false);
    mdelay(FT_RST_HOLD_MS);
    gpio_write(TP_RST, true);
    mdelay(FT_RST_SETTLE_MS);

    if (control(I2C0, I2C_CTRL_PROBE, TOUCH_ADDR_FT6336, 0) == OK)
    {
        touchAddr = TOUCH_ADDR_FT6336;
        touchCtrl = TOUCH_FT6336;
        return true;
    }

    return false;
}

touch_type_t touch_type(void)
{
    return touchCtrl;
}

int touch_read(touch_point_t *pts, int cap)
{
    uint8_t raw[1 + TOUCH_MAX_POINTS * FT_POINT_STRIDE];
    struct i2c_req req;
    uint8_t reported;
    uint8_t want;
    uint8_t i;

    if (pts == NULL || cap <= 0 || touchCtrl == TOUCH_NONE)
    {
        return 0;
    }

    want = (cap > TOUCH_MAX_POINTS) ? TOUCH_MAX_POINTS : (uint8_t)cap;

    req.reg = FT_TD_STATUS;
    req.buf = raw;
    req.len = 1U + FT_POINT_STRIDE * want;
    if (control(I2C0, I2C_CTRL_READ_REGS, touchAddr, (long)&req) != OK)
    {
        return 0;
    }

    reported = raw[0] & 0x0FU;
    if (reported > want)
    {
        reported = want;
    }

    for (i = 0; i < reported; i++)
    {
        const uint8_t *p = &raw[1 + FT_POINT_STRIDE * i];

        pts[i].x = (uint16_t)(((p[0] & 0x0FU) << 8) | p[1]);
        pts[i].y = (uint16_t)(((p[2] & 0x0FU) << 8) | p[3]);
        pts[i].id = (uint8_t)((p[2] >> 4) & 0x0FU);
    }

    return reported;
}
