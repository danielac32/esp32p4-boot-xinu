/**
 * @file xsh_elf.c
 *
 * 'elf': el unico comando de apps ELF del kernel.
 *
 *   elf ARCHIVO.elf [arg...]
 *
 * El comando busca el ELF en la microSD, y el cargador (system/elfload.c) lo
 * coloca en PSRAM, lo enlaza contra system/exports.c, le parchea las
 * relocaciones y salta a su '_start' con argc/argv.  El app corre en la hebra
 * de este comando; su 'exit' es kill(gettid()) y es kill() del kernel quien
 * suelta el bloque de PSRAM, asi que al morir la hebra del comando el prompt
 * vuelve y la PSRAM queda libre.  Si el app devolviera sin exit() --un _start
 * propio--, elf_run() suelta el bloque y aqui se informa.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <fsutil.h>
#include <elfload.h>

shellcmd xsh_elf(int nargs, char *args[])
{
    char ruta[FS_PATH_MAX];

    if (nargs < 2 || strcmp(args[1], "--help") == 0)
    {
        printf("Usage: elf ARCHIVO.elf [arg...]\r\n\r\n");
        printf("Description:\r\n");
        printf("\tCarga un ELF RV32 (ET_REL, 'ld -r') de la microSD, aplica los\r\n");
        printf("\texports del kernel y ejecuta su '_start' en la hebra de este\r\n");
        printf("\tcomando.  El 'exit' del app es kill(gettid()): la hebra muere\r\n");
        printf("\ty el kernel suelta su bloque de PSRAM.\r\n");
        printf("\tLos argumentos despues de la ruta son del app (argv[1..]).\r\n");
        return OK;
    }

    if (!fs_mounted())
    {
        fprintf(stderr, "elf: no hay volumen FAT montado; usa `sd'\r\n");
        return SYSERR;
    }

    if (!fs_resolve(args[1], ruta, sizeof(ruta)))
    {
        fprintf(stderr, "elf: ruta demasiado larga\r\n");
        return SYSERR;
    }

    if (fl_is_dir(ruta))
    {
        fprintf(stderr, "elf: %s: es un directorio\r\n", ruta);
        return SYSERR;
    }

    if (SYSERR == elf_run(ruta, nargs - 2, &args[2]))
    {
        return SYSERR;
    }
    printf("elf: %s volvio sin exit(); su PSRAM quedo libre\r\n", ruta);
    return OK;
}
