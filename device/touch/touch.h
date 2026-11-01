/**
 * @file touch.h
 *
 * Raw capacitive-touch driver: the FT6336 on the shared I2C0 bus, read by
 * polling the FT5x06-compatible report block.  The kernel device
 * (device/touch) wraps this behind read()/control(); nothing here takes a
 * kernel lock.
 */
#ifndef _TOUCHDRV_H_
#define _TOUCHDRV_H_

#include <stdbool.h>
#include <stdint.h>

/** Controller families this driver can talk to. */
typedef enum {
    TOUCH_NONE = 0,
    TOUCH_FT6336,
} touch_type_t;

/** One reported point, in panel pixels (0..319 x 0..479). */
typedef struct {
    uint16_t x;
    uint16_t y;
    uint8_t id;
} touch_point_t;

/* The FT6x36 report block holds five points. */
#define TOUCH_MAX_POINTS 5

/* Release TP_RST and look for the controller.  False when nothing answers. */
bool touch_init(void);

touch_type_t touch_type(void);

/* Read up to cap points; returns how many are reported (0 = no finger). */
int touch_read(touch_point_t *pts, int cap);

#endif                          /* _TOUCHDRV_H_ */
