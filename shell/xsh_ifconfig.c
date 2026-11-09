/**
 * @file xsh_ifconfig.c
 *
 * Estado de la interfaz Ethernet: el ETH0 del P4 es el ESP32-C6 colgado del
 * esp_hosted por SDIO, asi que el MAC que se imprime aqui no lo pregunta el
 * driver (netUp() lo pide con las interrupciones enmascaradas); lo publico
 * `netup' desde el RPC Req_GetMacAddress con espethSetMac().
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <stdio.h>
#include <string.h>
#include <shell.h>
#include <device.h>
#include <network.h>
#include <ipv4.h>

static void show_iface(int dev)
{
    struct netif *iface = netLookup(dev);
    char buf[32];

    if (!iface)
    {
        printf("%s: no such interface\n", devtab[dev].name);
        return;
    }

    printf("%s: flags=UP,BROADCAST,RUNNING\n", devtab[dev].name);
    netaddrsprintf(buf, &iface->ip);
    printf("        inet %s\n", buf);
    netaddrsprintf(buf, &iface->mask);
    printf("        netmask %s\n", buf);
    netaddrsprintf(buf, &iface->gateway);
    printf("        gateway %s\n", buf);
    netaddrsprintf(buf, &iface->hwaddr);
    printf("        ether %s\n", buf);
    printf("        mtu %u\n", iface->mtu);
}

shellcmd xsh_ifconfig(int nargs, char *args[])
{
    int descrp;

    if (nargs <= 1 || !strcmp(args[1], "-a"))
    {
        /* La interfaz se busca por nombre en el devtab, no por la macro: si el
         * conf renumera los devices el comando sigue mostrando la correcta, y
         * show_iface() indexa devtab[] sin comprobar, asi que el numero malo
         * tiene que morir aqui. */
        descrp = getdev("ETH0");
        if (isbaddev(descrp))
        {
            printf("ifconfig: no hay device \"ETH0\"\n");
            return SHELL_OK;
        }
        show_iface(descrp);
        return SHELL_OK;
    }

    if (!strcmp(args[1], "--help"))
    {
        printf("Usage: ifconfig [-a]\n");
        printf("Shows the configured IPv4 address, netmask, gateway and MAC.\n");
        return SHELL_OK;
    }

    printf("ifconfig: unknown option '%s'\n", args[1]);
    return SHELL_ERROR;
}
