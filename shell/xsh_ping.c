/**
 * @file     xsh_ping.c
 *
 * Shell command (ping): send ICMP echo requests to a host.
 *
 * El trabajo real lo hace el NET task (wifi_ping_service); este comando sólo
 * deja la petición y va imprimiendo los resultados según llegan.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <kernel.h>
#include <thread.h>
#include <drivers/net/wifi.h>

#define PING_DATA_BYTES  32
#define PING_DEFAULT_CNT 4

/* Acumula e imprime los resultados aún no vistos de reps[0..n-1]. */
static void ping_report(const char *host, struct wifi_ping_rep *reps, int n,
                        int *printed, int *recv, long *rmin, long *rmax,
                        unsigned long *rsum)
{
    int j;

    for (j = *printed; j < n; j++)
    {
        if (reps[j].rtt_ms < 0)
        {
            printf("timeout: seq=%d\n", reps[j].seq);
        }
        else
        {
            printf("64 bytes from %s: seq=%d time=%ld ms\n",
                   host, reps[j].seq, reps[j].rtt_ms);
            (*recv)++;
            if (*rmin < 0 || reps[j].rtt_ms < *rmin)
                *rmin = reps[j].rtt_ms;
            if (*rmax < 0 || reps[j].rtt_ms > *rmax)
                *rmax = reps[j].rtt_ms;
            *rsum += (unsigned long)reps[j].rtt_ms;
        }
    }
    if (n > *printed)
        *printed = n;
}

static void ping_usage(const char *cmd)
{
    printf("Usage: %s [-c COUNT] HOST\n\n", cmd);
    printf("Description:\n");
    printf("\tSends ICMP echo requests to HOST and prints the replies.\n");
    printf("\tHOST may be a dotted-quad IPv4 address or a host name.\n");
    printf("Options:\n");
    printf("\t-c COUNT\tnumber of packets to send (default %d)\n",
           PING_DEFAULT_CNT);
    printf("\t--help\t\tdisplay this help and exit\n");
}

/**
 * @ingroup shell
 *
 * Shell command (ping [-c COUNT] HOST).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_ping(int nargs, char *args[])
{
    int count = PING_DEFAULT_CNT;
    const char *host = NULL;
    int printed = 0;
    int recv = 0;
    long rmin = -1;
    long rmax = -1;
    unsigned long rsum = 0;
    int i;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        ping_usage(args[0]);
        return OK;
    }

    for (i = 1; i < nargs; i++)
    {
        if (strcmp(args[i], "-c") == 0)
        {
            if (i + 1 >= nargs)
            {
                fprintf(stderr, "%s: '-c' needs a count\n", args[0]);
                return SYSERR;
            }
            count = atoi(args[++i]);
        }
        else if (strncmp(args[i], "-c", 2) == 0 && args[i][2] != '\0')
        {
            count = atoi(args[i] + 2);
        }
        else if (args[i][0] == '-' && args[i][1] != '\0')
        {
            fprintf(stderr, "%s: unknown option '%s'\n", args[0], args[i]);
            ping_usage(args[0]);
            return SYSERR;
        }
        else if (host == NULL)
        {
            host = args[i];
        }
        else
        {
            fprintf(stderr, "%s: only one host allowed\n", args[0]);
            return SYSERR;
        }
    }

    if (host == NULL)
    {
        fprintf(stderr, "%s: missing HOST\n", args[0]);
        ping_usage(args[0]);
        return SYSERR;
    }

    if (count < 1)
        count = 1;
    if (count > WIFI_PING_MAX)
        count = WIFI_PING_MAX;

    /* El servicio de ping corre en el NET task; si el enlace aún no está
     * levantado (o el C6 no responde) el NET task no bombea y el ping se
     * quedaría esperando.  Mejor avisar. */
    if (!wifi_is_link_up())
    {
        fprintf(stderr, "%s: network is down (no WiFi link yet)\n", args[0]);
        return SYSERR;
    }

    if (wifi_ping_start(host, count) != OK)
    {
        fprintf(stderr, "%s: another ping is already running\n", args[0]);
        return SYSERR;
    }

    printf("PING %s: %d data bytes\n", host, PING_DATA_BYTES + 8);

    for (;;)
    {
        struct wifi_ping_rep reps[WIFI_PING_MAX];
        int n = wifi_ping_poll(reps, WIFI_PING_MAX);

        ping_report(host, reps, n, &printed, &recv, &rmin, &rmax, &rsum);

        if (wifi_ping_done())
        {
            /* Última pasada para drenar lo que quedara. */
            n = wifi_ping_poll(reps, WIFI_PING_MAX);
            ping_report(host, reps, n, &printed, &recv, &rmin, &rmax, &rsum);
            break;
        }

        sleep(20);           /* cede la CPU: el NET task hace el ping */
    }

    if (printed == 0)
    {
        fprintf(stderr, "%s: unknown host %s\n", args[0], host);
        return SYSERR;
    }

    printf("--- %s ping statistics ---\n", host);
    printf("%d packets transmitted, %d received, %d%% packet loss\n",
           count, recv, count ? (100 * (count - recv) / count) : 0);
    if (recv > 0)
    {
        printf("rtt min/avg/max = %ld/%lu/%ld ms\n",
               rmin, rsum / (unsigned long)recv, rmax);
    }

    return OK;
}
