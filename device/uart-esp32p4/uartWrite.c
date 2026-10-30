/**
 * @file uartWrite.c
 *
 * ESP32-P4 synchronous UART output.  Unlike the generic buffered version,
 * the P4 writes every byte straight into the TX FIFO (uartHwPutc waits for
 * room).  This keeps the console working without a TX interrupt, which on
 * the P4 is level-triggered and would otherwise fire continuously while the
 * transmitter is idle.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <uart.h>
#include <interrupt.h>

devcall uartWrite(device *devptr, const void *buf, uint len)
{
    struct uart *uartptr;
    uint count;

    uartptr = &uarttab[devptr->minor];
    if (NULL == uartptr->csr)
    {
        return SYSERR;
    }

    for (count = 0; count < len; count++)
    {
        uartHwPutc(uartptr->csr, ((const uchar *)buf)[count]);
        uartptr->cout++;
    }

    return count;
}
