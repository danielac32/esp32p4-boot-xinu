/*
 * jvm_io.h — platform seam for the Simple JVM core (p4-sdk).
 *
 * The interpreter (bytecodes.c, the pool parsers, heap.c, stack.c,
 * free_pool.c, java_lib.c, jvm_run.c) is shared between the host test build
 * and the ESP32-P4 target.  It never calls libc directly: it goes through
 * the small set of primitives declared here.  Each platform provides one
 * implementation file:
 *
 *     host   : jvm/jvm_io_host.c    (FILE*, malloc, vsnprintf, exit)
 *     target : device/jvm/jvm_io_target.c (fl_* FAT, malloc/free, kill task)
 *
 * The core translation units include "jvm_map.h" right after the system
 * headers; that header turns the familiar names (FILE, fopen, fread, fclose,
 * fseek, malloc, calloc, realloc, free, snprintf, exit) into these calls, so
 * the interpreter source stays readable and unchanged in spirit.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef SIMPLE_JVM_IO_H
#define SIMPLE_JVM_IO_H

#include <stddef.h>
#include <stdbool.h>

/* Opaque file handle: a FILE* on the host, a FAT handle on the P4. */
typedef void JvmFile;

JvmFile *jvm_open(const char *path, const char *mode);
int jvm_read(void *buf, int size, int count, JvmFile *f);
void jvm_close(JvmFile *f);
int jvm_seek(JvmFile *f, long offset, int whence);

void *jvm_alloc(size_t size);
void *jvm_calloc(size_t nmemb, size_t size);
void *jvm_realloc(void *ptr, size_t size);
void jvm_free(void *ptr);

/* Bounded formatting.  The target implementation must handle at least
 * %d, %u, %x, %c, %s, %g and %% (the core formats object ids, integers
 * and floats with it). */
int jvm_snprintf(char *buf, size_t n, const char *fmt, ...);

/* Unrecoverable VM error: the host exits, the target kills its task.
 * Never returns. */
void jvm_exit(int code) __attribute__((noreturn));

/* ---- per-instance VM state (reentrancy) -------------------------------
 * All interpreter state (pools, locals, heap, statics, call depth) lives
 * in a struct _JvmState (see simple_jvm.h).  These accessors give each OS
 * thread the state it is currently running:
 *
 *     host   : a single global pointer (single-threaded tests)
 *     target : a per-tid table, so concurrent VM tasks never share state
 */
struct _JvmState;
typedef struct _JvmState Jvm;

Jvm *jvm_current_get(void);
void jvm_current_set(Jvm *st);

/* Tope (direccion mas alta) de la pila de la tarea que llama: gc.c lo usa
 * como limite del barrido conservador de la pila C en el barrido de emergencia.
 * Devuelve 0 si la plataforma no lo puede decir, y entonces solo se pierde
 * precision (ningun mal). */
void *jvm_stack_top(void);

#endif                          /* SIMPLE_JVM_IO_H */
