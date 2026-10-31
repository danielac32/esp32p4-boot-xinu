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
thread main(void)
{
    /* Print information about the operating system  */
    print_os_info();
 
   // ready(create(uart_probe, INITSTK, 10, "uart_probe", 0), RESCHED_NO);

    if (SYSERR == open(CONSOLE, SERIAL0))
    {
        kprintf("WARNING: Can't open CONSOLE over SERIAL0\r\n");
        return 0;
    }
    
    if (SYSERR == ready(create(shell, INITSTK, INITPRIO, "SHELL", 3,
                               CONSOLE, CONSOLE, CONSOLE), RESCHED_NO))
    {
        kprintf("WARNING: Failed to create SHELL\r\n");
    }
    
    ready(create(bootTask, INITSTK, INITPRIO, "bootlcd", 0), RESCHED_NO);
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
