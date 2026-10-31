/**
 * @file unlock.c
 */
/* Embedded Xinu, Copyright (C) 2009, 2013.  All rights reserved. */

#include <monitor.h>

/**
 * @ingroup monitors
 *
 * Unlock a monitor.
 *
 * The monitor's lock count (indicating the number of times the owning thread
 * has locked the monitor) is decremented.  If the count remains greater than
 * zero, no further action is taken.  If the count reaches zero, the monitor is
 * set to unowned and up to one thread that may be waiting to lock() the monitor
 * is awakened.
 *
 * Only the owning thread may unlock: with lock() handing the monitor over
 * through its semaphore, an unlock from another thread would free a monitor
 * that a running thread still holds.  A monitor left owned by a thread that
 * died is a bug in that thread's caller, and is fixed by unlocking it from
 * a thread that owns it, not from anybody else.
 *
 * @param mon
 *      The monitor to unlock.
 *
 * @return
 *      ::OK on success; ::SYSERR on failure (@p mon did not specify a valid,
 *      allocated monitor, the monitor was not locked, or the current thread
 *      does not own it).
 */
syscall unlock(monitor mon)
{
    register struct monent *monptr;
    irqmask im;

    im = disable();
    if (isbadmon(mon))
    {
        restore(im);
        return SYSERR;
    }

    monptr = &montab[mon];

    /* safety check: the owner has to be the one holding it locked */
    if (0 == monptr->count || thrcurrent != monptr->owner)
    {
        restore(im);
        return SYSERR;
    }

    /* decrement the monitor's count signifying one "unlock" */
    (monptr->count)--;

    /* if this is the top-level unlock call, then free this monitor's lock */
    if (monptr->count == 0)
    {
        monptr->owner = NOOWNER;
        signal(monptr->sem);
    }

    restore(im);
    return OK;
}
