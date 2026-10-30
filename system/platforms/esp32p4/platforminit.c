/**
 * @file platforminit.c
 *
 * ESP32-P4 platform specific initialization.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <platform.h>
#include <string.h>
#include <stdint.h>
#include <kernel.h>
#include <riscv.h>
#include <trap.h>
#include <esp32p4.h>            /* shared SoC register map (base addresses) */

/* CLIC (Espressif, at P4_CLIC_BASE) - provided by clic.c */
extern void clic_init(void);

/* Set by _start (loader/platforms/esp32p4/start.S). */
void *panicstk;

/* Free L2MEM end (top of the internal SRAM the kernel runs from). */
extern unsigned char _l2mem_free_end[];

/**
 * ESP32-P4 hardware platform specific initialization.
 *
 * @return OK
 */
int platforminit(void)
{
    strlcpy(platform.family, "riscv32", PLT_STRMAX);
    strlcpy(platform.name, "esp32p4", PLT_STRMAX);

    /* The kernel heap lives in the internal L2MEM (SRAM), not in PSRAM. */
    platform.minaddr = (void *)P4_L2MEM_BASE;
    platform.maxaddr = (void *)_l2mem_free_end;

    platform.clkfreq = P4_TIMER_FREQ;
    platform.serial_low = 0;
    platform.serial_high = 0;
    /* Interrupt controller: threshold, vector table, all lines masked. */
    clic_init();

    /* ecall (mcause 11) -> syscall gate (system/arch/riscv/trap.c). */
    trap_register(TRAP_CAUSE_ECALL_M, ecall_handler);

    /* mtime sampling mode = 3 (CLINT_MTIMECTL |= 3 << 4). */
    P4_MTIMECTL |= (3UL << 4);

    return OK;
}
