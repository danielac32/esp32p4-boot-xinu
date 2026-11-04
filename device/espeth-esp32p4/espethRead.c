/**
 * @file espethRead.c
 *
 * ETH0 (ESP32-C6 por esp_hosted/SDIO): recibir una trama Ethernet.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <device.h>
#include <esp_hosted.h>
#include <esp_hosted_sdio.h>
#include <espeth.h>
#include <ethernet.h>
#include <interrupt.h>
#include <string.h>
#include <thread.h>

/**
 * @ingroup espeth
 *
 * Devolver UNA trama Ethernet cruda al hilo netRecv de la interfaz.
 *
 * El enlace es polled: no hay IRQ de SDIO atendido por software, asi que el
 * propio lector bombea el bus.  Cada vuelta hace
 *   esp_hosted_poll()         -> lee del SDHOST lo que el C6 haya dejado
 *                               subir y lo encola en esp_hosted
 *   esp_hosted_dispatch_rx()  -> vacia esa cola; las tramas STA/AP/ETH
 *                               caen en espethInput() y las SERIAL (RPC) en
 *                               la cb del controlador, que sigue funcionando
 *   si el anillo tiene algo, se copia y se devuelve
 * El sleep(1) entre intentos cede la CPU al shell y a la FAT, que comparten
 * el controlador SDMMC con este slot (el candado esta en system/sdmmcbus.c).
 *
 * @param devptr entrada del devtab
 * @param buf    destino; debe caber linkhdrlen + mtu (netRecv lo calcula)
 * @param len    capacidad de buf en bytes
 * @return bytes de trama copiados, TIMEOUT si no llego nada en ~1 s,
 *         SYSERR si el device no esta listo o el buffer es valido
 */
devcall espethRead(device *devptr, void *buf, uint len)
{
    struct espeth *ethptr = &espethtab[devptr->minor];
    irqmask im;
    uint tries;
    uint copy;

    if (!ethptr->ready || buf == NULL || len < ETH_HDR_LEN)
    {
        return SYSERR;
    }

    /* El enlace es polled, asi que este hilo es el unico punto garantizado
     * donde el transporte ya esta vivo: rearma la cb antes de bombear (ver
     * espethBindInput, y por que hace falta). */
    espethBindInput(ethptr);

    for (tries = 0U; tries < ESPETH_RX_TRIES; tries++)
    {
        if (esp_hosted_sdio_is_attached())
        {
            esp_hosted_poll();
            esp_hosted_dispatch_rx();
        }

        im = disable();
        if (ethptr->count > 0U)
        {
            /* Un solo lector (el hilo netRecv de la interfaz), asi que la
             * ranura rd es nuestra mientras count>0; solo hay que publicar
             * el avance de los indices. */
            copy = ethptr->slot_len[ethptr->rd];
            if (copy > len)
            {
                copy = len;
            }
            memcpy(buf, ethptr->slot[ethptr->rd], copy);
            ethptr->rd = (uint8_t)((ethptr->rd + 1U) % ESPETH_RX_SLOTS);
            ethptr->count--;
            restore(im);
            return (devcall)copy;
        }
        restore(im);

        sleep(1);
    }

    return TIMEOUT;
}
