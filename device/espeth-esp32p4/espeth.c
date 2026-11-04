/**
 * @file espeth.c
 *
 * ETH0 (ESP32-C6 por esp_hosted/SDIO): glue del driver.
 *
 * Aqui vive lo que no es una devcall: el cacheo del MAC del STA y el estado
 * de enlace.  Lo importante es POR QUE el MAC se cachea y no se pide aqui
 * mismo: network/net/netUp.c llama a control(NET_GET_HWADDR) con las
 * interrupciones enmascaradas (entra con disable() y no restaura hasta crear
 * los hilos netRecv), y una peticion RPC al C6 necesita timer y SDIO.  Si
 * el driver preguntara por el MAC dentro de control(), el arranque de red
 * se colgaria.  Asi que el MAC lo trae de fuera quien si puede bloquear
 * (shell/xsh_netup.c, tras el RPC Req_GetMacAddress id=257) y lo deja aqui.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <device.h>
#include <esp_hosted.h>
#include <esp_hosted_ctrl.h>
#include <esp_hosted_sdio.h>
#include <espeth.h>
#include <string.h>

/**
 * @ingroup espeth
 *
 * Dejar publicado el MAC del STA del C6 para que netUp() lo lea por
 * NET_GET_HWADDR.
 *
 * @param mac 6 bytes, en orden de aire (mac[0] es el byte mas significativo
 *            del OUI, igual que en la trama Ethernet)
 */
void espethSetMac(const uint8_t mac[6])
{
    struct espeth *ethptr = &espethtab[0];

    if (mac == NULL)
    {
        return;
    }

    memcpy(ethptr->mac, mac, sizeof(ethptr->mac));
    ethptr->mac_valid = true;
}

/**
 * @ingroup espeth
 *
 * Estado del enlace: el "cable" del C6 es su asociacion a la AP, medida por
 * el controlador esp_hosted (eventos StaConnected / StaStopped).
 *
 * @return true si el STA esta asociado y el SDIO esta adjunto
 */
bool espethLinkIsUp(void)
{
    return esp_hosted_sdio_is_attached() && esp_hosted_wifi_sta_is_connected();
}
