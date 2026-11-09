/**
 * @file espethInit.c
 *
 * ETH0 (ESP32-C6 por esp_hosted/SDIO): inicializacion del device.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <device.h>
#include <esp_hosted.h>
#include <espeth.h>
#include <interrupt.h>
#include <string.h>

/** Una entrada por interfaz; NETHER lo fija xinu.conf (hoy vale 1). */
struct espeth espethtab[NETHER];

/**
 * Callback de entrada de tramas Ethernet.
 *
 * La invoca esp_hosted_dispatch_rx() (device/sdio-esp32p4/espHosted.c) por
 * cada trama cuyo if_type es STA / AP / ETH, en el contexto del hilo que
 * este bombeando el SDIO.  Copia la trama al anillo del driver; si el anillo
 * esta lleno se descarta y se cuenta, porque no hay nada donde reintentar:
 * el buffer del transporte es la cola de 8 tramas de esp_hosted y ya se ha
 * consumido para llegar aqui.
 *
 * @param info    cabecera hosted ya decodificada (if_type, if_num, len...)
 * @param payload la trama Ethernet completa, cabecera de 14 bytes incluida
 * @param len     longitud de esa trama en bytes
 * @param ctx     struct espeth * de la interfaz
 */
static void espethInput(const esp_hosted_frame_info_t *info,
                        const uint8_t *payload,
                        size_t len,
                        void *ctx)
{
    struct espeth *ethptr = (struct espeth *)ctx;
    irqmask im;

    if (ethptr == NULL || info == NULL || payload == NULL)
    {
        return;
    }

    if (len == 0U || len > ESPETH_FRAME_MAX)
    {
        ethptr->stats.rx_drops++;
        return;
    }

    im = disable();
    if (ethptr->count < ESPETH_RX_SLOTS)
    {
        memcpy(ethptr->slot[ethptr->wr], payload, len);
        ethptr->slot_len[ethptr->wr] = (uint16_t)len;
        ethptr->wr = (uint8_t)((ethptr->wr + 1U) % ESPETH_RX_SLOTS);
        ethptr->count++;
        ethptr->stats.rx_frames++;
    }
    else
    {
        ethptr->stats.rx_drops++;
    }
    restore(im);
}

/**
 * @ingroup espeth
 *
 * Volver a publicar la cb de entrada en la capa esp_hosted.
 *
 * Hace falta porque esp_hosted_init() (device/sdio-esp32p4/espHosted.c) llama
 * a esp_hosted_reset_state(), que memset-a s_lwip_cb; y `wifi up' la invoca
 * DESPUES de esta devinit (espHostedSdio.c elige esp_hosted_init() solo si el
 * transporte aun no esta inicializado).  O sea: el registro hecho aqui en
 * devinit quedaba borrado por `wifi up' y las tramas if_type=STA subian del
 * C6 pero no caian en ningun sitio -- rx de ETH0 eternamente 0, aunque un
 * esp_hosted_rx_pop() en la consola si las veia.
 *
 * El SDK de referencia hace lo contrario: registra sus callbacks DESPUES de
 * hosted_init.  Como aqui el registro tiene que sobrevivir a un `wifi up'
 * tardio (o a varios), quien consume el enlace se re-arma antes de leer; por
 * eso este glue es no-static y lo llama espethRead().
 *
 * @param ethptr interfaz duena de las tramas
 */
void espethBindInput(struct espeth *ethptr)
{
    if (ethptr != NULL)
    {
        esp_hosted_lwip_set_input_cb(espethInput, ethptr);
    }
}

/**
 * @ingroup espeth
 *
 * Init del device: limpia el estado y registra el callback de tramas Ethernet
 * en la capa esp_hosted.
 *
 * Se ejecuta en sysinit (devinit), antes de que el SDIO este adjunto; por
 * eso solo toca memoria, no el bus.  adjuntar el C6 y asociar la STA los hace
 * la consola con `wifi up` / `wifi join`.
 *
 * @param devptr entrada del devtab
 * @return OK
 */
devcall espethInit(device *devptr)
{
    struct espeth *ethptr = &espethtab[devptr->minor];

    memset(ethptr, 0, sizeof(*ethptr));
    ethptr->lock = moncreate();
    if (SYSERR == (int)ethptr->lock)
    {
        return SYSERR;
    }
    ethptr->ready = true;

    espethBindInput(ethptr);

    return OK;
}
