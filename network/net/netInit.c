/* @file netInit.c
 *
 */
/* Embedded Xinu, Copyright (C) 2009.  All rights reserved. */

#include <stddef.h>
#include <arp.h>
#include <icmp.h>
#include <bufpool.h>
#include <network.h>
#include <route.h>
#include <stdlib.h>
#include <stdio.h>
#include <tcp.h>

#ifndef NNETIF
#define NNETIF 0
#endif

struct netif netiftab[NNETIF];
int netpool;

/**
 * @ingroup network
 *
 * Initialize network interfaces
 * @return OK if initialized properly, otherwise SYSERR
 */
syscall netInit()
{
    int i = 0;

    /* Clear each network interface */
    for (i = 0; i < NNETIF; i++)
    {
        bzero(&netiftab[i], sizeof(struct netif));
        netiftab[i].state = NET_FREE;
    }

    /* Allocate packet buffer pool */
    netpool = bfpalloc(NET_MAX_PKTLEN + sizeof(struct packet),
                       NET_POOLSIZE);
    NET_TRACE("netpool has been assigned pool ID %d.\r\n", netpool);
    if (SYSERR == netpool)
    {
        return SYSERR;
    }

    /* Initialize ARP */
    if (SYSERR == arpInit())
    {
        return SYSERR;
    }

    if (SYSERR == rtInit())
    {
        return SYSERR;
    }

    /* Initialize ICMP */
    if (SYSERR == icmpInit())
    {
        return SYSERR;
    }

    /* Initialize TCP */
#if NTCP
    /*
     * tcpTimer naciera con INITSTK (65536).  `ps' midio 212 B de pico, pero la
     * hebra tambien recorre la cadena de salida (tcpTick -> tcpSend -> ipOutput
     * -> espethWrite -> SDIO), que no se ejercita con el TCP en reposo, y el
     * heap del P4 esta en L2MEM y es lo que sostiene a ls, cat y al httpd.
     * Con 8192 queda 4 KB por encima del suelo de MINSTK y se devuelven 57 KB.
     */
    i = create((void *)tcpTimer, TCP_TIMER_STK, INITPRIO, "tcpTimer", 0);
    if (SYSERR == i)
    {
        return SYSERR;
    }
    else
    {
        ready(i, RESCHED_NO);
    }
#endif                          /* NTCP */

    return OK;
}
