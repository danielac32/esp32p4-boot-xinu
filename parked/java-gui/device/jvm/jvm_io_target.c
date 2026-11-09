/*
 * jvm_io_target.c — platform seam for the ESP32-P4 (device/jvm).
 *
 * Replaces the host C library behind the JVM core (see jvm_io.h):
 *
 *   files   -> the FAT library (fl_*) on the microSD
 *   memory  -> the PSRAM heap (psram_*); malloc() here is the L2MEM heap and
 *              a Jvm instance does not fit in it
 *   abort   -> kill the running task (never return)
 *   current -> a per-tid table, so concurrent VM tasks keep separate state
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <kernel.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <thread.h>
#include <memory.h>
#include <fat_filelib.h>

#include "jvm_io.h"

/* Instancia de VM que ejecuta cada tarea (indice = tid).  Cada jvmTask
 * fija la suya al arrancar y la limpia al terminar; asi dos tareas pueden
 * ejecutar Java a la vez sin compartir pools/heap/locals. */
static Jvm *jvm_slots[NTHREAD];

Jvm *jvm_current_get(void)
{
    return jvm_slots[thrcurrent];
}

void jvm_current_set(Jvm *st)
{
    jvm_slots[thrcurrent] = st;
}

/* Tope de la pila de la tarea actual, para el barrido conservador del GC de
 * emergencia (jvm/gc.c).  La region de la pila es [stkbase, stkbase+stklen):
 * stkbase es la cota baja y el SP arranca en el tope y baja. */
void *jvm_stack_top(void)
{
    tid_typ tid = gettid();

    if (isbadtid(tid))
        return 0;
    return (char *) thrtab[tid].stkbase + thrtab[tid].stklen;
}

/* ---- ficheros (FAT sobre microSD) ---------------------------------- */

JvmFile *jvm_open(const char *path, const char *mode)
{
    return (JvmFile *) fl_fopen(path, mode);
}

int jvm_read(void *buf, int size, int count, JvmFile *f)
{
    return fl_fread(buf, size, count, (void *) f);
}

void jvm_close(JvmFile *f)
{
    fl_fclose((void *) f);
}

int jvm_seek(JvmFile *f, long offset, int whence)
{
    return fl_fseek((void *) f, offset, whence);
}

/* ---- memoria (el heap de PSRAM) ------------------------------------
 * En el P4 malloc() vive sobre el heap de L2MEM (223 KB libres medidos con
 * `syscheck mem'), y un `Jvm' entero son ~559 KB (cls_slots[256] cuesta
 * 265 KB, code_array 131 KB, las cinco pilas de ranuras 82 KB, heap_slots
 * 66 KB).  Asi que la VM va al segundo heap --psram_malloc, 33 MB libres--,
 * igual que en el S3 pero con la API explícita que el P4 impone. */

void *jvm_alloc(size_t size)
{
    return psram_malloc(size);
}

void *jvm_calloc(size_t nmemb, size_t size)
{
    return psram_calloc(nmemb, size);
}

void *jvm_realloc(void *ptr, size_t size)
{
    return psram_realloc(ptr, size);
}

void jvm_free(void *ptr)
{
    psram_free(ptr);
}

/* ---- formato mínimo (subconjunto de printf) ------------------------
 * El núcleo sólo usa %d y %g; se cubren también %u/%x/%c/%s/%% para que
 * las natives del target puedan formatear sin depender de libc. */

struct jfmt
{
    char *buf;
    size_t cap;
    size_t len;
};

static void jf_putc(struct jfmt *f, char c)
{
    if (f->len + 1 < f->cap)
    {
        f->buf[f->len] = c;
    }
    f->len++;
}

static void jf_puts(struct jfmt *f, const char *s)
{
    while (*s != '\0')
    {
        jf_putc(f, *s++);
    }
}

