/**
 * @file i2s.h
 *
 * I2S0 del ESP32-P4 en modo maestro TX: relojes (MCLK, BCLK, LRCK) y estado
 * del bloque, para el ES8311.  Los datos los mete el AHB-DMA (device/audio/
 * ahbdma.c); aqui no hay FIFO accesible por CPU, solo la configuracion del
 * timing y sus contadores de medida.
 *
 * El divisor fraccionario de HP_SYS_CLKRST da 12,288 MHz exactos desde el
 * XTAL de 40 MHz, asi que aqui no interviene el APLL.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _AUDIO_I2S_H_
#define _AUDIO_I2S_H_

#include <stdbool.h>
#include <stdint.h>

#include "es8311.h"

/* Trama I2S estandard: dos slots de I2S_SLOT_BITS bits.  Es el ajuste que
 * E1 deja escrito en el codec, por eso se deriva de sus constantes. */
#define I2S_SLOT_BITS       16U
#define I2S_CHANNELS        2U
#define I2S_FRAME_BITS      (I2S_SLOT_BITS * I2S_CHANNELS)
#define I2S_BCLK_HZ         (ES8311_SAMPLE_RATE_HZ * I2S_FRAME_BITS)

/* Puesta en marcha y medida de los relojes.  Ninguna escribe en el codec ni
 * habilita el amplificador: con la salida muteada estos relojes son
 * inaudibles. */
bool i2s_clocks_start(void);
bool i2s_clocks_stop(void);
bool i2s_clocks_running(void);

/* BCLK medido por el contador interno del periferico (tx_bck_cnt) durante
 * @p us microsegundos; 0 si los relojes no estan corriendo. */
uint32_t i2s_bclk_measure(uint32_t us);

/* Lectura cruda del contador de BCLK (31 bits, una vuelta cada 1.39 s con
 * 1.536 MHz).  Vale como reloj de referencia para medir ritmos de audio sin
 * depender del tic del kernel; solo interesan los incrementos entre dos
 * lecturas.  i2s_bck_reset() lo pone a cero. */
uint32_t i2s_bck_count(void);
void i2s_bck_reset(void);

/* Estado del bloque TX, separado en sus dos bits para poder diagnosticar:
 * tx_start (el mando) y tx_idle (el FSM).  i2s_tx_sync_count() no es el nivel
 * del FIFO — en el P4 el FIFO esta detras del DMA y no tiene registro de llenado
 * —: es el contador de sincronizacion que cuenta lo que el bloque ha sacado.
 * Vale como prueba de flujo, y en placa su ritmo es el de muestras de 16 bits:
 * con 48 kHz estereo crece a 96.000 por segundo, dos por trama.  Si se queda
 * quieto, el TX esta hambriento; el SDK usa el mismo contador para eso. */
bool i2s_tx_started(void);
bool i2s_tx_idle(void);
uint32_t i2s_tx_sync_count(void);

/* Poner a cero el contador de sincronizacion, para que lo que se lea despues
 * sea lo emitido desde aqui. */
void i2s_tx_sync_reset(void);

/* RX del mismo bloque, para capturar al microfono.  En el P4 no hay reloj RX
 * que valga en duplex compartido: el RX se pone esclavo y recibe por dentro
 * los BCLK/LRCK del TX (tx_conf.sig_loopback), y el MCLK es comun.  Por eso
 * i2s_rx_start() exige y deja arrancados los relojes TX, y aqui no hay
 * contador de sincronizacion que leer: la posicion de la captura la da el
 * propio DMA (device/audio/ahbdma.c). */
/* Bytes por evento de fin de recepcion del RX.  El campo rx_eof_num del P4
 * cuenta bytes: de reset vale 64, y con bits_mod de 16 programado, el valor
 * 511 dio en placa un corte cada 512 bytes leidos por el motor.  El driver de
 * referencia hace lo mismo: ajusta el eof al tamano del buffer del descriptor
 * (esp_driver_i2s/i2s_common.c, i2s_ll_rx_set_eof_num(hw, bufsize)), y asi un
 * evento de fin cae justo con un descriptor lleno.  device/audio/audioIn.c
 * enlaza descriptores de este tamano por lo mismo. */
#define I2S_RX_EOF_BYTES    2048U

bool i2s_rx_start(void);
bool i2s_rx_stop(void);
bool i2s_rx_started(void);
bool i2s_rx_running(void);

#endif                          /* _AUDIO_I2S_H_ */
