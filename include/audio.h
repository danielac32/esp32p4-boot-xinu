/**
 * @file audio.h
 *
 * Device `audio`: el codec ES8311 del bus I2C0 compartido, los relojes I2S0 y
 * el flujo PCM que sale y entra por ellos.  Cubre el control por I2C (reloj,
 * formato, volumen, mute, ganancia del micro), la puesta en marcha del timing
 * I2S maestro (MCLK/BCLK/LRCK), la etapa de salida --un anillo de muestras en
 * PSRAM que el AHB-DMA repasa sin parar y entrega al FIFO TX del I2S-- y la de
 * entrada, con el FIFO RX del mismo bloque recogido por el canal IN del motor.
 *
 * El PCM va por read() y write() en el formato del cable: tramas estereofonicas
 * de dos slots de 16 bits a 48 kHz, 4 bytes por trama.  No hay cabeceras ni
 * decodificacion dentro del driver: un WAV o un PCM crudo se normalizan antes
 * de escribir (shell/xsh_audio.c), y los formatos con perdida (MP3, AAC, MP4)
 * necesitan un decodificador que este driver no trae.
 *
 * A diferencia del PMIC, escribir en el codec no puede tumbar la placa, y
 * sin escrituras no hay nada que hacer: por eso este device si escribe.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _AUDIO_H_
#define _AUDIO_H_

#include <device.h>
#include <device/audio/es8311.h>
#include <device/audio/i2s.h>
#include <device/audio/audioOut.h>
#include <device/audio/audioIn.h>

/* control(AUDIO0, CMD, arg1, arg2) */
#define AUDIO_CTRL_VOLUME     0x01 /**< arg1=0..100 -> valor leido de vuelta */
#define AUDIO_CTRL_MUTE       0x02 /**< arg1=0/1    -> OK o SYSERR           */
#define AUDIO_CTRL_GET_VOLUME 0x03 /**<               -> 0..100 o SYSERR     */
#define AUDIO_CTRL_MIC_GAIN   0x04 /**< arg1=0..7 (0..42 dB) -> OK o SYSERR  */
#define AUDIO_CTRL_GET_MUTE   0x05 /**<               -> 0/1 o SYSERR        */
#define AUDIO_CTRL_CLOCKS     0x06 /**< arg1=1 arrancar / 0 parar -> OK o SYSERR */
#define AUDIO_CTRL_BCLK       0x07 /**< arg1=microsegundos de medida -> Hz o SYSERR */
#define AUDIO_CTRL_GET_VOLUME_DB \
                       0x0E /**< -> decimas de dB del registro 0x32 o SYSERR  */

/* Etapa de salida (audioOut.c).  La frecuencia de AUDIO_CTRL_OUT se admite
 * tal cual solo si el patron cierra el bucle sin costura; si no, se programa
 * la mas proxima que si cierra y ese es el valor que devuelve. */
#define AUDIO_CTRL_OUT        0x08 /**< arg1=Hz pedidos -> Hz aplicados o SYSERR */
#define AUDIO_CTRL_OUT_OFF    0x09 /**<               -> OK (amplificador fuera) */
#define AUDIO_CTRL_OUT_STAT   0x0A /**< -> bits AUDIO_OUT_ST_*, o SYSERR         */
#define AUDIO_CTRL_OUT_DESC   0x0B /**< -> descriptor actual del DMA, 0 si no ha
                                    *   leido ninguno                             */
#define AUDIO_CTRL_OUT_CNT    0x0C /**< -> muestras de 16 bits sacadas por el I2S */
#define AUDIO_CTRL_OUT_RING   0x0D /**< -> frames del anillo que suena, 0 si parada */

/* Stream de salida (audioOut.c en modo stream).  El anillo pasa a PSRAM y se
 * rellena por write(); estos codigos lo montan, lo paran y dicen por donde va
 * la cabeza de escritura. */
#define AUDIO_CTRL_STREAM     0x0F /**< arg1=1 montar / 0 desmontar -> OK o SYSERR */
#define AUDIO_CTRL_STREAM_WAIT \
                        0x10 /**< -> esperar a que lo escrito salga por el I2S */
#define AUDIO_CTRL_OUT_QUEUE  0x11 /**< -> tramas escritas y aun dentro del DMA  */
#define AUDIO_CTRL_OUT_CAP    0x12 /**< -> capacidad del anillo en tramas, 0 si no
                                    *   hay stream montado                        */

/* Captura del microfono (audioIn.c).  El PCM se saca por read(); aqui solo se
 * monta, se para y se mira el estado de los registros. */
#define AUDIO_CTRL_IN         0x13 /**< arg1=1 montar / 0 parar -> OK o SYSERR    */
#define AUDIO_CTRL_IN_STAT    0x14 /**< -> bits AUDIO_IN_ST_*                      */
#define AUDIO_CTRL_IN_DESC    0x15 /**< -> descriptor actual del motor de entrada  */
#define AUDIO_CTRL_IN_DROP    0x16 /**< -> tramas perdidas por desbordar el anillo */
#define AUDIO_CTRL_IN_RING    0x17 /**< -> bytes del anillo de captura             */

/* Ruta del microfono dentro del codec (es8311.c).  El PGA es analogico
 * (0..10 = 0..+30 dB, 3 dB/paso) y se lee de vuelta del chip. */
#define AUDIO_CTRL_MIC_PGA    0x18 /**< arg1=0..10 -> OK o SYSERR                  */
#define AUDIO_CTRL_MIC_GET    0x19 /**< -> paso del PGA leido del codec o SYSERR   */
#define AUDIO_CTRL_MIC_STAT   0x1A /**< -> bits del estado del micro leido por I2C */
#define AUDIO_CTRL_MIC_ADCSV  0x1B /**< -> volumen digital del ADC leido (registro)*/
#define AUDIO_CTRL_MIC_SCALE  0x20 /**< -> escala digital del ADC (registro 0x16)  */

/* Direcciones de los anillos de PSRAM, en la forma que ve el AHB-DMA
 * (0x48......); el CPU los escribe por el acceso directo, 0x40000000 arriba. */
#define AUDIO_CTRL_STREAM_BASE \
                          0x1C /**< -> direccion del anillo de reproduccion */
#define AUDIO_CTRL_IN_BASE 0x1D /**< -> direccion del anillo de captura      */

/* Posicion del motor de captura sobre el anillo.  El RX del P4 no lleva
 * contador de sincronizacion, asi que la unica escritura que se puede seguir es
 * el descriptor que el motor acabo de cerrar (in_suc_eof_des_addr). */
#define AUDIO_CTRL_IN_EOF   0x1E /**< -> ultimo descriptor cerrado por el motor  */
#define AUDIO_CTRL_IN_QUEUE 0x1F /**< -> bytes capturados y aun sin entregar     */

/* Pruebas de ritmo de las dos etapas, leidas de los contadores del driver.
 * AUDIO_CTRL_IN_DROP cuenta los trozos de captura tirados por no llegar el
 * consumidor a tiempo y AUDIO_CTRL_STREAM_GAPS las vueltas que el motor de
 * salida dio por delante de quien escribia: ambas son la misma cosa al reves,
 * audio que se ha perdido. */
#define AUDIO_CTRL_STREAM_GAPS 0x21 /**< -> huecos de esta reproduccion          */

/* Driver functions */
devcall audioInit(device *);
devcall audioControl(device *, int, long, long);
devcall audioRead(device *, void *, uint32);
devcall audioWrite(device *, const void *, uint32);

#endif                          /* _AUDIO_H_ */
