/**
 * @file trap.h
 *
 * RISC-V machine-mode synchronous exception dispatch.
 *
 * Interrupts never reach here: on the ESP32-P4 they are hardware vectored
 * through the CLIC (mtvt -> InterruptVectorTable).  Only synchronous
 * exceptions (mtvec -> ExceptionVectorTable) land in the stub in
 * loader/platforms/esp32p4/start.S, which builds a ::trap_frame_t and
 * calls trap_handler().  ecall (mcause 11) is the syscall gate and is
 * registered for the C dispatcher in sysent.c.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _RISCV_TRAP_H_
#define _RISCV_TRAP_H_

#include <stdint.h>

/* Synchronous exception causes (mcause, RISC-V privileged spec). */
#define TRAP_CAUSE_INSN_MISALIGNED   0
#define TRAP_CAUSE_INSN_ACCESS       1
#define TRAP_CAUSE_ILLEGAL_INSN      2
#define TRAP_CAUSE_BREAKPOINT        3
#define TRAP_CAUSE_LOAD_MISALIGNED   4
#define TRAP_CAUSE_LOAD_ACCESS       5
#define TRAP_CAUSE_STORE_MISALIGNED  6
#define TRAP_CAUSE_STORE_ACCESS      7
#define TRAP_CAUSE_ECALL_U           8
#define TRAP_CAUSE_ECALL_S           9
#define TRAP_CAUSE_ECALL_M          11
#define TRAP_CAUSE_INSN_PAGE_FAULT  12
#define TRAP_CAUSE_LOAD_PAGE_FAULT  13
#define TRAP_CAUSE_STORE_PAGE_FAULT 15

/* Highest cause the dispatcher indexes; anything else is fatal. */
#define TRAP_CAUSE_MAX              16

/**
 * Register frame saved by the ExceptionVectorTable stub.  The field order
 * MUST match the sw/lw offsets in start.S (48-byte caller-saved block,
 * then mepc/mstatus).  Only caller-saved registers are captured; the C
 * dispatcher preserves everything else.
 */
typedef struct
{
    uint32_t ra;
    uint32_t t0;
    uint32_t t1;
    uint32_t t2;
    uint32_t t3;
    uint32_t t4;
    uint32_t t5;
    uint32_t t6;
    uint32_t a0;
    uint32_t a1;
    uint32_t a2;
    uint32_t a3;
    uint32_t a4;
    uint32_t a5;
    uint32_t a6;
    uint32_t a7;
    uint32_t mepc;
    uint32_t mstatus;
} trap_frame_t;

typedef void (*trap_handler_t)(trap_frame_t *frame);

/**
 * Register (or clear, with NULL) the handler for a synchronous cause.
 * @return OK on success, SYSERR if the cause is out of range.
 */
int trap_register(uint32_t cause, trap_handler_t handler);

/**
 * C entry point called from the asm stub with the saved frame.
 * Dispatches by mcause; unknown causes are reported and halt the kernel.
 */
void trap_handler(trap_frame_t *frame);

/**
 * ecall gate (mcause 11).  ABI: a7 = syscall number, a0-a5 = arguments,
 * a0 = return value.  Delegates to syscall_dispatch() (system/sysent.c).
 */
void ecall_handler(trap_frame_t *frame);

#endif /* _RISCV_TRAP_H_ */
