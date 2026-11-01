/**
 * @file touchRead.c
 *
 * Device-level read on the touch panel: one item is a touch_point_t, so
 * nitems is the capacity of the caller's array rather than a byte count.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <device.h>
#include <touch.h>

/**
 * @ingroup touch
 *
 * Read the currently reported touch points.
 *
 * @param devptr touch device table entry
 * @param buf    caller array of touch_point_t
 * @param nitems capacity of that array, in points
 * @return number of points copied (0 when no finger is down), or SYSERR
 */
devcall touchRead(device *devptr, void *buf, uint nitems)
{
    (void)devptr;

    if (buf == NULL || nitems == 0)
    {
        return SYSERR;
    }

    return touch_read((touch_point_t *)buf, (int)nitems);
}
