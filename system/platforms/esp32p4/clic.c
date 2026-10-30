/**
 * @file clic.c
 *
 * ESP32-P4 interrupt controller (Espressif CLIC) for Embedded Xinu.
 *
 * The P4 has a non-standard, hardware-vectored CLIC at 0x20800000 with a
 * 4-byte packed control block per interrupt at 0x20801000 (verified
 * against the HAL: soc/clic_reg.h).  When a line fires and its attr.SHV
 * bit is set, the core jumps *directly* to InterruptVectorTable[irq]
 * (the table loaded in mtvt).  Therefore the table entries are small
 * trampolines that call the Xinu handler registered in interruptVector[].
 *
 * External lines (16-47) additionally need the interrupt matrix to route
 * an ETS source to the line; every map register resets to 0, so we
 * identity-route source n -> line n (same formula as the HAL's
 * interrupt_clic_ll_route()).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <kernel.h>
#include <interrupt.h>
#include <stdint.h>
#include <thread.h>
#include <esp32p4.h>

#define P4_NIRQ         P4_CLIC_NIRQ
#define P4_EXT_OFFSET   16      /* first external CLIC line          */
#define CLIC_ATTR_SHV   0x01u   /* clicintattr: hardware vectored    */

typedef struct
{
    volatile uint8_t ip;
    volatile uint8_t ie;
    volatile uint8_t attr;
    volatile uint8_t ct;
} clicint_t;

typedef struct
{
    volatile uint32_t mcliccfg;
    volatile uint32_t clicinfo;
    volatile uint32_t mintthresh;
    clicint_t interrupt[P4_NIRQ] __attribute__((aligned(0x1000)));
} clic_t;

#define CLIC ((volatile clic_t *)P4_CLIC_BASE)
#define INTMTX(n) (*(volatile uint32_t *)(INTERRUPT_CORE0_BASE + 4UL * (n)))

/* Xinu handler table (what drivers/clkinit populate). */
interrupt_handler_t interruptVector[P4_NIRQ];

int resched(void);

/* fallback handler: do nothing (never a NULL jump) */
static void p4_irq_default(void)
{
}

/* Common assembly entry (irqentry.S).  It saves the caller-saved integer
 * and FP registers plus mepc/mstatus, dispatches to
 * interruptVector[mcause & 0xff], and returns with mret.  mepc/mstatus are
 * saved because ctxsw() contexts-switches with mepc + mret and would
 * otherwise leave the handler returning to the wrong PC and privilege. */
extern void p4_irq_entry(void);

/*
 * Hardware vector table (the address loaded in mtvt by _start).
 *
 * When line irq fires, the CLIC reads the 32-bit word at mtvt + 4*irq and
 * jumps there, so each entry is the address of its trampoline.
 *
 * The table MUST live in flash (.rodata): the CLIC fetches it without
 * going through the CPU data-cache coherence path, so a table written at
 * runtime in cached L2MEM (.vectors) reads back as zero and the core
 * jumps to address 0.  Flash is read-only, hence the table is static and
 * drivers register their handlers in interruptVector[] instead.
 */
interrupt_handler_t const InterruptVectorTable[P4_NIRQ]
    __attribute__((section(".rodata.vtable"), aligned(64), used)) =
{
    [0 ... P4_NIRQ - 1] = p4_irq_entry,
};

/*
 * Configure the CLIC: threshold 0, every line hardware-vectored and
 * masked, every Xinu handler set to the safe fallback.
 */
void clic_init(void)
{
    uint32_t i;

    CLIC->mintthresh = 0;

    for (i = 0; i < P4_NIRQ; i++)
    {
        interruptVector[i] = p4_irq_default;
        CLIC->interrupt[i].attr = CLIC_ATTR_SHV;
        CLIC->interrupt[i].ie = 0;
    }
}

/*
 * Enable one interrupt line: identity-route external sources and unmask.
 */
void enable_irq(irqmask irq)
{
    if (irq < P4_NIRQ)
    {
        if (irq >= P4_EXT_OFFSET)
        {
            INTMTX(irq) = irq;
        }
        CLIC->interrupt[irq].attr = CLIC_ATTR_SHV;
        CLIC->interrupt[irq].ie = 1;
    }
}

/*
 * Disable one interrupt line.
 */
void disable_irq(irqmask irq)
{
    if (irq < P4_NIRQ)
    {
        CLIC->interrupt[irq].ie = 0;
    }
}
