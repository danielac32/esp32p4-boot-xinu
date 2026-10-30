/**
 * @file p4uart.h
 *
 * ESP32-P4 UART0 control and status registers (a subset of Espressif's
 * UART0_Type, verified against esp32p4.svd).  UART0_BASE = 0x500CA000.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _P4UART_H_
#define _P4UART_H_

#include <stdint.h>
#include <conf.h>
#include <esp32p4.h>

/* UART0 is at UART0_BASE (0x500CA000).  To bring it up the device must
 * ungated its clocks in the HP system clock/reset block:
 *   HP_SYS_CLKRST +0x18 SOC_CLK_CTRL1: REG_UART0_SYS_CLK_EN = bit 18
 *   HP_SYS_CLKRST +0x1C SOC_CLK_CTRL2: REG_UART0_APB_CLK_EN = bit 7  */

struct p4_uart_csreg
{
    volatile uint32_t fifo;     /* 0x00  RX read / TX write                */
    volatile uint32_t int_raw;  /* 0x04                                    */
    volatile uint32_t int_st;   /* 0x08                                    */
    volatile uint32_t int_ena;  /* 0x0C                                    */
    volatile uint32_t int_clr;  /* 0x10                                    */
    volatile uint32_t clkdiv;   /* 0x14  CLKDIV[11:0], CLKDIV_FRAG[23:20] */
    volatile uint32_t rx_filt;  /* 0x18                                    */
    volatile uint32_t status;   /* 0x1C  RXFIFO_CNT[7:0], TXFIFO_CNT[23:16]*/
    volatile uint32_t conf0;    /* 0x20  PARITY, PARITY_EN, BIT_NUM, ...   */
    volatile uint32_t conf1;    /* 0x24  RX/TX FIFO empty thresholds       */
};

/* Interrupt bits (INT_RAW/INT_ST/INT_ENA/INT_CLR) */
#define P4_UART_INT_RXFIFO_FULL  (1UL << 0)
#define P4_UART_INT_TXFIFO_EMPTY (1UL << 1)
#define P4_UART_INT_RXFIFO_TOUT  (1UL << 8)
#define P4_UART_INT_ALL  0xFFFFFFFFUL
/* STATUS fields */
#define P4_UART_STATUS_RXCNT(s) (((s) >> 0) & 0xFFUL)
#define P4_UART_STATUS_TXCNT(s) (((s) >> 16) & 0xFFUL)

/* CONF0: BIT_NUM = 3 -> 8 data bits; PARITY_EN = 0; STOP_BIT_NUM = 0 (1) */
#define P4_UART_CONF0_8N1 ((3UL << 2))

/* Bits de CONF0/CONF1 verificados contra esp-idf .../soc/uart_struct.h.
 * OJO: en el P4 el bucle interno es el bit 12, no el 21 (el 21 es sw_rts).
 * mem_clk_en y clk_en son gates de reloj: clk_en=0 deja el reloj de
 * registros vivo solo durante las escrituras, y mem_clk_en=0 apaga la RAM
 * de las FIFO.  Con cualquiera de los dos apagados el receptor no avanza
 * aunque el TX siga escupiendo bytes. */
#define P4_UART_CONF0_LOOPBACK   (1UL << 12)
#define P4_UART_CONF0_MEM_CLK_EN (1UL << 20)
#define P4_UART_CONF0_RXFIFO_RST (1UL << 22)
#define P4_UART_CONF0_TXFIFO_RST (1UL << 23)
#define P4_UART_CONF1_CLK_EN     (1UL << 21)

/* UART0 source clock (ROM leaves UART0 on the 40 MHz XTAL) */
#define P4_UART_SCLK 40000000UL
#define P4_UART_CLKDIV(baud) (((P4_UART_SCLK) + (baud) / 2UL) / (baud))


/* --- Offsets verificados contra esp-idf/components/soc/esp32p4/register/soc/uart_reg.h ---
 * OJO: el SVD empaquetado en machine/sbl/esp32p4.h tiene los campos de CLK_CONF
 * mal ordenados (los pone en [3:0]); el layout real de la UART0 externa es el
 * de IDF, y es el que usan las constantes de abajo. */
#define P4_UART_TOUT_CONF   0x64  /* rx_tout_en[0], rx_tout_flow_dis[1], rx_tout_thrhd[11:2] */
#define P4_UART_RXD_CNT     0x84  /* rxd_edge_cnt[9:0] (RO) */
#define P4_UART_CLK_CONF    0x88  /* tx_sclk_en[24], rx_sclk_en[25], tx_rst_core[26], rx_rst_core[27] */
#define P4_UART_REG_UPDATE  0x98  /* bit0: pulso para latchear los registros _sync */

#define P4_UART_CLK_EN_BITS ((1UL << 24) | (1UL << 25)) /* tx_sclk_en | rx_sclk_en */

/* HP_SYS_CLKRST: reloj de fuente de la UART0 (IDF: peri_clk_ctrl110).
 *   bit 26 reg_uart0_clk_en (default 1), bits[25:24] reg_uart0_clk_src_sel,
 *   bits[7:0] reg_uart0_sclk_div_num.  src_sel 0 = XTAL (40 MHz). */
#define P4_UART0_CLK_CTRL   (HP_SYS_CLKRST_BASE + 0x128)

/* Acceso a registros que tu struct no alcanza (llega solo hasta 0x24) */
#define P4_REG(regptr, off) (*(volatile uint32_t *)((uint8_t *)(regptr) + (off)))

/* Timeout de RX en bit-times (10 ≈ 87 µs a 115200) */
#define P4_UART_TOUT_BITS   10UL

/* Bits extra (verificados): ovf = bit 4, brk_det = bit 7 */
#define P4_UART_INT_RXFIFO_OVF  (1UL << 4)
#define P4_UART_INT_BRK_DET     (1UL << 7)

#endif                          /* _P4UART_H_ */
