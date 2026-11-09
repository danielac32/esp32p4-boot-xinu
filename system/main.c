/**
 * @file     main.c
 */
/* Embedded Xinu, Copyright (C) 2009, 2013.  All rights reserved. */

#include <device.h>
#include <net.h>
#include <platform.h>
#include <shell.h>
#include <stdio.h>
#include <thread.h>
#include <version.h>
#include <string.h>
#include <stdlib.h>
#include <elfload.h>
static void print_os_info(void);
static thread launcherTask(void);
extern int bootTask(void);
extern int netBootTask(void);

/* El launcher del arranque es un ELF de la microSD, no codigo del kernel: aqui
 * solo esta la hebra que lo carga.  Necesita hebra propia porque elf_run()
 * ejecuta el app en la pila de quien lo llama y el exit() del app es
 * kill(gettid()): corriendo en main(), el SALIR del launcher se llevaria esta
 * hebra.  32 KB de pila: el marco de elf_run() es de unos cientos de bytes, pero
 * debajo corre la app entera (gestos + estado + creacion de objetos LVGL). */
#define LAUNCHER_ELF "/apps/launcher/app.elf"
#define LAUNCHER_STK 32768

/**
 * Crea una hebra que TERMINA y espera su tid.  El join es receive(): al devolver,
 * userret()->kill() hace send(parent, tid), y el padre de esta hebra es main().
 * Solo vale para tareas con fin (bootTask, netBootTask); el launcher es un app
 * con bucle infinito y esperarlo seria colgarse aqui para siempre.
 */
static void lanzar_y_esperar(void *proc, uint pila, const char *nombre)
{
    tid_typ tid = create(proc, pila, INITPRIO, nombre, 0);

    if (SYSERR == (int)tid)
    {
        kprintf("WARNING: no se pudo crear %s\r\n", nombre);
        return;
    }
    ready(tid, RESCHED_YES);
    while (receive() != (int)tid)
    {
        ;
    }
}

thread main(void)
{
    if (SYSERR == open(CONSOLE, SERIAL0))
    {
        kprintf("WARNING: Can't open CONSOLE over SERIAL0\r\n");
        return 0;
    }

    /* 1. El splash.  bootTask() pinta el banner de arranque en el panel y borra
     * SOLO su propio panel, sin tocar lv_scr_act().  Se le espera porque el
     * launcher dibuja en la misma pantalla: si arrancara a la vez, los dos
     * arboles de objetos se pisarian.  16 KB de pila: crea etiquetas LVGL y usa
     * un sprintf con un buffer de 80 bytes, y la hebra muere al terminar. */
    lanzar_y_esperar(bootTask, 16384, "boot");

    /* 2. El launcher, en hebra propia y SIN esperarlo.  elf_run() ejecuta el app
     * sobre la pila de quien lo llama, asi que necesita una hebra que pueda
     * morir con el: el exit() del app es kill(gettid()) y kill() suelta el bloque
     * de PSRAM.  RESCHED_NO para que main() siga colocando el arranque. */
    if (SYSERR == ready(create(launcherTask, LAUNCHER_STK, INITPRIO,
                               "launcher", 0), RESCHED_NO))
    {
        kprintf("WARNING: Failed to create launcher\r\n");
    }

    /* 3. La red, con gate.  netBootTask() duerme 10 s de margen para el C6 antes
     * de tocar nada: para entonces el launcher ya termino de leer sus app.txt de
     * la microSD, asi que el FAT y el SDIO del C6 no se mezclan en el mismo
     * bloque SDMMC.  Si la WiFi no asocia, agota sus 5 reintentos y devuelve
     * SYSERR: main() sigue igual y la shell sale, no se queda esperando. */
    lanzar_y_esperar(netBootTask, 16384, "netboot");

    print_os_info();

    /* 4. La consola, al final: el launcher ya esta en pantalla y la red arriba.
     * RESCHED_NO para que main() devuelva y la shell tome el procesador cuando
     * el planificador la elija. */
    if (SYSERR == ready(create(shell, INITSTK, INITPRIO, "SHELL", 3,
                               CONSOLE, CONSOLE, CONSOLE), RESCHED_NO))
    {
        kprintf("WARNING: Failed to create SHELL\r\n");
    }

    return 0;
}

/**
 * Carga el launcher de la microSD y se va con el.  elf_run() ya informa de cada
 * fallo (sin volumen FAT, ELF que no se abre, simbolo sin exportar), asi que
 * aqui no se repite el diagnostico ni se reintenta en bucle: un reintento
 * escupiria las mismas lineas por la consola para siempre.
 *
 * Al pulsar SALIR el app llama a exit(0), que es kill(gettid()) sobre ESTA
 * hebra: kill() suelta el bloque de PSRAM del app y la hebra desaparece.  No hay
 * quien la relance; volver al launcher es `elf /apps/launcher/app.elf'.
 */
static thread launcherTask(void)
{
    kprintf("[launcher] elf_run %s\r\n", LAUNCHER_ELF);
    if (SYSERR == elf_run(LAUNCHER_ELF, 0, NULL))
    {
        kprintf("[launcher] fallo al cargar %s\r\n", LAUNCHER_ELF);
        return SYSERR;
    }
    return OK;
}

 
#ifdef DETAIL
extern ulong cpuid;
#endif

#if __LP64__
#define HEXWIDTH 16
#define DECWIDTH 20
#else
#define HEXWIDTH 8
#define DECWIDTH 10
#endif

/* Start of kernel in memory (provided by linker)  */
extern void _start(void);

static void print_os_info(void)
{
    kprintf(VERSION);
    kprintf("\r\n\r\n");

#ifdef DETAIL
    /* Output detected platform. */
    kprintf("Processor identification: 0x%lX\r\n", cpuid);
    kprintf("Detected platform as: %s, %s\r\n\r\n",
            platform.family, platform.name);
#endif

    /* Output Xinu memory layout */
    kprintf("%*ld bytes physical memory.\r\n",
        DECWIDTH, (uintptr_t)platform.maxaddr - (uintptr_t)platform.minaddr);
#ifdef DETAIL
    kprintf("           [0x%0*X to 0x%0*X]\r\n",
        HEXWIDTH, (uintptr_t)platform.minaddr,
        HEXWIDTH, (uintptr_t)(platform.maxaddr - 1));
#endif

 

    kprintf("%*ld bytes Xinu code.\r\n", DECWIDTH, (uintptr_t)&_etext - (uintptr_t)_start);
#ifdef DETAIL
    kprintf("           [0x%0*lX to 0x%0*lX]\r\n",
        HEXWIDTH, (uintptr_t)_start,
        HEXWIDTH, (uintptr_t)&_end - 1);
#endif

    kprintf("%*ld bytes stack space.\r\n",
        DECWIDTH, (uintptr_t)memheap - (uintptr_t)&_end);
#ifdef DETAIL
    kprintf("           [0x%0*lX to 0x%0*lX]\r\n",
        HEXWIDTH, (uintptr_t)&_end,
        HEXWIDTH, (uintptr_t)memheap - 1);
#endif

    kprintf("%*ld bytes heap space.\r\n",
            DECWIDTH, (uintptr_t)platform.maxaddr - (uintptr_t)memheap);
#ifdef DETAIL
    kprintf("           [0x%0*lX to 0x%0*lX]\r\n\r\n",
        HEXWIDTH, (uintptr_t)memheap,
        HEXWIDTH, (uintptr_t)platform.maxaddr - 1);
#endif
    kprintf("\r\n");
}
