/**
 * @file mount.c
 *
 * Add a prefix mapping to the syntactic namespace.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <device.h>
#include <interrupt.h>
#include <kernel.h>
#include <nam.h>
#include <name.h>

/**
 * @ingroup nam
 *
 * Add a prefix mapping to the namespace.
 *
 * @param prefix  Prefix to add, for example "/dev/tft0"
 * @param replace Replacement string, normally "" for a direct device map
 * @param device  Device ID to return when the prefix matches
 * @return OK on success, SYSERR if an argument is invalid or the table is full
 */
syscall mount(const char *prefix, const char *replace, int device)
{
    irqmask mask;               /* Saved interrupt mask                 */
    struct nmentry *namptr;     /* Pointer to unused table entry        */
    int psiz, rsiz;             /* Sizes of prefix and replacement      */
    int i;                      /* Counter for copy loop                */

    mask = disable();

    psiz = namlen(prefix, NM_PRELEN);
    rsiz = namlen(replace, NM_REPLLEN);

    /* If arguments are invalid or table is full, return error. */
    if ((psiz == SYSERR) || (rsiz == SYSERR) ||
        isbaddev(device) || (nnames >= NNAMES))
    {
        restore(mask);
        return SYSERR;
    }

    /* Allocate a slot in the table. */
    namptr = &nametab[nnames];

    /* Copy prefix and replacement strings and record device ID. */
    for (i = 0; i < psiz; i++)
    {
        namptr->nprefix[i] = *prefix++;
    }
    for (i = 0; i < rsiz; i++)
    {
        namptr->nreplace[i] = *replace++;
    }

    namptr->ndevice = device;
    nnames++;

    restore(mask);
    return OK;
}
