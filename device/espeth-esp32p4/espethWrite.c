/**
 * @file espethWrite.c
 *
 * ETH0 (ESP32-C6 por esp_hosted/SDIO): enviar una trama Ethernet.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <device.h>
#include <esp_hosted.h>
#include <esp_hosted_sdio.h>
#include <espeth.h>
#include <ethernet.h>
#include <monitor.h>

/**
 * @ingroup espeth
 *
 * Bajar una trama Ethernet completa al C6 por el canal de datos (if_type
 * STA, if_num 0).  El transporte monta la cabecera hosted de 12 bytes y
 * escribe SLAVE_CMD53_END_ADDR - len, todo eso ya esta en espHostedSdio.c.
 *
 * netSend() exige que el valor devuelto sea exactamente la longitud de la
 * trama, asi que solo se devuelve el largo cuando el transporte escribio los
 * mismos bytes que le pasamos; si no, SYSERR.
 *
 * El monitor es porque puede haber DOS hilos escribiendo a la vez: el de la
 * shell (dhcpSendRequest, un TCP/UDP de app) y el hilo netRecv de la
 * interfaz (arpSendReply de una respuesta ARP).  Sin candado se intercalarían
 * el token y el CMD53 de dos tramas distintas y el esclavo las descartaría
 * por checksum.  Queda fuera de cobertura el caso RPC-vs-datos (el ctrl del
 * esp_hosted escribe por su cuenta en espHostedCtrl.c, que es port verbatim
 * y no se toca); ahi la ventana es corta y el esclavo descarta la trama
 * basura, y la pila reintenta ARP/DHCP sola.
 *
 * @param devptr entrada del devtab
 * @param buf    trama Ethernet (cabecera de 14 bytes incluida)
 * @param len    longitud de la trama
 * @return len si el C6 acepto la trama, SYSERR en cualquier otro caso
 */
devcall espethWrite(device *devptr, const void *buf, uint len)
{
    struct espeth *ethptr = &espethtab[devptr->minor];
    size_t written;

    if (!ethptr->ready || buf == NULL || len < ETH_HDR_LEN ||
        len > ESPETH_FRAME_MAX)
    {
        return SYSERR;
    }

    if (!esp_hosted_sdio_is_attached())
    {
        ethptr->stats.tx_drops++;
        return SYSERR;
    }

    lock(ethptr->lock);
    written = esp_hosted_lwip_tx_to_if(ESP_HOSTED_IF_STA, 0U, buf, (size_t)len);
    unlock(ethptr->lock);

    if (written != (size_t)len)
    {
        ethptr->stats.tx_drops++;
        return SYSERR;
    }

    ethptr->stats.tx_frames++;
    return (devcall)written;
}
