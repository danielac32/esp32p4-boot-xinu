/**
 * @file     xsh_ip.c
 *
 * Shell command (ip / ifconfig): show the network interface configuration.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <drivers/net/wifi.h>

/**
 * @ingroup shell
 *
 * Shell command (ip / ifconfig).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_ip(int nargs, char *args[])
{
    char ip[20];
    char mask[20];
    char gw[20];
    char mac[20];

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s\n\n", args[0]);
        printf("Description:\n");
        printf("\tShows the (single) network interface: MAC, IPv4,\n");
        printf("\tnetmask, gateway and link state.\n");
        printf("Options:\n");
        printf("\t--help\tdisplay this help and exit\n");
        return OK;
    }

    if (nargs > 1)
    {
        fprintf(stderr, "%s: too many arguments\n", args[0]);
        return SYSERR;
    }

    wifi_get_ip_str(ip, sizeof(ip));
    wifi_get_netmask_str(mask, sizeof(mask));
    wifi_get_gw_str(gw, sizeof(gw));
    wifi_get_mac_str(mac, sizeof(mac));

    printf("wlan0     HWaddr %s\n", mac);
    printf("          inet addr:%s  Mask:%s\n", ip, mask);
    printf("          Gateway:%s\n", gw);
    printf("          state:%s  dhcp:%s\n",
           wifi_is_link_up() ? "UP" : "DOWN",
           wifi_has_ip() ? "bound" : "pending");

    return OK;
}
