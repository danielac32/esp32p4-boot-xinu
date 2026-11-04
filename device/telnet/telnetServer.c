/**
 * @file telnetServer.c
 *
 */
/* Embedded Xinu, Copyright (C) 2009.  All rights reserved. */

#include <stdio.h>
#include <stddef.h>
#include <device.h>
#include <tcp.h>
#include <shell.h>
#include <thread.h>
#include <telnet.h>

/**
 * @ingroup telnet
 *
 * Start telnet server
 * @param ethdev  interface on which telnet server will listen
 * @param port  port on which to start the server
 * @param telnetdev  telnet device to use for connection
 * @param shellname     shell device to use for connection
 * @return      OK on success SYSERR on failure
 */
thread telnetServer(int ethdev, int port, ushort telnetdev,
                    char *shellname)
{
    tid_typ tid;
    ushort tcpdev;
    struct netif *interface;
    struct netaddr *host;
    int minor;
    uchar buf[6];
    TELNET_TRACE("ethdev %d, port %d, telnet %d", ethdev, port,
                 telnetdev);
    /* find netaddr of ethernet device */
    interface = netLookup(ethdev);
    if (NULL == interface)
    {
        close(telnetdev);
        return SYSERR;
    }
    host = &(interface->ip);
    minor = devtab[telnetdev].minor;

    while (TRUE)
    {
        tcpdev = tcpAllocPassive();
        if (SYSERR == (short)tcpdev)
        {
            /* All TCP devices are busy.  A previous connection may still
             * be in TIME_WAIT, so keep this server alive and retry rather
             * than exiting for good. */
            sleep(100);
            continue;
        }

        /* Remember the TCP device in the TELNET entry.  "telnetserver -h"
         * can then close it even while this thread is blocked in open()
         * waiting for a client. */
        telnettab[minor].tcpdev = tcpdev;

        if (open(tcpdev, host, NULL, port, NULL, TCP_PASSIVE) < 0)
        {
            close(tcpdev);
            close(telnetdev);
            fprintf(stderr,
                    "telnet server failed to open TCP socket %d\n",
                    tcpdev);
            return SYSERR;
        }

        if (SYSERR == open(telnetdev, tcpdev))
        {
            close(tcpdev);
            close(telnetdev);
            fprintf(stderr,
                    "telnet server failed to open TELNET device %d\n",
                    telnetdev);
            return SYSERR;
        }
#ifdef TELNET0
        TELNET_TRACE("telnetServer() opened telnet %d on TCP%d\n",
                     telnetdev - TELNET0, tcpdev - TCP0);
#endif

        /* Request these options to the client */
        buf[0] = TELNET_IAC;
        buf[1] = TELNET_WILL;
        buf[2] = TELNET_ECHO;
        buf[3] = TELNET_IAC;
        buf[4] = TELNET_DO;
        buf[5] = TELNET_SUPPRESS_GA;
        write(tcpdev, (void *)buf, 6);

        TELNET_TRACE
            ("telnetServer() sending WILL ECHO and Suppress GA\n");

        // open shell on TELNET device
        tid = create((void *)shell, TELNET_SHELL_STK, INITPRIO, shellname, 3,
                     telnetdev, telnetdev, telnetdev);
        if (SYSERR == tid)
        {
            close(tcpdev);
            close(telnetdev);
            return SYSERR;
        }
        /* Clear any pending messages */
        recvclr();
        TELNET_TRACE("telnetServer() spawning shell thread %d\n", tid);
        ready(tid, RESCHED_YES);

        // loop until child process dies
        while (recvclr() != tid)
        {
            sleep(200);
            control(telnetdev, TELNET_CTRL_FLUSH, 0, 0);
        }

        if (SYSERR == close(tcpdev))
        {
            /* The peer may have closed first; tcpClose() then reports
             * SYSERR but the slot is still reclaimed.  Keep serving. */
            TELNET_TRACE("tcpClose returned SYSERR");
        }
        close(telnetdev);
    }

    return SYSERR;
}
