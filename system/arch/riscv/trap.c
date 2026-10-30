/**
 * @file trap.c
 *
 * RISC-V machine-mode synchronous exception dispatch for Embedded Xinu.
 *
 * The asm stub in loader/platforms/esp32p4/start.S saves a ::trap_frame_t
 * and calls trap_handler() below.  Handlers are registered per mcause;
 * ecall (mcause 11) is wired to ecall_handler() -> syscall_dispatch().
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <kernel.h>
#include <stddef.h>
#include <stdint.h>
#include <riscv.h>
#include <trap.h>
#include <syscall.h>            /* syscall_dispatch() (system/sysent.c) */

extern __attribute__((noreturn)) void halt(void);

/* One C handler per synchronous cause (NULL = unhandled). */
static trap_handler_t trap_handlers[TRAP_CAUSE_MAX];

int trap_register(uint32_t cause, trap_handler_t handler)
{
    if (cause >= TRAP_CAUSE_MAX)
    {
        return SYSERR;
    }
    trap_handlers[cause] = handler;
    return OK;
}

void trap_handler(trap_frame_t *frame)
{
    uint32_t cause = (uint32_t)read_csr(mcause);

    if (cause < TRAP_CAUSE_MAX)
    {
        /* ecall is a "call", not a fault: resume at the next instruction
         * whether or not a handler consumes it. */
        if (cause == TRAP_CAUSE_ECALL_M)
        {
            frame->mepc += 4;
        }

        if (trap_handlers[cause] != NULL)
        {
            trap_handlers[cause](frame);
            return;
        }

        if (cause == TRAP_CAUSE_ECALL_M)
        {
            return;             /* benign: no gate registered */
        }
    }

    /* Any other exception is a kernel bug: report and stop. */
    kprintf("**** unhandled trap: mcause=0x%lx mepc=0x%lx mtval=0x%lx\n",
            (ulong)cause, (ulong)frame->mepc, (ulong)read_csr(mtval));
    halt();
}

void ecall_handler(trap_frame_t *frame)
{
    frame->a0 = (uint32_t)syscall_dispatch(frame);
}
