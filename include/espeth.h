/**
 * @file espeth.h
 *
 * ETH0: el enlace ESP32-C6 presentado como interfaz de red de Xinu.
 *
 * El C6 es un esclavo esp_hosted colgado del slot SDIO 1.  Por ese slot
 * viajan TRAMAS ETHERNET CRUDAS: el transporte de device/sdio-esp32p4
 * desmonta la cabecera hosted de 12 bytes en la subida y la monta en la
 * bajada, asi que aqui la trama llega y sale completa, con sus 14 bytes de
 * cabecera Ethernet.  Ese es exactamente el contrato que exige la pila
 * nativa:
 *   - network/net/netRecv.c hace read(dev, buf, linkhdrlen + mtu) y espera
 *     UNA trama completa por llamada;
 *   - network/net/netSend.c hace write(dev, trama, len) y exige que write()
 *     devuelva EXACTAMENTE len;
 *   - network/net/netUp.c pregunta mtu / linkhdrlen / hwaddr por control(),
 *     y lo hace con las interrupciones MASKED (netUp entra con disable()),
 *     o sea: control() jamas puede bloquearse ni hacer un RPC.
 *
 * Es el mismo esquema del puerto ESP32-S3, donde el W5500 trabaja en MACRAW
 * y la pila la pone Xinu.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _ESPETH_H_
#define _ESPETH_H_

#include <conf.h>
#include <esp_hosted.h>
#include <monitor.h>
#include <stdbool.h>
#include <stdint.h>

/* MTU ethernet de 802.3.  La trama maxima que sube del C6 es el buffer de
 * RX del transporte (ESP_HOSTED_RX_PAYLOAD_MAX = 1600), asi que entra holgada. */
#define ESPETH_MTU        1500U
#define ESPETH_FRAME_MAX  ESP_HOSTED_RX_PAYLOAD_MAX

/* Anillo de tramas recibidas dentro del driver.  La cb de entrada corre en
 * el hilo que bombea el SDIO (el de netRecv, o el que esté esperando un RPC),
 * y puede descargar varias tramas de una vez; el lector solo es netRecv.
 * Con 4 ranuras se llenaba en cuanto el HTTP tenia trafico: `wifi stats'
 * contaba caidas_rx subiendo de 102 a 129 en 8 segundos mientras el lector
 * esperaba el candado del bus que le quitaba una escritura.  8 ranuras son
 * 12,8 KB de .bss y cubren la descarga maxima del transporte (cola de 8). */
#define ESPETH_RX_SLOTS   8U
#define ESPETH_RX_TRIES   1000U   /* ~1 s esperando trama (1 tick = 1 ms) */

/* Códigos de control propios, aparte de los NET_GET_* de <network.h>. */
#define ESPETH_CTRL_LINK      260 /**< arg1 = 0; 1 si el STA esta asociado  */
#define ESPETH_CTRL_MAC_SET   261 /**< arg1 = const uint8_t (*)[6] del C6    */
#define ESPETH_CTRL_STATS     262 /**< arg1 = struct espeth_stats *          */

/** Contadores de diagnostico, leibles con ESPETH_CTRL_STATS. */
struct espeth_stats
{
    uint32_t rx_frames;     /**< tramas entregadas a la pila              */
    uint32_t rx_drops;      /**< tramas perdidas por anillo lleno         */
    uint32_t tx_frames;     /**< tramas escritas al SDIO                  */
    uint32_t tx_drops;      /**< escrituras SDIO rechazadas               */
};

/** Estado por interfaz. */
struct espeth
{
    bool     ready;                                  /**< init completado     */
    bool     mac_valid;                              /**< mac[] cargado       */
    uint8_t  mac[6];                                 /**< MAC real del STA C6 */
    uint8_t  slot[ESPETH_RX_SLOTS][ESPETH_FRAME_MAX];
    uint16_t slot_len[ESPETH_RX_SLOTS];
    uint8_t  wr;                                     /**< ranura de escritura */
    uint8_t  rd;                                     /**< ranura de lectura   */
    uint8_t  count;                                  /**< tramas en el anillo */
    monitor  lock;                                   /**< tx de una trama     */
    struct espeth_stats stats;
};

extern struct espeth espethtab[NETHER];

/* Funciones de dispositivo */
devcall espethInit(device *);
devcall espethOpen(device *, va_list);
devcall espethClose(device *);
devcall espethRead(device *, void *, uint);
devcall espethWrite(device *, const void *, uint);
devcall espethControl(device *, int, long, long);

/* Glue del driver (espeth.c) */
void    espethSetMac(const uint8_t mac[6]);
bool    espethLinkIsUp(void);

/* Re-armado de la cb de entrada (espethInit.c): esp_hosted_init() la borra
 * y `wifi up' la invoca despues del devinit. */
void    espethBindInput(struct espeth *ethptr);

#endif                          /* _ESPETH_H_ */
