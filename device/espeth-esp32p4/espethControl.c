/**
 * @file espethControl.c
 *
 * ETH0 (ESP32-C6 por esp_hosted/SDIO): control() de la interfaz de red.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <device.h>
#include <espeth.h>
#include <ether.h>
#include <ethernet.h>
#include <network.h>

/**
 * @ingroup espeth
 *
 * Responder a lo que pregunta la pila (y a lo que pregunta la consola).
 *
 * Ojo: netUp() llama a NET_GET_MTU / NET_GET_LINKHDRLEN / NET_GET_HWADDR /
 * NET_GET_HWBRC con las interrupciones enmascaradas, asi que ninguna rama
 * puede esperar nada del C6.  El MAC viene cacheado por espethSetMac() desde
 * xsh_wifi, que si puede bloquear en un RPC.
 *
 * @param devptr entrada del devtab
 * @param func   NET_GET_* de <network.h>, ETH_CTRL_GET_MAC de <ether.h> o
 *               ESPETH_CTRL_* de <espeth.h>
 * @param arg1   destino segun el codigo
 * @param arg2   sin uso
 * @return el valor pedido, o SYSERR
 */
devcall espethControl(device *devptr, int func, long arg1, long arg2)
{
    struct espeth *ethptr = &espethtab[devptr->minor];
    struct netaddr *addr;
    struct espeth_stats *stats;
    const uint8_t *mac;
    int i;

    (void)arg2;

    if (!ethptr->ready)
    {
        return SYSERR;
    }

    switch (func)
    {
        /* ---------- lo que exige la pila ---------- */

    case NET_GET_MTU:
        return (devcall)ESPETH_MTU;

    case NET_GET_LINKHDRLEN:
        return (devcall)ETH_HDR_LEN;

    case NET_GET_HWADDR:
        addr = (struct netaddr *)arg1;
        if (addr == NULL || !ethptr->mac_valid)
        {
            return SYSERR;
        }
        addr->type = NETADDR_ETHERNET;
        addr->len = ETH_ADDR_LEN;
        for (i = 0; i < ETH_ADDR_LEN; i++)
        {
            addr->addr[i] = ethptr->mac[i];
        }
        return OK;

    case NET_GET_HWBRC:
        addr = (struct netaddr *)arg1;
        if (addr == NULL)
        {
            return SYSERR;
        }
        addr->type = NETADDR_ETHERNET;
        addr->len = ETH_ADDR_LEN;
        for (i = 0; i < ETH_ADDR_LEN; i++)
        {
            addr->addr[i] = 0xFFU;
        }
        return OK;

        /* ---------- lo que exige el cliente DHCP ---------- */

    case ETH_CTRL_GET_MAC:
    {
        /* dhcpClient() (network/dhcpc/dhcpClient.c) rellena aqui su
         * clientHwAddr antes de mandar el primer DHCPDISCOVER; es el mismo
         * MAC cacheado que sirve NET_GET_HWADDR.  Si este codigo no estuviera
         * dhcpClient devolveria SYSERR (-1) sin llegar a emitir nada. */
        uint8_t *dst = (uint8_t *)arg1;

        if (dst == NULL || !ethptr->mac_valid)
        {
            return SYSERR;
        }
        for (i = 0; i < ETH_ADDR_LEN; i++)
        {
            dst[i] = ethptr->mac[i];
        }
        return OK;
    }

        /* ---------- diagnostico de consola ---------- */

    case ESPETH_CTRL_LINK:
        return espethLinkIsUp() ? OK : SYSERR;

    case ESPETH_CTRL_MAC_SET:
        mac = (const uint8_t *)arg1;
        if (mac == NULL)
        {
            return SYSERR;
        }
        espethSetMac(mac);
        return OK;

    case ESPETH_CTRL_STATS:
        stats = (struct espeth_stats *)arg1;
        if (stats == NULL)
        {
            return SYSERR;
        }
        *stats = ethptr->stats;
        return OK;

    default:
        return SYSERR;
    }
}
