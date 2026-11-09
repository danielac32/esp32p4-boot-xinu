/**
 * @file xsh_netup.c
 *
 * Shell command `netup`: subir una interfaz de red.
 *
 * En esta placa la unica interfaz es ETH0, el ESP32-C6 colgado del slot 1
 * del controlador SDMMC (device/espeth-esp32p4).  No hay PHY que negociar:
 * el "cable" es la asociacion del STA a la AP, y eso lo decide el C6.  Por
 * eso antes de DHCP hay que comprobar dos cosas que una interfaz por cable
 * no necesita:
 *   1) que el STA este asociado (control(ESPETH_CTRL_LINK), que pregunta al
 *      controlador esp_hosted), y
 *   2) que el driver tenga el MAC del STA, porque netUp() lo pide por
 *      NET_GET_HWADDR con las interrupciones enmascaradas y alli no se puede
 *      esperar un RPC.  Este comando si puede bloquear, asi que lanza el RPC
 *      Req_GetMacAddress (id=257) y se lo pasa al driver con espethSetMac().
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <conf.h>

#if NETHER

#include <clock.h>
#include <device.h>
#include <dhcpc.h>
#include <esp_hosted.h>
#include <esp_hosted_ctrl.h>
#include <espeth.h>
#include <ipv4.h>
#include <network.h>
#include <shell.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread.h>

/* Segundos de espera maxima por la asociacion del STA. */
#define NETUP_LINK_TRIES    15U

static void usage(const char *command);

/**
 * @ingroup shell
 *
 * @param nargs  numero de argumentos en args
 * @param args   vector de argumentos
 * @return SHELL_OK si la interfac quedo arriba
 */
