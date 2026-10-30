/**
 * @file chprio.c
 *
 */
/* Embedded Xinu, Copyright (C) 2009.  All rights reserved. */

#include <thread.h>
#include <queue.h>

/**
 * @ingroup threads
 *
 * Change the scheduling priority of a thread
 * @param tid target thread
 * @param newprio new priority
 * @return old priority of thread
 */
syscall chprio(tid_typ tid, int newprio)
{
    register struct thrent *thrptr;     /* thread control block */
    irqmask im;
    int oldprio;

    im = disable();
    if (isbadtid(tid))
    {
        restore(im);
        return SYSERR;
    }
    thrptr = &thrtab[tid];
    oldprio = thrptr->prio;
    thrptr->prio = newprio;

    /* If the target is on the ready list, reinsert it at its new priority
     * so the list stays correctly ordered. */
    if (THRREADY == thrptr->state)
    {
        getitem(tid);
        insert(tid, readylist, newprio);
    }

    restore(im);
    return oldprio;
}
