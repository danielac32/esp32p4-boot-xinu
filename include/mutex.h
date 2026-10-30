/**
 * @file mutex.h
 *
 * Mutex no recursivo (a diferencia de monitor: el doble lock del mismo hilo
 * devuelve SYSERR en vez de incrementar un contador).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _MUTEX_H_
#define _MUTEX_H_

#include <conf.h>
#include <thread.h>

#define MUTEX_FREE 0
#define MUTEX_USED 1

typedef uint mutex;

struct mutent
{
    uint8_t state;              /**< MUTEX_FREE / MUTEX_USED            */
    uint8_t pad;
    uint16_t users;             /**< hilos dentro de mtxlock()          */
    tid_typ owner;              /**< tid que lo posee, o BADTID         */
    semaphore sem;              /**< binario: 1 libre, 0 tomado         */
};

extern struct mutent muttab[NMUTEX];

mutex mtxcreate(void);
syscall mtxlock(mutex);
syscall mtxunlock(mutex);
syscall mtxfree(mutex);
int mtxcount(mutex);

#endif                          /* _MUTEX_H_ */
