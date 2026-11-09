/**
 * @file namopen.c
 *
 * Open a device by its name through the syntactic namespace.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <device.h>
#include <nam.h>
#include <name.h>
#include <stdarg.h>
#include <stddef.h>

/**
 * @ingroup nam
 *
 * Open a file or device based on its name.
 *
 * The caller passes the path as the first optional argument to open().
 * Any remaining arguments belong to the target device's open() method and
 * are forwarded unchanged.  For example:
 *
 *     open(NAMESPACE, "/dev/tft0");
 *     open(NAMESPACE, "/dev/console", SERIAL0);
 *
 * @param devptr Entry in the device switch table for NAMESPACE
 * @param ap     Remaining open() arguments
 * @return The result of the target device's open() method
 */
devcall namopen(device *devptr, va_list ap)
{
    char newname[NM_MAXLEN];    /* Name with prefix replaced            */
    char *name;                 /* Name supplied by the caller          */
    int newdev;                 /* Device ID after mapping              */
    device *newdevptr;          /* Target device table entry            */

    name = va_arg(ap, char *);
    if (NULL == name)
    {
        return SYSERR;
    }

    /* Use the namespace to map name to a new name and device descriptor. */
    newdev = nammap(name, newname, devptr->num);
    if ((SYSERR == newdev) || isbaddev(newdev))
    {
        return SYSERR;
    }

    /* Forward the arguments that follow the path to the target driver.
     * Some drivers take no extra arguments; the extra va_list argument is
     * harmless for those. */
    newdevptr = (device *)&devtab[newdev];
    return ((*newdevptr->open)(newdevptr, ap));
}
