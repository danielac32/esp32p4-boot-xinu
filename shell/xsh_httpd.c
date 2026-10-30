/**
 * @file     xsh_httpd.c
 *
 * Shell command (httpd): start/stop/inspect the built-in web server.  The
 * start/stop request is applied by the NET task (see wifi_http_control) so
 * lwIP's non-reentrant raw API is only ever touched from the pump.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <drivers/net/wifi.h>
#include <drivers/net/http.h>

/**
 * @ingroup shell
 *
 * Shell command (httpd [start|stop|status]).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_httpd(int nargs, char *args[])
{
    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s [start|stop|status]\n\n", args[0]);
        printf("Description:\n");
        printf("\tControls the built-in web server.\n");
        printf("Options:\n");
        printf("\tstart\tenable the server\n");
        printf("\tstop\tdisable the server\n");
        printf("\tstatus\tshow whether it is listening (default)\n");
        printf("\t--help\tdisplay this help and exit\n");
        return OK;
    }

    if (nargs == 1 || (nargs == 2 && strcmp(args[1], "status") == 0))
    {
        printf("httpd: %s\n", http_is_running() ? "running" : "stopped");
        return OK;
    }

    if (nargs == 2 && strcmp(args[1], "start") == 0)
    {
        wifi_http_control(TRUE);
        printf("httpd: start requested\n");
        return OK;
    }

    if (nargs == 2 && strcmp(args[1], "stop") == 0)
    {
        wifi_http_control(FALSE);
        printf("httpd: stop requested\n");
        return OK;
    }

    fprintf(stderr, "%s: invalid arguments\n", args[0]);
    fprintf(stderr, "Try '%s --help' for more information\n", args[0]);
    return SYSERR;
}
