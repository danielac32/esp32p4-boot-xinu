/**
 * @file delay.h
 *
 * Encaje de nombres para el port del driver esp_hosted.  El SDK bare-metal
 * (core/delay/delay.h) define delay_ms()/delay_us() girando sobre el contador
 * MTIME del mtimer; Xinu ya publica exactamente esas dos firmas, con el mismo
 * comportamiento de espera ocupada, sobre su propio reloj del sistema
 * (declaraciones en include/timer.h, implementacion en system/timershim.c).
 *
 * Se mantiene el nombre del include del driver de referencia para no tener que
 * reescribir sus llamadas: "delay.h" -> los retardos ya probados de Xinu.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _DELAY_H_
#define _DELAY_H_

#include <timer.h>

#endif                          /* _DELAY_H_ */
