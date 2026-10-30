/**
 * @file uartHwStat.c
 *
 * Volcado de estado del camino de recepción completo de UART0: registros de
 * la periférica, enrutado del pad RXD y bloque de control del CLIC.  Lo
 * consume el comando "uartstat" del shell.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <uart.h>
#include <stdio.h>
#include <esp32p4.h>
#include "p4uart.h"

/* Un bloque de control por línea del CLIC: ip, ie, attr, ct (4 bytes). */
#define CLIC_CTRL(line) (*(volatile uint32_t *)(P4_CLIC_BASE + 0x1000UL + \
                                                 4UL * (line)))
#define CLIC_CFG()      (*(volatile uint32_t *)(P4_CLIC_BASE + 0x0))
#define CLIC_INFO()     (*(volatile uint32_t *)(P4_CLIC_BASE + 0x4))
#define CLIC_THRESH()   (*(volatile uint32_t *)(P4_CLIC_BASE + 0x8))
/* Fuente ETS n -> registro de mapa de la matriz (IDF: +4*intr_src). */
#define MATRIX_REG(src) (*(volatile uint32_t *)(INTERRUPT_CORE0_BASE + \
                                                4UL * (src)))
/* Enrutado del pad RXD (ver uartHwInit.c). */
#define PAD38_REG       (*(volatile uint32_t *)(0x500E1000UL + 4UL + 4UL * 38UL))
#define RXD_IN_SEL      (*(volatile uint32_t *)(0x500E0000UL + 0x158UL + 4UL * 10UL))

void uartHwStat(void *csr)
{
    volatile struct p4_uart_csreg *regptr = csr;
    uint ctrl = CLIC_CTRL(31);

    printf("\n\tESP32-P4 UART0 (periferica)\n");
    printf("\t------------------------------------------\n");
    printf("\tINT_RAW  = 0x%08X\n", (uint)regptr->int_raw);
    printf("\tINT_ST   = 0x%08X\n", (uint)regptr->int_st);
    printf("\tINT_ENA  = 0x%08X\n", (uint)regptr->int_ena);
    printf("\tSTATUS   = 0x%08X  (rxcnt=%d rxd=%d txcnt=%d)\n",
           (uint)regptr->status, (int)(regptr->status & 0xFFU),
           (int)((regptr->status >> 15) & 1U),
           (int)((regptr->status >> 16) & 0xFFU));
    printf("\tCLKDIV   = 0x%08X\n", (uint)regptr->clkdiv);
    printf("\tCONF0    = 0x%08X\n", (uint)regptr->conf0);
    printf("\tCONF1    = 0x%08X\n", (uint)regptr->conf1);
    printf("\tTOUT_CONF= 0x%08X\n", (uint)P4_REG(regptr, P4_UART_TOUT_CONF));
    printf("\tCLK_CONF = 0x%08X\n", (uint)P4_REG(regptr, P4_UART_CLK_CONF));
    printf("\tRXD_CNT  = 0x%08X\n", (uint)P4_REG(regptr, P4_UART_RXD_CNT));
    printf("\tAFIFO_ST = 0x%08X\n", (uint)P4_REG(regptr, 0x90));

    printf("\tCamino de entrada RXD\n");
    printf("\t------------------------------------------\n");
    printf("\tPAD38    = 0x%08X  (func=%d ie=%d)\n", (uint)PAD38_REG,
           (int)((PAD38_REG >> 12) & 7U), (int)((PAD38_REG >> 9) & 1U));
    printf("\tIN_SEL10 = 0x%08X  (src=%d por_matriz=%d)\n", (uint)RXD_IN_SEL,
           (int)(RXD_IN_SEL & 0x3FU), (int)((RXD_IN_SEL >> 7) & 1U));

    printf("\tMatriz / CLIC (linea 31)\n");
    printf("\t------------------------------------------\n");
    printf("\tMAP[31]  = 0x%08X\n", (uint)MATRIX_REG(31));
    printf("\tCLIC_CTRL= 0x%08X  (ip=%d ie=%d attr=0x%02X ct=0x%02X)\n",
           ctrl, (int)(ctrl & 0xFF), (int)((ctrl >> 8) & 0xFF),
           (int)((ctrl >> 16) & 0xFF), (int)((ctrl >> 24) & 0xFF));
    printf("\tCLIC_CFG = 0x%08X  INFO=0x%08X  THRESH=0x%08X\n",
           (uint)CLIC_CFG(), (uint)CLIC_INFO(), (uint)CLIC_THRESH());

    printf("\tContadores Xinu: cin=%d cout=%d iirq=%d ovrrn=%d lserr=%d\n",
           (int)uarttab[0].cin, (int)uarttab[0].cout, (int)uarttab[0].iirq,
           (int)uarttab[0].ovrrn, (int)uarttab[0].lserr);
}