static void jf_putu(struct jfmt *f, unsigned int v, int base)
{
    char tmp[12];
    int i = 0;

    if (v == 0)
    {
        tmp[i++] = '0';
    }
    while (v != 0)
    {
        int d = (int) (v % (unsigned int) base);
        tmp[i++] = (char) ((d < 10) ? ('0' + d) : ('a' + d - 10));
        v /= (unsigned int) base;
    }
    while (i > 0)
    {
        jf_putc(f, tmp[--i]);
    }
}

/* Aproximación a %g: parte entera + hasta `prec` decimales, sin ceros
 * finales ni punto si es entero.  Suficiente para los valores que maneja
 * la VM (ids y floats de las apps). */
static void jf_putg(struct jfmt *f, double v, int prec)
{
    unsigned int ip;
    double frac;
    char fb[12];
    int i;
    int n = 0;
    int neg = 0;

    if (v < 0.0)
    {
        neg = 1;
        v = -v;
    }
    if (prec > (int) sizeof(fb))
    {
        prec = (int) sizeof(fb);
    }
    ip = (unsigned int) v;
    frac = v - (double) ip;

    /* redondeo de la parte fraccionaria */
    for (i = 0; i < prec; i++)
    {
        int d;
        frac *= 10.0;
        d = (int) frac;
        fb[n++] = (char) d;
        frac -= (double) d;
    }
    if (frac >= 0.5 && n > 0)
    {
        for (i = n - 1; i >= 0; i--)
        {
            if (++fb[i] < 10)
            {
                break;
            }
            fb[i] = 0;
        }
        if (i < 0)
        {
            ip++;
        }
    }
    while (n > 0 && fb[n - 1] == 0)
    {
        n--;
    }

    if (neg && (ip != 0 || n != 0))
    {
        jf_putc(f, '-');
    }
    jf_putu(f, ip, 10);
    if (n > 0)
    {
        jf_putc(f, '.');
        for (i = 0; i < n; i++)
        {
            jf_putc(f, (char) ('0' + fb[i]));
        }
    }
}

int jvm_snprintf(char *buf, size_t n, const char *fmt, ...)
{
    struct jfmt f;
    const char *p = fmt;
    va_list ap;

    f.buf = buf;
    f.cap = n;
    f.len = 0;

    va_start(ap, fmt);
    while (*p != '\0')
    {
        if (*p != '%')
        {
            jf_putc(&f, *p++);
            continue;
        }
        p++;
        switch (*p)
        {
        case 'd':
        {
            int v = va_arg(ap, int);
            if (v < 0)
            {
                jf_putc(&f, '-');
                jf_putu(&f, (unsigned int) (-v), 10);
            }
            else
            {
                jf_putu(&f, (unsigned int) v, 10);
            }
            break;
        }
        case 'u':
            jf_putu(&f, va_arg(ap, unsigned int), 10);
            break;
        case 'x':
            jf_putu(&f, va_arg(ap, unsigned int), 16);
            break;
        case 'c':
            jf_putc(&f, (char) va_arg(ap, int));
            break;
        case 's':
            jf_puts(&f, va_arg(ap, char *));
            break;
        case 'g':
        case 'f':
            jf_putg(&f, va_arg(ap, double), 6);
            break;
        case '%':
            jf_putc(&f, '%');
            break;
        default:
            jf_putc(&f, '%');
            if (*p != '\0')
            {
                jf_putc(&f, *p);
            }
            break;
        }
        if (*p != '\0')
        {
            p++;
        }
    }
    va_end(ap);

    if (n > 0)
    {
        size_t t = (f.len < n - 1) ? f.len : (n - 1);
        buf[t] = '\0';
    }
    return (int) f.len;
}

/* ---- terminación --------------------------------------------------- */

void jvm_exit(int code)
{
    printf("[jvm] abortado (codigo %d)\r\n", code);
    kill(thrcurrent);
    /* Si kill() no nos elimina (p.ej. fuera de tarea), no seguir -->
     * ejecutando bytecode: quedarse dormido indefinidamente. */
    for (;;)
    {
        sleep(1000U);
    }
}
