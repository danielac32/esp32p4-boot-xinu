/**
 * @file uartHwPutc.c
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <uart.h>
#include "p4uart.h"

/*
 * Write one character to the UART0 TX FIFO, waiting for room.  This is the
 * synchronous path used by kprintf() and by the (also synchronous) P4
 * uartWrite(); no TX interrupt is needed.
 */
void uartHwPutc(void *csr, uchar c)
{
    volatile struct p4_uart_csreg *regptr = csr;

    while (P4_UART_STATUS_TXCNT(regptr->status) >= (UART_FIFO_LEN - 8))
    {
        /* Wait for room in the TX FIFO. */
    }

    regptr->fifo = (uint32_t)c;
}
