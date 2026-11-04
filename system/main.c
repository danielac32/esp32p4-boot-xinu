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
static void print_os_info(void);

 
/**
 * Main thread.  It opens the console and starts the shell.
 */

extern int bootTask(int dev);
extern int dashTask(void);
extern int p4demoTask(void);
extern int netBootTask(void);

thread main(void)
{
    tid_typ nettid = (tid_typ)SYSERR;
    uchar netdead = FALSE;
    uchar childdead = FALSE;

    /* Print information about the operating system  */
    print_os_info();
 
   // ready(create(uart_probe, INITSTK, 10, "uart_probe", 0), RESCHED_NO);

    if (SYSERR == open(CONSOLE, SERIAL0))
    {
        kprintf("WARNING: Can't open CONSOLE over SERIAL0\r\n");
        return 0;
    }
    
    

    /* Arranque de red: C6 arriba, asociacion a la AP, ETH0 por DHCP y los
     * servidores (telnet, y despues httpd/udpserver).  En xinu-s3-amp esta
     * secuencia va en linea en main(), pero aqui necesita hebra propia porque
     * el `wifi join' tarda varios segundos y main tiene que seguir con el
     * panel.  El margen que necesita el C6 al arrancar lo pone la propia tarea,
     * que espera y reintenta (NETBOOT_SETTLE_MS, shell/xsh_netboot.c:32). */
    {
        /* Pila de 16 KB, no INITSTK (64 KB): el heap de L2MEM del P4 son ~432
         * KB y las hebras ya vivas dejan poco sitio, asi que una hebra de
         * 64 KB se llevaba todo y la shell no podia ni crear un comando.  El
         * pico medido de esta hebra es ~900 B. */
        nettid = create(netBootTask, 16384, INITPRIO, "netboot", 0);

        if (SYSERR == nettid)
        {
            kprintf("WARNING: Failed to create netboot\r\n");
        }
        else
        {
            ready(nettid, RESCHED_NO);
        }
    }

    /* La tarea de arranque pinta el booteo y, al volver de su funcion,
     * userret()->kill()->send(parent) avisa a esta hebra.  dashTask espera ese
     * aviso para no pisar el banner mientras bootTask aun dibuja.  */
    ready(create(bootTask, INITSTK, INITPRIO, "bootlcd", 0), RESCHED_YES);
    sleep(500);
    int child = create(p4demoTask, 8192, INITPRIO, "dash", 0);
    irqmask im;
    while (recvclr() != NOMSG);
    im = disable();
    ready(child, RESCHED_YES);
    restore(im);

    /* Red antes que consola: la shell no nace hasta que la hebra netboot muera
     * (netBootTask() vuelve y userret()->kill()->send(parent) le entrega su tid
     * a esta hebra), asi que el `xsh$' aparece despues del resumen de netboot y
     * no encima.  El mensaje de bootTask puede llegar antes y receive() consume
     * la bandeja: por eso se anota (childdead) en lugar de dejarlo pasar, si no
     * el bucle del panel no lo volveria a ver y el dash nunca nace. */
    if (SYSERR != (int)nettid)
    {
        while (netdead == FALSE)
        {
            int msg = receive();

            if (msg == (int)nettid)
            {
                netdead = TRUE;
            }
            else if (msg == child)
            {
                childdead = TRUE;
            }
        }
    }

    if (SYSERR == ready(create(shell, INITSTK, INITPRIO, "SHELL", 3,
                               CONSOLE, CONSOLE, CONSOLE), RESCHED_NO))
    {
        kprintf("WARNING: Failed to create SHELL\r\n");
    }

    /* Wait for boot thread to finish, then hand the panel to the dashboard. */
    while (childdead == FALSE && receive() != child);
    sleep(500);

    /* 8 KB, no el suelo de 4 KB que da MINSTK: el panel pregunta el RSSI por
     * su cuenta y ese RPC bomba la cola del transporte, cuyo
     * esp_hosted_dispatch_rx() gasta 1664 B de frame (medido con
     * -fstack-usage) mas la demux de la trama que llegue.  Su propio frame
     * son 352 B. */
    
    //ready(create(dashTask, INITSTK, INITPRIO, "dash", 0), RESCHED_YES);
    return 0;
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
