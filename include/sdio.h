/**
 * @file sdio.h
 *
 * Interfaz pública de la sonda SDIO del ESP32-C6 sobre el slot 1 del
 * controlador SDMMC compartido (W1).  El cuerpo está en device/sdio-esp32p4
 * y reutiliza el motor de comandos del host del slot 0 (sdHw.c) bajo el
 * candado del bus.  Esto no es todavía un device del devtab: es la prueba de
 * que el enlace físico y la enumeración SDIO funcionan contra el slave
 * esp_hosted del C6.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _SDIODRV_H_
#define _SDIODRV_H_

#include <stddef.h>                     /* uint8/uint16/uint32 + bool         */

/* Por qué falló la última transacción SDIO (tag del host en el nibble alto),
 * reutilizando el mismo registro que el device SD.  Definido en sdHw.c. */
extern uint32 sd_hwerr;

/** Resultado de enumerar el C6: todo lo que sigue se leyó del periférico. */
struct sdioinfo
{
    bool     up;            /* TRUE si el C6 respondió y quedó seleccionado */
    uint32   ocr;           /* R4 del CMD5                                   */
    uint32   rca;           /* dirección relativa asignada por CMD3          */
    uint8    cio_enable;    /* CCCR 0x02 tras habilitar la función 1         */
    uint8    cio_ready;     /* CCCR 0x03 (bit1 = fn1 lista)                  */
    uint8    sdio_ver;      /* CCCR 0x08                                     */
    uint8    fn0_info[8];   /* CCCR 0x0C..0x13: región de identificación      */
    uint16   blksize;       /* CCCR 0x10/0x11: tamaño de bloque de fn0         */
    uint32   clk_khz;       /* reloj de tarjeta del host                     */
};

/** Enumerar el C6 en el slot 1.  Devuelve OK o SYSERR; rellena \p out. */
int sdioEnumerate(struct sdioinfo *out);

/**
 * Igual pero fijando el reloj del slot AL DIVISOR \p card_div inmediatamente
 * despues del reset del C6, dentro de la unica ventana en la que el
 * controlador admite un "update clock": antes de que el esclavo afirme DAT0.
 * \p trace vuelca el estado del bus paso a paso (se apaga en los reintentos).
 */
int sdioEnumerateAt(struct sdioinfo *out, uint8 card_div, bool trace);

/** Imprimir el resultado por la consola. */
void sdioProbeReport(const struct sdioinfo *info);

/**
 * Retener al C6 en reset (CHIP_PU abajo) y salir.  Se usa al final de la sonda
 * del transporte: con el slave retenido sus pads del slot 1 dejan de conducir,
 * y asi se puede comprobar si el DAT0 que el C6 afirma con una interrupcion
 * pendiente es lo que inhibe el camino de datos COMPARTIDO del bloque, es decir
 * si ese es el motivo de que la microSD del slot 0 quede muerta.
 */
void sdioC6HoldReset(void);

/**
 * Sonda del transporte de la funcion 1 (W2.1): levanta la funcion 1 reusando
 * la enumeracion W1 y ejercita el camino de datos (CMD53/CMD52) contra los
 * registros del slave esp_hosted, imprimiendo los valores leidos del propio
 * periférico.  Devuelve OK si la funcion 1 respondio.
 */
int sdioFn1TransportProbe(void);

/**
 * Volcado de diagnóstico del transporte esp_hosted (W2.c), al final de
 * device/sdio-esp32p4/espHostedSdio.c.  Solo lee: reloj padre compartido,
 * CLKDIV/CLKENA/CLKSRC/STATUS del bloque (los dos slots en el mismo
 * registro), las direcciones cached/sin-cache de las paginas que usa su DMA y
 * tres registros de la funcion 1 leidos con CMD53.  Sirve para comprobar que
 * levantar el C6 no movio nada del slot 0.
 */
void esp_hosted_sdio_diag_print(void);

#endif                          /* _SDIODRV_H_ */
