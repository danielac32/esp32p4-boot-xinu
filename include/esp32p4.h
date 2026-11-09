/**
 * @file esp32p4.h
 *
 * ESP32-P4 SoC register map for the Xinu kernel: peripheral base addresses,
 * interrupt numbers and the RISC-V core peripherals (CLIC/CLINT).
 *
 * Only symbolic @c *_BASE and @c *_IRQn names are provided; this header never
 * defines a bare peripheral macro (such as @c GPIO or @c I2C0), so the device
 * names generated in conf.h (SERIAL0, GPIO0, I2C0, I2S0, ...) can never
 * collide with it.
 *
 * The SBL/firmware build (machine/) uses Espressif's full generated register
 * header (machine/include/esp32p4.h) instead; that one is not visible to the
 * kernel.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef ESP32_P4_H
#define ESP32_P4_H

#include <stdint.h>

/* ======================================================================
 * Peripheral base addresses
 * ====================================================================== */

/* UART */
#define UART0_BASE              0x500CA000UL
#define UART1_BASE              0x500CB000UL
#define UART2_BASE              0x500CC000UL
#define UART3_BASE              0x500CD000UL
#define UART4_BASE              0x500CE000UL

/* I2C / I3C */
#define I2C0_BASE               0x500C4000UL
#define I2C1_BASE               0x500C5000UL

/* I2S */
#define I2S0_BASE               0x500C6000UL
#define I2S1_BASE               0x500C7000UL
#define I2S2_BASE               0x500C8000UL

/* SPI */
#define SPI0_BASE               0x5008C000UL
#define SPI1_BASE               0x5008D000UL
#define SPI2_BASE               0x500D0000UL
#define SPI3_BASE               0x500D1000UL

/* Display / camera */
#define LCD_CAM_BASE            0x500DC000UL

/* Storage */
#define SDHOST_BASE             0x50083000UL

/* DMA engines */
#define DMA_BASE                0x50081000UL
#define AHB_DMA_BASE            0x50085000UL
#define AXI_DMA_BASE            0x5008A000UL

/* GPIO / pin mux */
#define GPIO_BASE               0x500E0000UL
#define GPIO_SD_BASE            0x500E0F00UL
#define IO_MUX_BASE             0x500E1000UL

/* System / clock / reset */
#define HP_SYS_BASE             0x500E5000UL
#define HP_SYS_CLKRST_BASE      0x500E6000UL
#define LP_SYS_BASE             0x50110000UL
#define LP_AON_CLKRST_BASE      0x50111000UL
#define PMU_BASE                0x50115000UL

/* Interrupt matrix (ETS -> CLIC line routing) */
#define INTERRUPT_CORE0_BASE    0x500D6000UL
#define INTERRUPT_CORE1_BASE    0x500D6800UL

/* Timers */
#define SYSTIMER_BASE           0x500E2000UL
#define TIMG0_BASE              0x500C2000UL
#define TIMG1_BASE              0x500C3000UL
#define LP_TIMER_BASE           0x50112000UL
#define LP_WDT_BASE             0x50116000UL

/* Other HP peripherals */
#define MCPWM0_BASE             0x500C0000UL
#define MCPWM1_BASE             0x500C1000UL
#define PCNT_BASE               0x500C9000UL
#define RMT_BASE                0x500D4000UL
#define LEDC_BASE               0x500D3000UL
#define PARL_IO_BASE            0x500CF000UL
#define USB_DEVICE_BASE         0x500D2000UL
#define USB_WRAP_BASE           0x50080000UL

/* Low-power (ULP) peripherals */
#define LP_PERI_BASE            0x50120000UL
#define LP_GPIO_BASE            0x5012A000UL
#define LP_IO_MUX_BASE          0x5012B000UL
#define LP_I2C0_BASE            0x50122000UL
#define LP_I2S0_BASE            0x50125000UL
#define LP_UART_BASE            0x50121000UL

/* ======================================================================
 * Interrupt numbers (CLIC lines / ETS sources)
 * ====================================================================== */
#define LP_WDT_IRQn             1
#define LP_TIMER0_IRQn          2
#define LP_TIMER1_IRQn          3
#define PMU0_IRQn               6
#define PMU1_IRQn               7
#define LP_GPIO_IRQn            10
#define LP_I2C0_IRQn            11
#define LP_I2S0_IRQn            12
#define LP_UART_IRQn            16
#define USB_DEVICE_IRQn         22
#define DMA_IRQn                24
#define SPI2_IRQn               25
#define SPI3_IRQn               26
#define I2S0_IRQn               27
#define I2S1_IRQn               28
#define I2S2_IRQn               29
#define UHCI0_IRQn              30
#define UART0_IRQn              31
#define UART1_IRQn              32
#define UART2_IRQn              33
#define UART3_IRQn              34
#define UART4_IRQn              35
#define TWAI0_IRQn              40
#define RMT_IRQn                43
#define I2C0_IRQn               44
#define I2C1_IRQn               45
#define TG0_T0_IRQn             46
#define TG0_T1_IRQn             47
#define LEDC_IRQn               52
#define SYSTIMER_TARGET0_IRQn   53
#define GPIO_INT0_IRQn          74
#define CACHE_IRQn              83
#define HP_SYS_IRQn             110
#define PCNT_IRQn               111

/* ======================================================================
 * RISC-V core peripherals (not part of the peripheral SVD)
 * ====================================================================== */

/* Espressif hardware-vectored CLIC: 4-byte packed control block per line
 * starting at +0x1000.  The P4 port implements the first 48 lines (enough
 * for the UART/I2C/I2S/SPI/DMA lines used so far). */
#define P4_CLIC_BASE            0x20800000UL
#define P4_CLIC_NIRQ            48

/* RISC-V CLINT: machine timer (mtime) and timer compare (mtimecmp). */
#define P4_CLINT_BASE           0x20000000UL
#define P4_MTIMECMP_LO          (*(volatile uint32_t *)(P4_CLINT_BASE + 0x4000UL))
#define P4_MTIMECMP_HI          (*(volatile uint32_t *)(P4_CLINT_BASE + 0x4004UL))
#define P4_MTIME_LO             (*(volatile uint32_t *)(P4_CLINT_BASE + 0xBFF8UL))
#define P4_MTIME_HI             (*(volatile uint32_t *)(P4_CLINT_BASE + 0xBFFCUL))
#define P4_MTIMECTL             (*(volatile uint32_t *)(P4_CLINT_BASE + 0x4010UL))

/* ======================================================================
 * Clock tree and memory map
 * ====================================================================== */
#define P4_TIMER_FREQ           400000000UL /* CPLL root clock == mtime tick (SBL clock_init) */
#define P4_XTAL_FREQ            40000000UL  /* 40 MHz crystal (UART0 sclk)  */

#define P4_L2MEM_BASE           0x4FF00000UL /* internal SRAM used by kernel */
#define P4_PSRAM_BASE           0x48000000UL /* mapped by the SBL (MMU)      */

#endif                          /* ESP32_P4_H */
