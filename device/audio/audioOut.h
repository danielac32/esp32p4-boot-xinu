/**
 * @file audioOut.h
 *
 * Etapa de salida del device audio: un anillo de PCM en PSRAM que el
 * AHB-DMA repasa sin parar y entrega al FIFO TX del I2S0, con el codec y el
 * amplificador de la placa en la secuencia justa para que no se oiga un golpe
 * al empezar ni al terminar.
 *
 * Dos modos sobre el mismo camino: el tono, que escribe el patron una vez y lo
 * deja dando vueltas (audio_out_start), y el stream, donde el anillo es de
 * PSRAM y se rellena por audio_out_stream_write() mientras suena, que es lo
 * que usa el write() del device.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _AUDIOOUT_H_
#define _AUDIOOUT_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * Montar y arrancar la salida con un tono de @p req_hz hercios.
 *
 * La frecuencia se cuantifica al valor mas cercano que cierra el bucle en un
 * numero entero de periodos dentro del anillo: si no, cada vuelta del DMA
 * cortaria la onda y se oiria un chasquido.  @p applied_hz y @p ring_frames
 * devuelven lo que realmente suena y la longitud del anillo en frames.
 *
 * @param req_hz     frecuencia pedida (50..8000 Hz)
 * @param applied_hz frecuencia efectivamente programada
 * @param ring_frames frames del anillo (una vuelta del DMA)
 * @return true si la salida quedo sonando
 */
bool audio_out_start(uint32_t req_hz, uint32_t *applied_hz,
                     uint32_t *ring_frames);

/** Parar la salida: amplificador fuera, codec muteado, DMA e I2S parados. */
void audio_out_stop(void);

/* Bits de audio_out_status(): el estado del pipeline tal y como lo ven los
 * propios registros, para poder diagnosticar sin sonda. */
#define AUDIO_OUT_ST_ON          (1U << 0)   /**< salida montada            */
#define AUDIO_OUT_ST_LINK        (1U << 1)   /**< enlace DMA dando vueltas  */
#define AUDIO_OUT_ST_DSCR_ERR    (1U << 2)   /**< el motor rechazo la lista */
#define AUDIO_OUT_ST_DMA_EOF     (1U << 3)   /**< descriptor terminado      */
#define AUDIO_OUT_ST_DMA_DONE    (1U << 4)   /**< datos entregados al I2S   */
#define AUDIO_OUT_ST_TX_START    (1U << 5)   /**< tx_start del I2S          */
#define AUDIO_OUT_ST_TX_IDLE     (1U << 6)   /**< FSM TX parado             */
#define AUDIO_OUT_ST_AMP         (1U << 7)   /**< AMP_POWER (GPIO53) alta   */
#define AUDIO_OUT_ST_STREAM      (1U << 8)   /**< anillo de stream, no tono */

/** Estado combinado del DMA, del I2S y del amplificador. */
uint32_t audio_out_status(void);

/** Direccion actual del enlace tal y como la ve el motor, 0 si no ha leido. */
uint32_t audio_out_dma_desc(void);

/* Datos que el I2S ha sacado desde el ultimo arranque de la salida: si este
 * numero crece, el camino DMA -> FIFO -> I2S esta vivo. */
uint32_t audio_out_sync_count(void);

/* Frames del anillo que suena ahora (una vuelta del DMA); 0 con la salida
 * parada.  Su duracion es frames/48000 s y de ahi sale el periodo del chasquido
 * si el patron no cerrara. */
uint32_t audio_out_ring_frames(void);

/**
 * @name Stream
 *
 * El mismo camino de salida, pero con el anillo en PSRAM y relleno desde
 * fuera.  El formato es el del cable: tramas estereofonicas de dos slots de
 * 16 bits, 4 bytes por trama, 48 kHz.  Quien llama no tiene que escribir por
 * trozos de tamano alguno: el device mantiene el cursor y espera cuando el
 * anillo se llena.
 *
 * @{
 */

/** Montar la salida en modo stream y dejarla sonando en silencio. */
bool audio_out_stream_start(void);

/**
 * Escribir @p nbytes de PCM en la salida, bloqueando mientras el anillo este
 * lleno.  Se aceptan solo tramas enteras: devuelve los bytes entregados, que
 * seran un multiplo de 4 y podran ser menos de los pedidos si la última trama
 * quedaba incompleta.
 */
uint32_t audio_out_stream_write(const void *buf, uint32_t nbytes);

/** Esperar a que lo ultimo escrito haya salido por el I2S. */
void audio_out_stream_drain(void);

/** Tramas escritas y aun dentro del anillo, sin salir por el I2S. */
uint32_t audio_out_stream_queued(void);

/** Capacidad del anillo en tramas, 0 si no hay stream montado. */
uint32_t audio_out_stream_capacity(void);

/** true con el anillo de stream montado (en vez del patron del tono). */
bool audio_out_streaming(void);

/** Direccion del anillo de stream en la forma que ve el motor, 0 si no hay. */
uint32_t audio_out_stream_base(void);

/** Huecos de reproduccion de este stream: veces que el motor dio la vuelta por
 *  delante de quien escribe.  Cada uno es audio que dejo de sonar cuando tenia
 *  que sonar; con 0 la reproduccion fue continua. */
uint32_t audio_out_stream_gaps(void);

/**@}*/

#endif                          /* _AUDIOOUT_H_ */
