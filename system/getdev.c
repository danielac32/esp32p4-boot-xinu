/**
 * @file     getdev.c
 *
 */
/* Embedded Xinu, Copyright (C) 2009.  All rights reserved. */

#include <stddef.h>
#include <device.h>
#include <nam.h>
#include <string.h>

/**
 * @ingroup devcalls
 *
 * Translates a device name into a device number.
 *
 * @param dev
 *      Name of the device.
 *
 * @return
 *      The number of the device, or ::SYSERR if the device was not found.
 */
syscall getdev(const char *dev)
{
    int devnum;
#ifdef NAMESPACE
    char newname[NM_MAXLEN];
#endif

    for (devnum = 0; devnum < NDEVS; devnum++)
    {
        if (0 == strncmp(dev, devtab[devnum].name, DEVMAXNAME))
        {
            return devnum;
        }
    }

#ifdef NAMESPACE
    /* Fall back to the syntactic namespace, so paths like "/dev/lcd0" work
     * for callers that use getdev() instead of open(NAMESPACE, ...). */
    devnum = nammap(dev, newname, NAMESPACE);
    if (SYSERR != devnum)
    {
        return devnum;
    }
#endif

    return SYSERR;
}
