/**
 * @file mqueue.h
 *
 * Colas de mensajes de tamaño variable.  A diferencia de mailbox (que solo
 * transporta un int), cada mensaje lleva su longitud y se copia.
 *
 * No sustituye a mailbox: es una API paralela.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _MQUEUE_H_
#define _MQUEUE_H_

#include <conf.h>
#include <stdint.h>

#define MQ_MAXSLOTS 16          /**< máximo de mensajes en una cola       */
#define MQ_MAXMSG   256         /**< máximo de bytes por mensaje          */

typedef uint mqueue;

/** Crea una cola con nslots mensajes de hasta maxmsg bytes.  Memoria PSRAM. */
mqueue mqcreate(uint nslots, uint maxmsg);

/** Encola una copia de buf[0..len).  Bloquea si la cola está llena. */
syscall mqsend(mqueue q, const void *buf, uint len);

/** Desencola en buf (capacidad cap).  Bloquea si está vacía.  Devuelve len. */
int mqreceive(mqueue q, void *buf, uint cap);

/** Igual que mqreceive pero no bloquea; SYSERR si no hay mensaje. */
int mqtryreceive(mqueue q, void *buf, uint cap);

/** Libera la cola (debe estar vacía y sin esperas). */
syscall mqfree(mqueue q);

/** Número de mensajes encolados, o SYSERR si q no es válida. */
int mqcount(mqueue q);

#endif                          /* _MQUEUE_H_ */
