/**
 * @file stack.h
 *
 * Medición del uso de pila por tarea (high-water mark por SP).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _STACK_H_
#define _STACK_H_

#include <stdint.h>
#include <thread.h>

/* Anota el SP actual de una tarea en ejecución.  Lo llama resched() en cada
 * cambio de contexto; el pico es el máximo de (top - sp). */
void stack_note(tid_typ tid, uint32_t sp);

/** Uso máximo observado, en bytes (0 si la tarea nunca corrió). */
uint32_t stack_peak(tid_typ tid);

/** Tamaño total de la pila, en bytes. */
uint32_t stack_total(tid_typ tid);

/** Bytes de pila que nunca se usaron (total - peak). */
uint32_t stack_free(tid_typ tid);

/** TRUE si el SP observado cayó fuera de la pila. */
int stack_overflow(tid_typ tid);

/** Pone a cero el pico observado (para medir una carga concreta). */
void stack_reset(tid_typ tid);

/** Pone a cero los picos de todas las tareas. */
void stack_reset_all(void);

#endif                          /* _STACK_H_ */
