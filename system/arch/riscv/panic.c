/**
 * @file panic.c
 */
/* Embedded Xinu, Copyright (C) 2018.  All rights reserved. */
//#include <platform.h>
#include <kernel.h>
#include <riscv.h>

extern __attribute__((noreturn)) void halt(void);

/* Lazos debiles al cargador ELF (system/elfload.c): ver system/arch/riscv/trap.c,
 * que es la otra puerta por la que sale un trap. */
extern const char *elf_quien(uintptr_t) __attribute__((weak));
extern void elf_dump_trap(void) __attribute__((weak));

/* panic message when caught exception that we cannot handle */
void __attribute__((noreturn)) handle_exception(ulong sp)
{
    /* TODO: print all registers
       decode cause codes */
    const char *imagen = NULL;

    kprintf("**** handle_exception  *****\n");
    ulong cause = read_csr(mcause);
    ulong tval = read_csr(mtval);
    ulong mstatus = read_csr(mstatus);
    ulong mepc = read_csr(mepc);
    kprintf("sp = 0x%lx mstatus=0x%lx\n", sp, mstatus);
    kprintf("mcause = 0x%lx mtval=0x%lx\n", cause, tval);
    kprintf("at addr mepc=%lx\n", mepc);
    if (elf_quien != NULL)
    {
        imagen = elf_quien((uintptr_t)mepc);
    }
    kprintf("mepc es %s\n",
            (NULL != imagen) ? imagen : "del kernel o fuera de toda imagen");
    if (elf_dump_trap != NULL)
    {
        elf_dump_trap();
    }
    halt();
}
