/**
 * @file     xsh_telnetserver.c
 *
 */
/* Embedded Xinu, Copyright (C) 2009.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shell.h>
#include <thread.h>
#include <device.h>
#include <telnet.h>
#include <ipv4.h>
#include <network.h>
#include <ether.h>

#if NETHER
static int argErr(char *command, char *arg)
{
    fprintf(stderr, "%s: missing or invalid argument\n", command);
    fprintf(stderr, "Try %s --help for usage\n", command);
    return SHELL_ERROR;
}

/**
 * @ingroup shell
 *
 * Shell command (telnetserver).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return 0 for success, 1 for error
 */
shellcmd xsh_telnetserver(int nargs, char *args[])
{
    int descrp, port, i, spawntelnet, nservers, started;
    tid_typ tid;
    semaphore oldkill;
    struct thrent *thrptr;
    char thrname[TNMLEN];

    bzero(thrname, TNMLEN);
    /* parse arguments to find port number */
    if ((2 == nargs) && (strcmp(args[1], "--help") == 0))
    {
        printf("Usage: %s [-d device] [-p port] [-n count] [-h]\n\n",
               args[0]);
        printf("Description:\n");
        printf
            ("\tSpawns a telnet server, providing remote shell access\n");
        printf("Options:\n");
        printf
            ("\t-d device\tdevice to listen for traffic. (default: ETH0)\n");
        printf
            ("\t-p port\t\tport on which the server listens. (default: 23)\n");
        printf
            ("\t-n count\tnumber of server threads. (default: 1)\n");
        printf("\t-h \t\thalt server.\n");
        printf("\t--help\t\tdisplay this help information and exit\n");
        return 0;
    }

    /* Halt telnet server */
    if ((2 == nargs) && (0 == strcmp(args[1], "-h")))
    {
        oldkill = telnettab[0].killswitch;

        /* Kill the remote shells first, then the server threads.  The
         * shells are created with the name given by this command. */
        for (i = 0; i < NTHREAD; i++)
        {
            thrptr = &thrtab[i];
            if (thrptr->state == THRFREE)
            {
                continue;
            }

            if (0 == strncmp(thrptr->name, "SHELL2", 6))
            {
                kill(i);
            }
        }

        for (i = 0; i < NTHREAD; i++)
        {
            thrptr = &thrtab[i];
            if (thrptr->state == THRFREE)
            {
                continue;
            }

            if ((0 == strncmp(thrptr->name, "telnetServ_", 11)) ||
                (0 == strncmp(thrptr->name, "telnetSvrKill_", 14)))
            {
                kill(i);
            }
        }

        /* Release the TCP sockets held by the servers and then the TELNET
         * devices themselves.  Without this, a restart would leak every
         * tcpAlloc()/telnetAlloc() performed by the previous run. */
        for (i = 0; i < NTELNET; i++)
        {
            int tcpdev = telnettab[i].tcpdev;

            if ((tcpdev >= TCP0) && (tcpdev < TCP0 + NTCP))
            {
                close(tcpdev);
            }
            if (TELNET_STATE_FREE != telnettab[i].state)
            {
                close(TELNET0 + i);
            }
        }

#if NTELNET
        if (SYSERR != (int)oldkill)
        {
            semfree(oldkill);
        }
        oldkill = semcreate(0);
        for (i = 0; i < NTELNET; i++)
        {
            telnettab[i].killswitch = oldkill;
        }
#endif                          /* NTELNET */
        return 0;
    }

    if (nargs > 7)
    {
        fprintf(stderr, "%s: too many arguments\n", args[0]);
        fprintf(stderr, "Try %s --help for usage\n", args[0]);
        return SHELL_ERROR;
    }

    /* assume default device, port and server count */
    /* El device por defecto se resuelve por nombre (getdev sobre el devtab), no
     * con la macro ETH0; mas abajo isbaddev() corta si el conf no lo tiene. */
    descrp = getdev("ETH0");
    port = TELNET_PORT;
    nservers = 1;

    /* set user options if specified */
    for (i = 1; i < nargs; i++)
    {
        if (strcmp(args[i], "-d") == 0)
        {
            i++;
            if (i >= nargs)
                return argErr(args[0], "");
            descrp = getdev(args[i]);
        }
        else if (strcmp(args[i], "-p") == 0)
        {
            i++;
            if (i >= nargs)
                return argErr(args[0], "");
            port = atoi(args[i]);
        }
        else if (strcmp(args[i], "-n") == 0)
        {
            i++;
            if (i >= nargs)
                return argErr(args[0], "");
            nservers = atoi(args[i]);
        }
        else
        {
            return argErr(args[0], args[i]);
        }
    }

    /* verify the device is valid */
    if (isbaddev(descrp))
    {
        fprintf(stderr, "%s: invalid device.\n", args[0]);
        fprintf(stderr, "Try %s --help for usage\n", args[0]);
        return SHELL_ERROR;
    }

    /* verify the provided port is a valid number */
    if (port <= 0)
    {
        fprintf(stderr, "%s: invalid port\n", args[0]);
        fprintf(stderr, "Try %s --help for usage\n", args[0]);
        return SHELL_ERROR;
    }

    /* Clamp the server count.  NTCP is the hard limit: each listening
     * server holds one TCP device. */
    if (nservers < 1)
    {
        nservers = 1;
    }
    if (nservers > NTELNET)
    {
        nservers = NTELNET;
    }
    if (nservers > NTCP)
    {
        nservers = NTCP;
    }

    started = 0;
#if NTELNET
    for (i = 0; i < nservers; i++)
    {
        spawntelnet = telnetAlloc();
        if (SYSERR == spawntelnet)
        {
            break;
        }

        sprintf(thrname, "telnetServ_%d", (spawntelnet - TELNET0));
        TELNET_TRACE("Spawning %s on %d", thrname, spawntelnet - TELNET0);
        tid = create((void *)telnetServer, TELNET_SERVER_STK, INITPRIO, thrname,
                     4, descrp, port, spawntelnet, "SHELL2");
        if (SYSERR == tid)
        {
            /* Give the device back; otherwise the failed create() leaks
             * both the TELNET slot and its stack. */
            close(spawntelnet);
            break;
        }
        ready(tid, RESCHED_YES);
        started++;
    }
#endif

    if (started == 0)
    {
        fprintf(stderr, "%s: cannot create server (out of resources)\n",
                args[0]);
        return SHELL_ERROR;
    }

    if (started < nservers)
    {
        fprintf(stderr,
                "%s: only %d of %d servers could be started\n",
                args[0], started, nservers);
    }

    return SHELL_OK;
}
#endif /* NETHER */
