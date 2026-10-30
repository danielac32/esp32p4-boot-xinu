/**
 * @file mutex.c
 *
 * Mutex no recursivo sobre un semáforo binario.  El dueño se guarda para
 * detectar dos errores típicos en vez de colgarse o corromper el contador:
 *
 *   - mtxlock() del mismo hilo que ya lo posee  -> SYSERR
 *   - mtxunlock() de un hilo que no lo posee     -> SYSERR
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <mutex.h>
#include <semaphore.h>
#include <interrupt.h>

struct mutent muttab[NMUTEX];

mutex mtxcreate(void)
{
    irqmask im;
    int m;

    im = disable();

    for (m = 0; m < NMUTEX; m++)
    {
        if (muttab[m].state == MUTEX_FREE)
        {
            muttab[m].state = MUTEX_USED;
            muttab[m].owner = BADTID;
            muttab[m].users = 0;
            muttab[m].sem = semcreate(1);

            if ((semaphore)SYSERR == muttab[m].sem)
            {
                muttab[m].state = MUTEX_FREE;
                restore(im);
                return SYSERR;
            }

            restore(im);
            return (mutex)m;
        }
    }

    restore(im);
    return SYSERR;
}

syscall mtxlock(mutex m)
{
    irqmask im;

    if (m >= NMUTEX || muttab[m].state != MUTEX_USED)
    {
        return SYSERR;
    }

    im = disable();
    if (muttab[m].owner == thrcurrent)
    {
        /* Doble lock: es un error de diseño, no un candado reentrante. */
        restore(im);
        return SYSERR;
    }

    /* Marca que hay un hilo usando el mutex: mtxfree() no puede liberarlo
     * mientras estemos entre la comprobación y wait(). */
    muttab[m].users++;
    restore(im);

    if (SYSERR == wait(muttab[m].sem))
    {
        return SYSERR;
    }

    im = disable();
    if (muttab[m].users > 0)
    {
        muttab[m].users--;
    }
    muttab[m].owner = thrcurrent;
    restore(im);

    return OK;
}

syscall mtxunlock(mutex m)
{
    irqmask im;

    im = disable();

    if (m >= NMUTEX || muttab[m].state != MUTEX_USED ||
        muttab[m].owner != thrcurrent)
    {
        restore(im);
        return SYSERR;
    }

    muttab[m].owner = BADTID;
    restore(im);

    return signal(muttab[m].sem);
}

syscall mtxfree(mutex m)
{
    irqmask im;

    im = disable();

    if (m >= NMUTEX || muttab[m].state != MUTEX_USED ||
        muttab[m].owner != BADTID)
    {
        restore(im);
        return SYSERR;
    }

    /* No liberar si alguien está esperando el semáforo o dentro de lock(). */
    if (muttab[m].users != 0 || semcount(muttab[m].sem) != 1)
    {
        restore(im);
        return SYSERR;
    }

    muttab[m].state = MUTEX_FREE;
    restore(im);

    return semfree(muttab[m].sem);
}

int mtxcount(mutex m)
{
    if (m >= NMUTEX || muttab[m].state != MUTEX_USED)
    {
        return SYSERR;
    }

    return (muttab[m].owner == BADTID) ? 0 : 1;
}
