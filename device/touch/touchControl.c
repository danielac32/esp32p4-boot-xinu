/**
 * @file touchControl.c
 *
 * Device-level control on the touch panel: the controller family and the
 * point array, for callers that prefer control() over read().
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <device.h>
#include <touch.h>

/**
 * @ingroup touch
 *
 * @param devptr touch device table entry
 * @param func   command (TOUCH_CTRL_*)
 * @param arg1   command argument
 * @param arg2   command argument
 * @return OK, a value, or SYSERR for an unknown command
 */
devcall touchControl(device *devptr, int func, long arg1, long arg2)
{
    (void)devptr;

    switch (func)
    {
    case TOUCH_CTRL_READ:
        return touch_read((touch_point_t *)arg1, (int)arg2);

    case TOUCH_CTRL_TYPE:
        return touch_type();

    default:
        return SYSERR;
    }
}
