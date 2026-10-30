/**
 * @file lvgl.h
 *
 * LVGL device.  The vendored LVGL v8 library lives in lvgl/ (built as a
 * component with its own static heap); this device wires its display port to
 * the ST7796 LCD (LCD0) and its input port to the touch panel (TOUCH0).  Each
 * use is serialized with a monitor.
 *
 * Depends on LCD0 and TOUCH0 being initialized first (devtab order).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _LVGL_H_
#define _LVGL_H_

#include <device.h>

/* control(LVGL0, CMD, arg1, arg2) */
#define LVGL_CTRL_INIT  0x01 /**< (re)build the demo screen                */
#define LVGL_CTRL_TICK  0x02 /**< advance LVGL's tick from millis()        */
#define LVGL_CTRL_TASK  0x03 /**< run lv_timer_handler() once              */

/* Driver functions */
devcall lvglInit(device *);
devcall lvglControl(device *, int, long, long);

/* Lock/unlock del monitor de LVGL (para natives que llamen a la API LVGL). */
void lvgl_lock(void);
void lvgl_unlock(void);

#endif                          /* _LVGL_H_ */
