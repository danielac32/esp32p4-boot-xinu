/**
 * @file lock.c
 */
/* Embedded Xinu, Copyright (C) 2009, 2013.  All rights reserved. */

#include <monitor.h>

/**
 * @ingroup monitors
 *
 * Lock a monitor.
 *
 * If the current thread already owns the monitor, its count is incremented and
 * no further action is taken.  Otherwise the thread waits until the owning
 * thread has fully unlocked the monitor, then becomes its owner with a count
 * of 1.
 *
 * The semaphore is what hands the monitor over, so it is taken before
 * ownership is claimed.  Claiming first and waiting afterwards deadlocks:
 * signal() can already have promised the wake-up to a blocked waiter, so a
 * thread that finds the monitor unowned blocks inside its own claim, and the
 * monitor ends up owned by whichever thread collects the promised wake-up --
 * possibly a thread that then exits, leaving the monitor locked forever.
 *
 * @param mon
 *      The monitor to lock.
 *
 * @return
 *      ::OK on success; ::SYSERR on failure (@p mon did not specify a valid,
 *      allocated monitor).
 */
syscall lock(monitor mon)
{
    struct monent *monptr;
    irqmask im;

    im = disable();
    if (isbadmon(mon))
    {
        restore(im);
        return SYSERR;
    }

    monptr = &montab[mon];

    /* the owner re-entering the monitor only deepens the count */
    if (thrcurrent == monptr->owner)
    {
        (monptr->count)++;
        restore(im);
        return OK;
    }

    restore(im);

    /* Interrupts stay enabled for the wait: this is where the thread may be
     * taken off the CPU, and resched() only acts when they are. */
    wait(monptr->sem);

    /* the previous owner has fully unlocked; this thread owns the monitor */
    im = disable();
    monptr = &montab[mon];
    monptr->owner = thrcurrent;
    monptr->count = 1;
    restore(im);
    return OK;
}
