/**
 * @file audioIn.h
 *
 * Etapa de entrada del device audio: el microfono de la placa entra por el ADC
 * del ES8311, sale por el pad DIN del I2S0 en el mismo reloj que la salida
 * (duplex compartido) y lo recoge el canal IN del AHB-DMA sobre un anillo de
 * descriptores en PSRAM.
 *
 * No hay interrupciones, y tampoco hay que ceder descriptores: la lista es
 * ciclica y el motor corre solo, reescribiendo el trozo mas antiguo cuando el
 * consumidor no llega.  La posicion de escritura la da in_suc_eof_des_addr, el
 * descriptor donde acabo de ocurrir un fin de recepcion, porque el bloque I2S
 * no tiene contador de sincronizacion de entrada (lo tiene solo el TX).  Sobre
 * ese registro se lleva la cuenta de trozos producidos y consumidos.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _AUDIOIN_H_
#define _AUDIOIN_H_

#include <stdbool.h>
#include <stdint.h>

/* Bits de audio_in_status(). */
#define AUDIO_IN_ST_ON        (1U << 0)   /**< captura montada            */
#define AUDIO_IN_ST_LINK      (1U << 1)   /**< enlace IN dando vueltas    */
#define AUDIO_IN_ST_PARKED    (1U << 2)   /**< FSM del enlace aparcado    */
#define AUDIO_IN_ST_RX        (1U << 3)   /**< rx_start del I2S0          */
#define AUDIO_IN_ST_MIC       (1U << 4)   /**< ruta del codec abierta     */
#define AUDIO_IN_ST_DSCR_ERR  (1U << 5)   /**< el motor rechazo un descriptor  */
#define AUDIO_IN_ST_EMPTY     (1U << 6)   /**< sin descriptores libres          */
#define AUDIO_IN_ST_ERR_EOF   (1U << 7)   /**< fin de recepcion con error       */

/**
 * Montar la captura: ruta del microfono en el codec, RX del I2S0 en esclavo y
 * el canal IN del motor enlazado sobre un anillo nuevo.  Repetirla con la
 * captura ya en marcha es inocua.
 *
 * @return true si la captura quedo funcionando
 */
bool audio_in_start(void);

/** Parar la captura y soltar la memoria del anillo. */
void audio_in_stop(void);

/** true con la captura montada, sin mirar ningun registro. */
bool audio_in_running(void);

/**
 * Volcar en @p dst hasta @p nbytes de lo capturado, en tramas estereo de
 * 16 bits (4 bytes por trama).  No espera: devuelve lo que habia, y 0 cuando
 * el motor aun no ha llenado ningun trozo.
 *
 * @return numero de bytes escritos en @p dst
 */
uint32_t audio_in_read(void *dst, uint32_t nbytes);

/** Bytes que caben en el anillo, 0 si no esta montado. */
uint32_t audio_in_ring_bytes(void);

/** Frames perdidas por desbordamiento desde el ultimo arranque. */
uint32_t audio_in_dropped(void);

/** Estado combinado del codec, del I2S y del motor (bits AUDIO_IN_ST_*). */
uint32_t audio_in_status(void);

/** Descriptor que el motor esta procesando, en su forma de bus. */
uint32_t audio_in_dma_desc(void);

/** Ultimo descriptor cerrado por el motor (in_suc_eof_des_addr), 0 si parado. */
uint32_t audio_in_eof_desc(void);

/** Bytes capturados y sin entregar que siguen intactos en el anillo. */
uint32_t audio_in_queued(void);

/** Direccion del anillo de capturas en la forma que ve el motor, 0 si parado. */
uint32_t audio_in_ring_base(void);

#endif                          /* _AUDIOIN_H_ */
