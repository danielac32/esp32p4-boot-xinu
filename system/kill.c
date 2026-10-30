/**
 * @file kill.c
 *
 */
/* Embedded Xinu, Copyright (C) 2009.  All rights reserved. */

#include <thread.h>
#include <queue.h>
#include <memory.h>

extern void xdone(void);

/**
 * @ingroup threads
 *
 * Kill a thread and remove it from the system
 * @param tid target thread
 * @return OK on success, SYSERR otherwise
 */
syscall kill(tid_typ tid)
{
    register struct thrent *thrptr;     /* thread control block */
    irqmask im;

    im = disable();
    if (isbadtid(tid) || (NULLTHREAD == tid))
    {
        restore(im);
        return SYSERR;
    }
    thrptr = &thrtab[tid];

    if (--thrcount <= 1)
    {
        xdone();
    }

    send(thrptr->parent, tid);

    stkfree(thrptr->stkbase, thrptr->stklen);

    /* Liberar la imagen ELF cargada en PSRAM si esta tarea la uso */
    if (thrptr->elf_task && thrptr->elf_img != NULL)
    {
        psram_freemem(thrptr->elf_img, thrptr->elf_size);
        thrptr->elf_img = NULL;
        thrptr->elf_size = 0;
        thrptr->elf_task = FALSE;
    }

    switch (thrptr->state)
    {
    case THRSLEEP:
    case THRTMOUT:
        /* THRTMOUT también está en sleepq (recvtime); hay que sacarlo o la
         * cola de sleepers queda con un tid libre. */
        unsleep(tid);
        thrptr->state = THRFREE;
        break;
    case THRCURR:
        thrptr->state = THRFREE;        /* suicide */
        resched();

    case THRWAIT:
        semtab[thrptr->sem].count++;

    case THRREADY:
        getitem(tid);           /* removes from queue */

    default:
        thrptr->state = THRFREE;
    }

    restore(im);
    return OK;
}
