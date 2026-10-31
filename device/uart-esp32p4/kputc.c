/**
 * @file kputc.c
 *
 * Synchronous character output to the ESP32-P4 UART0, mirrored onto the LCD
 * text console so the kernel's log is readable on the board's screen.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <uart.h>
#include <lcd.h>
#include "p4uart.h"

syscall kputc(uchar c, device *devptr)
{
    struct uart *uartptr = &uarttab[devptr->minor];
    volatile struct p4_uart_csreg *regptr = devptr->csr;

    while (P4_UART_STATUS_TXCNT(regptr->status) >= (UART_FIFO_LEN - 8))
    {
        /* Wait for room in the TX FIFO. */
    }

    regptr->fifo = (uint32_t)c;
    uartptr->cout++;
    //lcdConsolePutc(c);
    return c;
}
