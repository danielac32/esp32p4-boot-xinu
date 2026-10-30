/**
 * @file stack.c
 *
 * High-water mark de pila por tarea.  En cada cambio de contexto se anota el
 * SP con el que la tarea sale; el máximo de (top - sp) es una cota superior
 * del uso real (incluye el marco de resched/IRQ, que también ocupa pila).
 *
 * La tarea nula (tid 0) es especial: no pasa por create(), y
 * initialize.c le asigna stkbase = &_end (la BASE, no el top) y
 * stklen = memheap - &_end.  Por eso su región es [_end, memheap).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stack.h>
#include <memory.h>

static uint32_t stack_low(tid_typ tid)
{
    if (tid == NULLTHREAD)
    {
        return (uint32_t)&_end;
    }

    return (uint32_t)thrtab[tid].stkbase
         - (uint32_t)roundmb(thrtab[tid].stklen)
         + (uint32_t)sizeof(ulong);
}

static uint32_t stack_top(tid_typ tid)
{
    if (tid == NULLTHREAD)
    {
        return (uint32_t)memheap;
    }

    return (uint32_t)thrtab[tid].stkbase + (uint32_t)sizeof(ulong);
}

uint32_t stack_total(tid_typ tid)
{
    if (tid < 0 || tid >= NTHREAD || thrtab[tid].state == THRFREE)
    {
        return 0;
    }

    return stack_top(tid) - stack_low(tid);
}

void stack_note(tid_typ tid, uint32_t sp)
{
    struct thrent *t;
    uint32_t top;
    uint32_t low;
    uint32_t used;

    if (tid < 0 || tid >= NTHREAD)
    {
        return;
    }

    t = &thrtab[tid];
    if (t->state == THRFREE)
    {
        return;
    }

    top = stack_top(tid);
    low = stack_low(tid);

    if (sp <= top && sp >= low)
    {
        used = top - sp;
        if (used > t->stk_peak)
        {
            t->stk_peak = used;
        }
    }
    else
    {
        /* El SP no está en la pila de la tarea: o hay corrupción o la
         * medición viene de una pila distinta.  Se marca y se satura. */
        t->stk_overflow = TRUE;
        used = stack_total(tid);
        if (used > t->stk_peak)
        {
            t->stk_peak = used;
        }
    }
}

uint32_t stack_peak(tid_typ tid)
{
    if (tid < 0 || tid >= NTHREAD)
    {
        return 0;
    }

    return thrtab[tid].stk_peak;
}

uint32_t stack_free(tid_typ tid)
{
    uint32_t total = stack_total(tid);
    uint32_t used = stack_peak(tid);

    return (used < total) ? (total - used) : 0;
}

int stack_overflow(tid_typ tid)
{
    if (tid < 0 || tid >= NTHREAD)
    {
        return FALSE;
    }

    return thrtab[tid].stk_overflow ? TRUE : FALSE;
}

void stack_reset(tid_typ tid)
{
    if (tid < 0 || tid >= NTHREAD)
    {
        return;
    }

    thrtab[tid].stk_peak = 0;
    thrtab[tid].stk_overflow = FALSE;
}

void stack_reset_all(void)
{
    tid_typ tid;

    for (tid = 0; tid < NTHREAD; tid++)
    {
        stack_reset(tid);
    }
}
