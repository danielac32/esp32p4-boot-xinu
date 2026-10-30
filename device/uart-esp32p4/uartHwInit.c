/**
 * @file uartHwInit.c
 *
 * ESP32-P4 UART0 hardware initialization.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <uart.h>
#include <interrupt.h>
#include "p4uart.h"

/* El RXD no entra al periférico desde su pad por defecto: la UART lee el
 * puerto de entrada nº 10 de la matriz GPIO (UART0_RXD_PAD_IN_IDX) y ese
 * puerto conserva lo que haya programado el ROM, que puede ser otro pin.
 * Direcciones de esp-idf/components/soc/esp32p4/register/soc:
 *   GPIO   = 0x500E0000, func_in_sel_cfg[i] = +0x158 + 4i
 *   IO_MUX = 0x500E1000, gpio[n] = +0x4 + 4n  (fun_ie[9], mcu_sel[14:12]) */
#define P4_GPIO_BASE        0x500E0000UL
#define P4_IO_MUX_BASE      0x500E1000UL
#define P4_UART0_RXD_IN_IDX 10
#define P4_UART0_RXD_PAD    38
#define P4_GPIO_IN_SEL(i)   \
    (*(volatile uint32_t *)(P4_GPIO_BASE + 0x158UL + 4UL * (i)))
#define P4_IOMUX_PAD(n)     \
    (*(volatile uint32_t *)(P4_IO_MUX_BASE + 4UL + 4UL * (n)))

devcall uartHwInit(device *devptr)
{
    volatile struct p4_uart_csreg *regptr = devptr->csr;
    volatile uint32_t *clkctrl = (volatile uint32_t *)P4_UART0_CLK_CTRL;
    volatile uint32_t *clkconf = &P4_REG(regptr, P4_UART_CLK_CONF);
    uint conf0, conf1;

    /* Reloj de sistema/APB del bloque UART0. */
    *(volatile uint32_t *)(HP_SYS_CLKRST_BASE + 0x18) |= (1UL << 18);
    *(volatile uint32_t *)(HP_SYS_CLKRST_BASE + 0x1C) |= (1UL << 7);

    /* El datapath vive en el clock domain del sclk: sin tx/rx_sclk_en (bits
     * 24/25 de CLK_CONF, no los bajos) el receptor no muestrea y no se llena
     * el RX FIFO, aunque el TX siga funcionando.  (IDF: uart_ll_sclk_enable) */
    *clkctrl |= (1UL << 26);                          /* reg_uart0_clk_en */
    *clkconf |= P4_UART_CLK_EN_BITS;

    conf0 = regptr->conf0;
    conf1 = regptr->conf1;

    /* 8N1.  Se escriben solo los bits del formato: conf0 entero apaga
     * mem_clk_en (bit 20), el gate de reloj de la RAM de las FIFO. */
    regptr->conf0 = (conf0 & ~(3UL << 2)) | P4_UART_CONF0_8N1
                    | P4_UART_CONF0_MEM_CLK_EN;

    /* Baud. */
    regptr->clkdiv = P4_UART_CLKDIV(UART_BAUD);

    /* Umbral RX bajo (con 120 la interrupción FULL nunca dispara con
     * entrada interactiva) y clk_en (bit 21), que fuerza el reloj de
     * registros: con 0 el reloj solo vive durante las escrituras. */
    regptr->conf1 = (conf1 & ~0xFFFFUL) | 8UL | P4_UART_CONF1_CLK_EN;

    /* Timeout de RX: sin rx_tout_en el contador de idle no corre y las
     * teclas sueltas (1 o 2 bytes) no generan interrupción.  Thrhd va en
     * bits [11:2], en bit-times. */
    P4_REG(regptr, P4_UART_TOUT_CONF) = (P4_UART_TOUT_BITS << 2) | 1UL;

    /* conf0/clkdiv/tout_conf son registros _sync: sin este pulso no se
     * latchean al clock domain de la UART. */
    P4_REG(regptr, P4_UART_REG_UPDATE) = 1UL;

    /* Entrada RXD: pad 38 en su función 0 (UART0_RXD_PAD), con el receptor
     * de entrada activo (fun_ie), y el puerto 10 sin pasar por la matriz
     * (sig_in_sel=0) para que use esa conexión directa del IOMUX. */
    P4_IOMUX_PAD(P4_UART0_RXD_PAD) =
        (P4_IOMUX_PAD(P4_UART0_RXD_PAD) & ~((7UL << 12) | (1UL << 15)))
        | (1UL << 9);
    P4_GPIO_IN_SEL(P4_UART0_RXD_IN_IDX) = P4_UART0_RXD_PAD;

    /* Limpia todo estado RX pendiente (incluye ovf y brk heredados). */
    regptr->int_clr = P4_UART_INT_RXFIFO_FULL | P4_UART_INT_RXFIFO_TOUT
                    | P4_UART_INT_RXFIFO_OVF  | P4_UART_INT_BRK_DET;

    /* Habilita solo FULL y TOUT (ovf/brk quedan limpios pero sin enable). */
    regptr->int_ena = P4_UART_INT_RXFIFO_FULL | P4_UART_INT_RXFIFO_TOUT;

    /* Instalación del handler. */
    interruptVector[devptr->irq] = devptr->intr;
    enable_irq(devptr->irq);

    return OK;
}
