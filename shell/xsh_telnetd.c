/**
 * @file     xsh_telnetd.c
 *
 * Shell command (telnetd): controla el servidor telnet.  Un cliente = un hilo
 * shell sobre un device TELNETn (su "descriptor").  La petición de arranque/
 * parada la aplica el NET task (la raw API de lwIP no es reentrante).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <kernel.h>
#include <shell.h>
#include <telnet.h>

static void telnetd_usage(const char *cmd)
{
    printf("Usage: %s [start|stop|status] [-p PORT]\n\n", cmd);
    printf("Description:\n");
    printf("\tControls the telnet server.  Each connected client gets its\n");
    printf("\town shell on a per-client TELNETn device.\n");
    printf("Options:\n");
    printf("\tstart\tstart listening\n");
    printf("\tstop\tstop listening (open sessions stay)\n");
    printf("\tstatus\tshow server state (default)\n");
    printf("\t-p PORT\tlistening port (default %d)\n", TELNET_PORT);
    printf("\t--help\tdisplay this help and exit\n");
}

/**
 * @ingroup shell
 *
 * Shell command (telnetd [start|stop|status] [-p PORT]).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_telnetd(int nargs, char *args[])
{
    const char *action = NULL;
    int port = 0;
    int i;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        telnetd_usage(args[0]);
        return OK;
    }

    for (i = 1; i < nargs; i++)
    {
        if (strcmp(args[i], "-p") == 0)
        {
            if (i + 1 >= nargs)
            {
                fprintf(stderr, "%s: -p needs a port\n", args[0]);
                return SYSERR;
            }
            port = atoi(args[++i]);
            if (port <= 0 || port > 65535)
            {
                fprintf(stderr, "%s: invalid port '%s'\n", args[0], args[i]);
                return SYSERR;
            }
        }
        else if (strcmp(args[i], "start") == 0 ||
                 strcmp(args[i], "stop") == 0 ||
                 strcmp(args[i], "status") == 0)
        {
            if (action != NULL)
            {
                fprintf(stderr, "%s: too many actions\n", args[0]);
                return SYSERR;
            }
            action = args[i];
        }
        else if (args[i][0] == '-' && args[i][1] != '\0')
        {
            fprintf(stderr, "%s: unknown option '%s'\n", args[0], args[i]);
            telnetd_usage(args[0]);
            return SYSERR;
        }
        else
        {
            fprintf(stderr, "%s: invalid argument '%s'\n", args[0], args[i]);
            telnetd_usage(args[0]);
            return SYSERR;
        }
    }

    if (action == NULL)
        action = (port > 0) ? "start" : "status";

    if (strcmp(action, "start") == 0)
    {
        if (port > 0)
            telnet_server_set_port(port);
        telnet_server_request(TRUE);
        printf("telnetd: start requested\n");
        return OK;
    }

    if (strcmp(action, "stop") == 0)
    {
        telnet_server_request(FALSE);
        printf("telnetd: stop requested\n");
        return OK;
    }

    /* status */
    printf("telnetd: %s, %d/%d client(s)\n",
           telnet_server_running() ? "running" : "stopped",
           telnet_server_clients(), NTELNET);
    return OK;
}
