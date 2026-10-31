/**
 * @file nammap.c
 *
 * Map a name through the syntactic namespace.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <device.h>
#include <nam.h>
#include <name.h>
#include <stddef.h>

/**
 * @ingroup nam
 *
 * Using the namespace, map a name to a new name and a device ID.
 *
 * @param name    Name to map
 * @param newname Buffer for the mapped name
 * @param namdev  ID of the namespace device
 * @return The mapped device ID, or SYSERR if no mapping was found
 */
int nammap(const char *name, char *newname, int namdev)
{
    int newdev;                 /* Device descriptor to return          */
    char tmpname[NM_MAXLEN];    /* Temporary buffer for name            */
    int iter;                   /* Number of iterations                 */

    if ((NULL == name) || (NULL == newname))
    {
        return SYSERR;
    }

    /* Place original name in temporary buffer and null terminate. */
    if (namcpy(tmpname, name, NM_MAXLEN) == SYSERR)
    {
        return SYSERR;
    }

    /* Repeatedly substitute the name prefix until a non-namespace device
     * is reached or an iteration limit is exceeded. */
    for (iter = 0; iter < nnames; iter++)
    {
        newdev = namrepl(tmpname, newname);
        if (newdev != namdev)
        {
            return newdev;      /* Either valid ID or SYSERR            */
        }
        if (namcpy(tmpname, newname, NM_MAXLEN) == SYSERR)
        {
            return SYSERR;
        }
    }
    return SYSERR;
}

/**
 * @ingroup nam
 *
 * Use the name table to perform prefix substitution.
 *
 * @param name    Original name
 * @param newname Buffer for the substituted name
 * @return The device ID associated with the matching prefix, or SYSERR
 */
int namrepl(const char *name, char *newname)
{
    int i;                      /* Iterate through name table           */
    const char *pptr;           /* Walks through a prefix               */
    const char *rptr;           /* Walks through a replacement          */
    const char *optr;           /* Walks through original name          */
    char *nptr;                 /* Walks through new name               */
    int olen;                   /* Length of original name              */
    int plen;                   /* Length of a prefix string            */
    int rlen;                   /* Length of replacement string         */
    int remain;                 /* Bytes beyond prefix                  */
    struct nmentry *namptr;     /* Pointer to a table entry             */

    if ((NULL == name) || (NULL == newname))
    {
        return SYSERR;
    }

    /* Search name table for first prefix that matches. */
    for (i = 0; i < nnames; i++)
    {
        namptr = &nametab[i];
        optr = name;            /* Start at beginning of name           */
        pptr = namptr->nprefix; /* Start at beginning of prefix         */

        /* Compare prefix to string and count prefix size. */
        for (plen = 0; *pptr != '\0'; plen++)
        {
            if (*pptr != *optr)
            {
                break;
            }
            pptr++;
            optr++;
        }
        if (*pptr != '\0')      /* Prefix does not match                */
        {
            continue;
        }

        /* Found a match - check that replacement string plus bytes
         * remaining at the end of the original name will fit into the new
         * name buffer.  Ignore null on replacement string, but keep null on
         * remainder of name. */
        olen = namlen(name, NM_MAXLEN);
        rlen = namlen(namptr->nreplace, NM_MAXLEN) - 1;
        if ((olen == SYSERR) || (rlen < 0))
        {
            return SYSERR;
        }
        remain = olen - plen;
        if ((rlen + remain) > NM_MAXLEN)
        {
            return SYSERR;
        }

        /* Place replacement string followed by remainder of original name
         * (and null) into the new name buffer. */
        nptr = newname;
        rptr = namptr->nreplace;
        for (; rlen > 0; rlen--)
        {
            *nptr++ = *rptr++;
        }
        for (; remain > 0; remain--)
        {
            *nptr++ = *optr++;
        }
        return namptr->ndevice;
    }
    return SYSERR;
}

/**
 * @ingroup nam
 *
 * Copy a name from one buffer to another, checking length.
 *
 * @param newname Buffer to hold copy
 * @param oldname Buffer containing name
 * @param buflen  Size of buffer for copy
 * @return OK if the name was copied, SYSERR if it did not fit
 */
int namcpy(char *newname, const char *oldname, int buflen)
{
    char *nptr;                 /* Point to new name                    */
    const char *optr;           /* Point to old name                    */
    int cnt;                    /* Count of characters copied           */

    if ((NULL == newname) || (NULL == oldname) || (buflen <= 0))
    {
        return SYSERR;
    }

    nptr = newname;
    optr = oldname;

    for (cnt = 0; cnt < buflen; cnt++)
    {
        if ((*nptr++ = *optr++) == '\0')
        {
            return OK;
        }
    }
    return SYSERR;              /* Buffer filled before copy completed  */
}

/**
 * @ingroup nam
 *
 * Compute the length of a string, stopping at maxlen.
 *
 * @param name   Name to use
 * @param maxlen Maximum length, including the null character
 * @return Length including the null character, or SYSERR if not terminated
 */
int namlen(const char *name, int maxlen)
{
    int i;                      /* Count of characters found            */

    if (NULL == name)
    {
        return SYSERR;
    }

    for (i = 0; i < maxlen; i++)
    {
        if (*name++ == '\0')
        {
            return i + 1;       /* Include null in length               */
        }
    }
    return SYSERR;
}
