/*
 * xsh_elf.c — Shell command: load and run an ELF32 (RISC-V PIE) from the SD.
 *
 *   elf <path> [args...]
 *
 * Uses system/elfload.c (GOT relocation + .init_array) and runs the entry
 * point in a task with INITSTK.  The app calls the kernel via ecall/syscall.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <thread.h>
#include <device.h>
#include <memory.h>
#include <elfload.h>
#include <fsutil.h>

#define ELF_STK  INITSTK

shellcmd xsh_elf(int nargs, char *args[])
{
    void *entry;
    uint32_t base = 0, size = 0;
    char path[FS_PATH_MAX];
    tid_typ child;

    if (nargs < 2) {
        printf("Usage: %s <path.elf> [args...]\n", args[0]);
        return SYSERR;
    }

    if (!fs_resolve(args[1], path, sizeof(path))) {
        fprintf(stderr, "elf: path too long\n");
        return SYSERR;
    }

    entry = elf_load(path, &base, &size);
    if (entry == 0) {
        fprintf(stderr, "elf: no se pudo cargar %s\n", path);
        return SYSERR;
    }

    printf("elf: cargado %s @ %p (base=%p, size=%lu)\n",
           path, entry, (void *)base, (unsigned long)size);

    /* Pasar argumentos adicionales al programa ELF (args[2..nargs-1]) */
    int elf_argc = nargs - 2;
    char **elf_argv = (elf_argc > 0) ? &args[2] : NULL;

    /* Lanza el entry point como tarea con sus argumentos */
    child = create((void (*)(void))entry, ELF_STK, INITPRIO, "ELF", 3,
                   entry, elf_argc, elf_argv);
    if (SYSERR == child) {
        printf("elf: no se pudo crear la tarea\n");
        /* Liberar el bloque ELF ya que no se pudo crear la tarea */
        psram_freemem((void *)base, size);
        return SYSERR;
    }

    /* Marcar la tarea como ELF para liberar el bloque PSRAM al salir */
    thrtab[child].elf_task = TRUE;
    thrtab[child].elf_img = (void *)base;
    thrtab[child].elf_size = size;

    while (recvclr() != NOMSG);
    ready(child, RESCHED_YES);

    /* Espera a que termine (exit syscall envia el tid al padre). */
    while (receive() != child);

    printf("elf: terminado\n");
    return OK;
}
