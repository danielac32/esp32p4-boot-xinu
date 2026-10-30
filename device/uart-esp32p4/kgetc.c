/**
 * @file kgetc.c
 *
 * Synchronous character input from the ESP32-P4 UART0 (polls the RX FIFO).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <uart.h>
#include "p4uart.h"

syscall kgetc(device *devptr)
{
    struct uart *uartptr = &uarttab[devptr->minor];
    volatile struct p4_uart_csreg *regptr = devptr->csr;
    uchar c;

    while (P4_UART_STATUS_RXCNT(regptr->status) == 0)
    {
        /* Wait for a character. */
    }

    c = (uchar)(regptr->fifo & 0xFFU);
    uartptr->cin++;
    return c;
}
