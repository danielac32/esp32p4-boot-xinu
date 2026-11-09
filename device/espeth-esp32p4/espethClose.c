/**
 * @file espethClose.c
 *
 * ETH0 (ESP32-C6 por esp_hosted/SDIO): cierre del device.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <device.h>
#include <espeth.h>

/**
 * @ingroup espeth
 *
 * Cerrar la interfaz.  No hay recursos por hilo: el anillo de RX y el
 * monitor viven en la tabla del driver, y bajar la interfaz la hace
 * netDown(), que mata los hilos netRecv.
 *
 * @param devptr entrada del devtab
 * @return OK
 */
devcall espethClose(device *devptr)
{
    struct espeth *ethptr = &espethtab[devptr->minor];

    if (!ethptr->ready)
    {
        return SYSERR;
    }

    return OK;
}
