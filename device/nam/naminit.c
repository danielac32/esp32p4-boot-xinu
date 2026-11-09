/**
 * @file naminit.c
 *
 * Initialize the syntactic namespace.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <device.h>
#include <kernel.h>
#include <nam.h>
#include <name.h>
#include <stddef.h>

struct nmentry nametab[NNAMES];     /* Table of name mappings           */
int nnames;                         /* Number of entries allocated      */

/**
 * @ingroup nam
 *
 * Initialize the syntactic namespace.
 *
 * This function only builds the name table.  It deliberately does not call
 * open() on any device: at this point the drivers may not have been
 * initialized yet, and some open() implementations may block.  The actual
 * open is performed later by namopen() when a caller opens NAMESPACE with a
 * path such as "/dev/tft0".
 *
 * @param devptr Namespace device table entry (unused)
 * @return OK
 */
devcall naminit(device *devptr)
{
    int i;                      /* Index into devtab                    */
    const device *dev;          /* Pointer to device table entry        */
    const char *prefix;         /* Pointer into "/dev/"                 */
    const char *name;           /* Pointer to device name               */
    char tmpstr[NM_MAXLEN];     /* String to hold a name                */
    int pos;                    /* Position in tmpstr                   */
    char ch;                    /* Storage for a character              */

    (void)devptr;

    /* Set prefix table to empty. */
    nnames = 0;

    for (i = 0; i < NDEVS; i++)
    {
        dev = &devtab[i];
        pos = 0;

        /* Copy the fixed prefix. */
        prefix = "/dev/";
        while ((*prefix != '\0') && (pos < (NM_MAXLEN - 1)))
        {
            tmpstr[pos++] = *prefix++;
        }

        /* Map device name to lower case and append it. */
        name = dev->name;
        while ((*name != '\0') && (pos < (NM_MAXLEN - 1)))
        {
            ch = *name++;
            if ((ch >= 'A') && (ch <= 'Z'))
            {
                ch = (char)(ch + ('a' - 'A'));
            }
            tmpstr[pos++] = ch;
        }
        tmpstr[pos] = '\0';

        if (*name != '\0')
        {
            kprintf("namespace: device name %s too long\r\n", dev->name);
            continue;
        }

        if (SYSERR == mount(tmpstr, "", dev->num))
        {
            kprintf("namespace: cannot mount device %s\r\n", dev->name);
            continue;
        }

        kprintf("  %s\t\t id: %d\r\n", tmpstr, dev->num);
    }

    return OK;
}
