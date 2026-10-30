/**
 * @file uartInterrupt.c
 *
 * ESP32-P4 UART0 receive interrupt: drain the RX FIFO into the Xinu input
 * buffer and wake up any readers.  (Transmission is synchronous on this
 * port, so there is no TX half.)
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <thread.h>
#include <device.h>
#include <uart.h>
#include "p4uart.h"

extern int resdefer;

interrupt uartInterruptNoRsch(void)
{
    struct uart *uartptr = &uarttab[0];
    volatile struct p4_uart_csreg *regptr;
    int count = 0;

    if (NULL == uartptr)
    {
        return;
    }

    regptr = uartptr->csr;
    if (NULL == regptr)
    {
        return;
    }

    uartptr->iirq++;

    while (P4_UART_STATUS_RXCNT(regptr->status) != 0)
    {
        uchar c = (uchar)(regptr->fifo & 0xFFU);

        if (uartptr->icount < UART_IBLEN)
        {
            uartptr->in[(uartptr->istart + uartptr->icount) % UART_IBLEN] = c;
            uartptr->icount++;
            count++;
        }
        else
        {
            uartptr->ovrrn++;
        }
    }

    uartptr->cin += count;
    if (count > 0)
    {
        signaln(uartptr->isema, count);
    }

    regptr->int_clr = P4_UART_INT_RXFIFO_FULL | P4_UART_INT_RXFIFO_TOUT;
}

interrupt uartInterrupt(void)
{
    /* Defer rescheduling while we drain the FIFO: signaln() then bumps
     * resdefer instead of switching context in the middle of the ISR. */
    resdefer = 1;
    uartInterruptNoRsch();
    if (--resdefer > 0)
    {
        resdefer = 0;
        resched();
    }
}
