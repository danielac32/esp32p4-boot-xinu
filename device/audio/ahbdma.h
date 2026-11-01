/**
 * @file ahbdma.h
 *
 * Interfaz del canal TX del motor AHB-DMA del ESP32-P4 (0x50085000), que es
 * el unico que puede alimentar el FIFO del I2S0: el bloque I2S no tiene FIFO
 * accesible por CPU, asi que sin este motor no hay forma de escribirle datos.
 *
 * El motor trabaja sobre descriptores encadenados en memoria; aqui solo vive
 * el canal (relojes, conexion con el periferico, arranque/parada del enlace y
 * estado).  Quien construye los descriptores es device/audio/audioOut.c en la
 * salida y device/audio/audioIn.c en la entrada.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _AHBDMA_H_
#define _AHBDMA_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * Soltar el canal TX 0 y ponerlo a dar vueltas por una lista de descriptores.
 *
 * @p first_desc es la direccion del primer descriptor TAL Y COMO LA VE EL
 * motor (la forma con cache, 0x48...... en PSRAM): la ventana del AHB-DMA no
 * acepta el acceso directo del CPU.  Los descriptores ya tienen su bit de
 * propiedad puesto a DMA antes de llamar aqui.
 *
 * @param first_desc direccion de bus del primer descriptor
 * @return true si el enlace quedo en marcha
 */
bool ahbdma_tx_link(uint32_t first_desc);

/** Parar el enlace y resetear el FSM del canal. */
void ahbdma_tx_stop(void);

/** true mientras el enlace trabaja (la lista esta enlazada y el FSM no esta
 *  aparcado). */
bool ahbdma_tx_running(void);

/**
 * Bits crudos del canal, sin limpiarlos: bit0 done, bit1 eof, bit2 error de
 * descriptor, bit3 total eof.  Sirven para saber si el motor llego a leer la
 * lista o si la rechazo.
 */
uint32_t ahbdma_tx_raw(void);

/** Limpiar los bits crudos que se indiquen. */
void ahbdma_tx_clear(uint32_t mask);

/** Direccion del descriptor que el motor esta procesando ahora mismo. */
uint32_t ahbdma_tx_current_desc(void);

/* ---- mitad IN del canal 0: la captura del microfono -----------------
 *
 * Misma lista ciclica y mismos registros de mando, pero sin comprobacion de
 * dueno, que es como la monta el RX de referencia: el motor recorre la lista
 * sin preguntar y reescribe el trozo mas antiguo cuando el consumidor no llega
 * al ritmo del reloj.  Con la comprobacion activada, en placa el FSM se aparcaba
 * tras el primer descriptor y habia que reanimarlo a mano en cada vuelta, que no
 * es el modelo del driver de referencia ni sirve para un streaming continuo.
 *
 * La posicion de la escritura la da in_suc_eof_des_addr, el ultimo descriptor
 * cuyo buffer quedo cerrado por un fin de recepcion; el RX del P4 no tiene el
 * contador de sincronizacion del TX, asi que no hay nada mas que mirar.
 */

/** Enlazar y soltar el canal IN 0 sobre la primera descripcion. */
bool ahbdma_in_link(uint32_t first_desc);

/** Parar el enlace de entrada y resetear el canal. */
void ahbdma_in_stop(void);

/** true mientras el enlace de entrada trabaja. */
bool ahbdma_in_running(void);

/** Bits crudos del canal IN: done[0] suc_eof[1] err_eof[2] dscr_err[3] dscr_empty[4]. */
uint32_t ahbdma_in_raw(void);

/** Limpiar los bits crudos que se indiquen. */
void ahbdma_in_clear(uint32_t mask);

/** Descriptor que el motor esta procesando ahora mismo. */
uint32_t ahbdma_in_current_desc(void);

/** Descriptor cuyo buffer acabo de llenar; la posicion de la escritura DMA. */
uint32_t ahbdma_in_eof_desc(void);

/** Palabras de 4 bytes retenidas en el FIFO de recepcion del canal
 *  (in_infifo_status[7:2]): dice si el atasco esta en la memoria (nivel alto y
 *  quieto) o en el periferico (nivel casi siempre a cero). */
uint32_t ahbdma_in_fifo_words(void);

/** Valor crudo de in_state: direccion del descriptor en curso en [17:0] y la
 *  maquina de estados del enlace en [22:20].  Solo diagnostico. */
uint32_t ahbdma_in_state(void);

#endif                          /* _AHBDMA_H_ */
