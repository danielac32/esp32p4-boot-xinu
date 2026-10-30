/**
 * @file     xsh_wifi.c
 *
 * Shell command (wifi): show the WiFi (ESP32-C6) link state and addressing,
 * or set new STA credentials and reconnect.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <drivers/net/wifi.h>

static void wifi_usage(const char *cmd)
{
    printf("Usage: %s [status]\n", cmd);
    printf("       %s set <ssid> <password>\n\n", cmd);
    printf("Description:\n");
    printf("\tWithout arguments (or with 'status') shows the WiFi (ESP32-C6)\n");
    printf("\tlink state and addressing.\n");
    printf("\t'set' stores new STA credentials in /wifi.conf (microSD) and\n");
    printf("\treconnects.\n");
    printf("Options:\n");
    printf("\t--help\tdisplay this help and exit\n");
}

static void wifi_status(void)
{
    char ip[20];
    char mask[20];
    char gw[20];
    char mac[20];

    wifi_get_ip_str(ip, sizeof(ip));
    wifi_get_netmask_str(mask, sizeof(mask));
    wifi_get_gw_str(gw, sizeof(gw));
    wifi_get_mac_str(mac, sizeof(mac));

    printf("SSID    : %s\n", wifi_ssid());
    printf("link    : %s\n", wifi_is_link_up() ? "up" : "down");
    printf("MAC     : %s\n", mac);
    printf("IPv4    : %s\n", ip);
    printf("netmask : %s\n", mask);
    printf("gateway : %s\n", gw);
    printf("DHCP    : %s\n", wifi_has_ip() ? "bound" : "pending");
}

/**
 * @ingroup shell
 *
 * Shell command (wifi [status] | wifi set <ssid> <password>).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_wifi(int nargs, char *args[])
{
    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        wifi_usage(args[0]);
        return OK;
    }

    /* wifi set <ssid> <password> */
    if (nargs >= 2 && strcmp(args[1], "set") == 0)
    {
        int rc;

        if (nargs != 4)
        {
            fprintf(stderr, "%s: 'set' needs <ssid> <password>\n", args[0]);
            wifi_usage(args[0]);
            return SYSERR;
        }

        rc = wifi_set_credentials(args[2], args[3]);
        if (rc == OK)
            printf("wifi: credenciales guardadas en /wifi.conf\n");
        else
            printf("wifi: credenciales aplicadas, pero no se pudieron guardar"
                   " (¿FS montado?)\n");
        printf("wifi: reconectando a \"%s\"...\n", args[2]);
        return OK;
    }

    if (nargs > 2 || (nargs == 2 && strcmp(args[1], "status") != 0))
    {
        fprintf(stderr, "%s: invalid arguments\n", args[0]);
        wifi_usage(args[0]);
        return SYSERR;
    }

    wifi_status();
    return OK;
}
