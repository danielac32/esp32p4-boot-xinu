/**
 * @file fslock.c
 *
 * Candado global del sistema de ficheros (ver include/fslock.h).  Inicializacion
 * perezosa y segura entre hebras.
 */
#include <kernel.h>
#include <interrupt.h>
#include <mutex.h>
#include <fslock.h>

static mutex fsmtx;
static int fsready;

static void fs_init(void)
{
    irqmask im = disable();
    if (!fsready)
    {
        fsmtx = mtxcreate();
        fsready = 1;
    }
    restore(im);
}

void fs_lock(void)
{
    if (!fsready)
    {
        fs_init();
    }
    mtxlock(fsmtx);
}

void fs_unlock(void)
{
    if (fsready)
    {
        mtxunlock(fsmtx);
    }
}
