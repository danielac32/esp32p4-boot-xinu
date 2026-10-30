/**
 * @file mqueue.c
 *
 * Colas de mensajes de tamaño variable sobre PSRAM.
 *
 * Cada slot es [uint16 len][payload maxmsg].  Los mensajes se copian, así que
 * el emisor puede reutilizar su buffer al volver.  Dos semáforos hacen la
 * sincronización (slots libres / mensajes disponibles) y los índices del
 * anillo se protegen con disable()/restore(): la sección crítica es un memcpy
 * de como mucho maxmsg bytes y no consume monitores (NMON es escaso).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <mqueue.h>
#include <thread.h>
#include <semaphore.h>
#include <queue.h>
#include <memory.h>
#include <interrupt.h>
#include <string.h>

#define MQ_FREE 0
#define MQ_USED 1
#define MQ_HDR  2               /* uint16 len por slot */

struct mqent
{
    uint8_t state;
    uint16_t nslots;
    uint16_t maxmsg;
    uint16_t slot_size;         /* MQ_HDR + maxmsg, alineado a 4          */
    uint16_t head;
    uint16_t tail;
    uint16_t count;
    uint8_t *slots;             /* nslots * slot_size, en PSRAM           */
    uint32_t total;             /* bytes reservados en PSRAM              */
    semaphore free_slots;       /* nslots                                 */
    semaphore items;            /* 0                                      */
    tid_typ owner;
};

static struct mqent mqtab[NMQUEUE];

static int mq_valid(mqueue q)
{
    return (q < NMQUEUE && mqtab[q].state == MQ_USED);
}

static uint8_t *mq_slot(struct mqent *q, uint16_t index)
{
    return q->slots + ((uint32_t)index * q->slot_size);
}

mqueue mqcreate(uint nslots, uint maxmsg)
{
    irqmask im;
    struct mqent *q;
    uint slot_size;
    uint total;
    int i;

    if (nslots == 0 || nslots > MQ_MAXSLOTS ||
        maxmsg == 0 || maxmsg > MQ_MAXMSG)
    {
        return SYSERR;
    }

    slot_size = (MQ_HDR + maxmsg + 3) & ~3u;
    total = nslots * slot_size;

    im = disable();

    for (i = 0; i < NMQUEUE; i++)
    {
        q = &mqtab[i];
        if (q->state != MQ_FREE)
        {
            continue;
        }

        q->state = MQ_USED;
        q->nslots = (uint16_t)nslots;
        q->maxmsg = (uint16_t)maxmsg;
        q->slot_size = (uint16_t)slot_size;
        q->head = q->tail = q->count = 0;
        q->total = total;
        q->owner = thrcurrent;
        q->slots = psram_getmem(total);

        if (q->slots == NULL || (intptr_t)q->slots == SYSERR)
        {
            q->slots = NULL;
            q->state = MQ_FREE;
            restore(im);
            return SYSERR;
        }

        q->free_slots = semcreate((int)nslots);
        q->items = semcreate(0);

        if ((semaphore)SYSERR == q->free_slots ||
            (semaphore)SYSERR == q->items)
        {
            if ((semaphore)SYSERR != q->free_slots)
            {
                semfree(q->free_slots);
            }
            if ((semaphore)SYSERR != q->items)
            {
                semfree(q->items);
            }
            psram_freemem(q->slots, total);
            q->slots = NULL;
            q->state = MQ_FREE;
            restore(im);
            return SYSERR;
        }

        restore(im);
        return (mqueue)i;
    }

    restore(im);
    return SYSERR;
}

syscall mqsend(mqueue q, const void *buf, uint len)
{
    struct mqent *mq;
    uint8_t *slot;
    irqmask im;

    if (!mq_valid(q) || buf == NULL || len == 0)
    {
        return SYSERR;
    }

    if (len > mqtab[q].maxmsg)
    {
        return SYSERR;
    }

    /* Backpressure: espera un slot libre. */
    if (SYSERR == wait(mqtab[q].free_slots))
    {
        return SYSERR;
    }

    if (!mq_valid(q))
    {
        return SYSERR;
    }

    mq = &mqtab[q];

    im = disable();
    slot = mq_slot(mq, mq->tail);
    *(uint16_t *)slot = (uint16_t)len;
    memcpy(slot + MQ_HDR, buf, len);
    mq->tail = (uint16_t)((mq->tail + 1) % mq->nslots);
    mq->count++;
    restore(im);

    signal(mq->items);
    return OK;
}

static int mq_take(mqueue q, void *buf, uint cap, int have_item)
{
    struct mqent *mq;
    uint8_t *slot;
    uint16_t len;
    irqmask im;

    im = disable();

    if (!mq_valid(q) || mqtab[q].count == 0)
    {
        restore(im);
        return SYSERR;
    }

    mq = &mqtab[q];
    slot = mq_slot(mq, mq->head);
    len = *(uint16_t *)slot;

    if (len > cap)
    {
        /* No consumir el mensaje.  Si el token de items ya se tomó
         * (mqreceive), devolverlo para no perder el mensaje. */
        restore(im);
        if (have_item)
        {
            signal(mq->items);
        }
        return SYSERR;
    }

    if (buf != NULL && len > 0)
    {
        memcpy(buf, slot + MQ_HDR, len);
    }

    mq->head = (uint16_t)((mq->head + 1) % mq->nslots);
    mq->count--;

    restore(im);

    signal(mq->free_slots);
    return (int)len;
}

int mqreceive(mqueue q, void *buf, uint cap)
{
    if (!mq_valid(q))
    {
        return SYSERR;
    }

    if (SYSERR == wait(mqtab[q].items))
    {
        return SYSERR;
    }

    return mq_take(q, buf, cap, 1);
}

int mqtryreceive(mqueue q, void *buf, uint cap)
{
    irqmask im;
    int empty;

    im = disable();
    empty = (!mq_valid(q) || mqtab[q].count == 0);
    restore(im);

    if (empty)
    {
        return SYSERR;
    }

    return mq_take(q, buf, cap, 0);
}

syscall mqfree(mqueue q)
{
    irqmask im;
    struct mqent *mq;

    if (!mq_valid(q))
    {
        return SYSERR;
    }

    mq = &mqtab[q];

    im = disable();

    /* No liberar con mensajes dentro ni con hilos esperando. */
    if (mq->count != 0 ||
        nonempty(semtab[mq->items].queue) ||
        nonempty(semtab[mq->free_slots].queue))
    {
        restore(im);
        return SYSERR;
    }

    restore(im);

    /* Liberar los recursos antes de marcar el hueco como libre: así otra
     * tarea no puede reutilizar el índice mientras aún se está liberando. */
    semfree(mq->items);
    semfree(mq->free_slots);
    psram_freemem(mq->slots, mq->total);

    im = disable();
    mq->slots = NULL;
    mq->total = 0;
    mq->owner = BADTID;
    mq->state = MQ_FREE;
    restore(im);

    return OK;
}

int mqcount(mqueue q)
{
    irqmask im;
    int n;

    im = disable();
    n = mq_valid(q) ? (int)mqtab[q].count : SYSERR;
    restore(im);

    return n;
}
