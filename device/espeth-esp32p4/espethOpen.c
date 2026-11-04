/**
 * @file espethOpen.c
 *
 * ETH0 (ESP32-C6 por esp_hosted/SDIO): apertura del device.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <device.h>
#include <espeth.h>
#include <stdarg.h>

/**
 * @ingroup espeth
 *
 * No hay nada que reservar por apertura: el estado del enlace vive en
 * espethtab y la pila tampoco abre el device (netUp() recibe el indice de
 * la consola).  Se deja implementado porque el devtab exige la ranura.
 *
 * @param devptr entrada del devtab
 * @param ap     sin uso
 * @return OK
 */
devcall espethOpen(device *devptr, va_list ap)
{
    struct espeth *ethptr = &espethtab[devptr->minor];

    (void)ap;

    if (!ethptr->ready)
    {
        return SYSERR;
    }

    return OK;
}
