/**
 * @file sysent.c
 *
 * Kernel-side syscall table for the ecall gate (system/arch/riscv/trap.c,
 * ecall_handler -> syscall_dispatch).  Numbers come from include/syscall.h
 * and MUST stay in sync with the XINU_* ABI.
 *
 * A syscall entry receives a0-a5 and returns the value placed in a0.
 * Entries left NULL return SYSERR.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <thread.h>
#include <syscall.h>
#include <trap.h>
#include <device.h>
#include <memory.h>
#include <stdio.h>
#include <string.h>

#include <clock.h>
#include <timer.h>

typedef int (*sysent_t)(int, int, int, int, int, int);

 
int syscall_dispatch(trap_frame_t *frame)
{
    uint32_t nr = frame->a7;

   /* if (nr >= (uint32_t)NSYSCALL || sys_call_table[nr] == NULL)
    {
        return SYSERR;
    }*/

    return 0;//sys_call_table[nr]((int)frame->a0, (int)frame->a1, (int)frame->a2,
                             // (int)frame->a3, (int)frame->a4, (int)frame->a5);
}