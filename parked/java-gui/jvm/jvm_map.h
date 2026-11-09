/*
 * jvm_map.h — the libc calls the JVM core uses, redirected to the platform
 * seam (jvm_io.h).  Include this from the portable interpreter sources
 * (bytecodes.c, the pool parsers, heap.c, stack.c, free_pool.c, java_lib.c,
 * jvm_run.c) and NOT from a platform implementation file.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef SIMPLE_JVM_MAP_H
#define SIMPLE_JVM_MAP_H

#include "jvm_io.h"

/* stdio-style file I/O.  "FILE" itself becomes the opaque handle so the
 * parsers can keep their familiar `FILE *fp` spelling. */
#define FILE        JvmFile
#define fopen(p, m) jvm_open((p), (m))
#define fread(b, s, n, f) jvm_read((b), (int)(s), (int)(n), (f))
#define fclose(f)   jvm_close(f)
#define fseek(f, o, w) jvm_seek((f), (o), (w))

/* memory */
#define malloc(n)      jvm_alloc((n))
#define calloc(n, s)   jvm_calloc((n), (s))
#define realloc(p, n)  jvm_realloc((p), (n))
#define free(p)        jvm_free((p))

/* formatting + termination */
#define snprintf       jvm_snprintf
#define exit(c)        jvm_exit((c))

#ifndef SEEK_SET
#define SEEK_SET 0
#endif
#ifndef SEEK_CUR
#define SEEK_CUR 1
#endif
#ifndef SEEK_END
#define SEEK_END 2
#endif

#endif                          /* SIMPLE_JVM_MAP_H */