shellcmd xsh_netup(int nargs, char *args[])
{
    int descrp;
    struct netaddr ip;
    struct netaddr mask;
    struct netaddr gateway;
    struct netaddr *gatewayptr;
    const char *str_ip, *str_mask, *str_gateway;
    const char *devname;
    int result;

    /* Ayuda */
    if (2 == nargs && 0 == strcmp(args[1], "--help"))
    {
        usage(args[0]);
        return SHELL_OK;
    }

    /* Dispositivo indicado en la linea de comandos */
    if (nargs > 1)
    {
        descrp = getdev(args[1]);
        devname = args[1];
        if (SYSERR == descrp)
        {
            fprintf(stderr, "ERROR: %s no es un dispositivo valido\n", devname);
            return SHELL_ERROR;
        }
    }
    else
    {
        /* Sin argumento se sube el enlace del C6, buscandolo por nombre en el
         * devtab igual que cuando se escribe en la linea de comandos. */
        devname = "ETH0";
        descrp = getdev(devname);
        if (SYSERR == descrp)
        {
            fprintf(stderr, "ERROR: %s no esta en la tabla de devices\n",
                    devname);
            return SHELL_ERROR;
        }
    }

    if (NULL != netLookup(descrp))
    {
        fprintf(stderr, "ERROR: %s ya esta arriba; usa `netdown' antes\n",
                devname);
        return SHELL_ERROR;
    }

    if (descrp < ETH0 || descrp >= ETH0 + NETHER)
    {
        fprintf(stderr, "ERROR: %s no es un dispositivo de red\n", devname);
        return SHELL_ERROR;
    }

    str_ip = NULL;
    str_mask = NULL;
    str_gateway = NULL;

    /* El MAC del STA hace falta para las dos vias (estatica y DHCP): netUp()
     * lo pide por NET_GET_HWADDR con las interrupciones enmascaradas, y ahi el
     * driver solo puede devolver lo que ya tenga cacheado.  Este comando si
     * puede bloquear, asi que lanza el RPC y se lo entrega al driver. */
    {
        uint8_t mac[6];

        if (0 != esp_hosted_wifi_get_mac(ESP_HOSTED_WIFI_IF_STA, mac, 3000U))
        {
            fprintf(stderr,
                    "ERROR: %s no consegui el MAC del STA (hace falta `wifi "
                    "up' y `wifi init', o `wifi join <ssid> <pass>' antes)\n",
                    devname);
            return SHELL_ERROR;
        }
        espethSetMac(mac);
        printf("%s mac %02x:%02x:%02x:%02x:%02x:%02x\n",
               devname, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    if (nargs >= 3)
    {
        str_ip = args[2];

        if (nargs >= 4)
        {
            str_mask = args[3];

            if (nargs >= 5)
            {
                str_gateway = args[4];
            }
        }
    }

    if (NULL != str_ip)
    {
        /* Configuracion estatica */
        if (NULL == str_mask)
        {
            str_mask = "255.255.255.0";
            fprintf(stderr, "AVISO: mascara por defecto %s\n", str_mask);
        }

        if (SYSERR == dot2ipv4(str_ip, &ip))
        {
            fprintf(stderr, "ERROR: %s no es una direccion IPv4 valida\n", str_ip);
            return SHELL_ERROR;
        }

        if (SYSERR == dot2ipv4(str_mask, &mask))
        {
            fprintf(stderr, "ERROR: %s no es una mascara valida\n", str_mask);
            return SHELL_ERROR;
        }

        if (NULL != str_gateway)
        {
            if (SYSERR == dot2ipv4(str_gateway, &gateway))
            {
                fprintf(stderr, "ERROR: %s no es una direccion IPv4 valida\n",
                        str_gateway);
                return SHELL_ERROR;
            }
            gatewayptr = &gateway;
        }
        else
        {
            fprintf(stderr, "AVISO: sin puerta de enlace\n");
            gatewayptr = NULL;
        }
    }
    else
    {
        /* Sin IP: DHCP, y antes el enlace y el MAC del C6 */
#ifdef WITH_DHCPC
        struct dhcpData data;
        uint tries;
        int link = 0;

        for (tries = 0U; tries < NETUP_LINK_TRIES; tries++)
        {
            if (OK == control(descrp, ESPETH_CTRL_LINK, 0, 0))
            {
                link = 1;
                break;
            }
            sleep(1000);
        }

        if (!link)
        {
            fprintf(stderr,
                    "ERROR: %s sin enlace: el STA del C6 no esta asociado "
                    "(usa `wifi join <ssid> <pass>')\n", devname);
            return SHELL_ERROR;
        }

        printf("DHCP en %s...\n", devname);

        /* 20 s, no 10: el unico bombeo del bus mientras espera la oferta es el
         * read() del hilo de dhcpRecvReply, y ese read agota su propio anillo
         * de 1 s por vuelta.  Con 10 s el margen para una AP perezosa era de
         * un par de DISCOVER. */
        result = dhcpClient(descrp, 20, &data);

        if (OK != result)
        {
            struct espeth_stats st;

            /* Los contadores del driver dicen en que sentido fallo el
             * intento: tx=0 es que la DHCPDISCOVER no salio al C6, y
             * tx>0/rx=0 es que el enlace saca tramas pero no entra ninguna
             * (oferta ausente o filtro del esclavo). */
            if (OK == control(descrp, ESPETH_CTRL_STATS, (long)&st, 0))
            {
                fprintf(stderr, "%s tx=%u caidas_tx=%u rx=%u caidas_rx=%u\n",
                        devname, st.tx_frames, st.tx_drops,
                        st.rx_frames, st.rx_drops);
            }
        }

        if (TIMEOUT == result)
        {
            fprintf(stderr, "ERROR: DHCP en %s agoto los 20 s\n", devname);
            return SHELL_ERROR;
        }
        else if (OK != result)
        {
            fprintf(stderr, "ERROR: DHCP en %s fallo (rc=%d)\n", devname, result);
            return SHELL_ERROR;
        }

        netaddrcpy(&ip, &data.ip);
        netaddrcpy(&mask, &data.mask);
        if (0 != data.gateway.len)
        {
            netaddrcpy(&gateway, &data.gateway);
            gatewayptr = &gateway;
        }
        else
        {
            gatewayptr = NULL;
        }
#else
        fprintf(stderr,
                "ERROR: sin DHCP: recompila con WITH_DHCPC en xinu.conf o da "
                "la IP a mano (`? %s')\n", args[0]);
        return SHELL_ERROR;
#endif
    }

    result = netUp(descrp, &ip, &mask, gatewayptr);

    if (OK == result)
    {
        char str_ip_buf[20];
        char str_mask_buf[20];
        char str_gateway_buf[20];

        netaddrsprintf(str_ip_buf, &ip);
        netaddrsprintf(str_mask_buf, &mask);

        if (NULL != gatewayptr)
        {
            netaddrsprintf(str_gateway_buf, gatewayptr);
            printf("%s es %s mascara %s (gw %s)\n",
                   devname, str_ip_buf, str_mask_buf, str_gateway_buf);
        }
        else
        {
            printf("%s es %s mascara %s (sin gw)\n",
                   devname, str_ip_buf, str_mask_buf);
        }

        return SHELL_OK;
    }

    fprintf(stderr, "ERROR: no se pudo subir la interfaz en %s\n", devname);
    return SHELL_ERROR;
}

static void usage(const char *command)
{
    printf(
"Uso:\n"
"\t%s [<DISPOSITIVO> [<IP> [<MASCARA> [<PUERTA>]]]]\n"
"Descripcion:\n"
"\tSube una interfaz de red.  Sin IP usa DHCP; para eso el STA del C6 tiene\n"
"\tque estar asociado (`wifi join <ssid> <pass>') porque el enlace del C6 es\n"
"\tsu asociacion, no un cable.\n"
"Ejemplos:\n"
"\t%s\n"
"\t%s ETH0\n"
"\t%s ETH0 192.168.1.50 255.255.255.0 192.168.1.1\n",
           command, command, command, command);
}

#endif                          /* NETHER */
