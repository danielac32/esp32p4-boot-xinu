/**
 * @file xsh_ktest.c
 *
 * Pruebas de humo del kernel: mutex no recursivo, mqueue de tamaño variable
 * y escritura/lectura real sobre el sistema de ficheros.
 *
 *   ktest [all|mutex|mq|fat]
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <kernel.h>
#include <thread.h>
#include <interrupt.h>
#include <shell.h>
#include <mutex.h>
#include <mqueue.h>
#include <memory.h>
#include <fsutil.h>

static volatile int mtx_counter;
static volatile int ktest_done;
static mutex test_mtx;

static thread mtx_worker(void)
{
    int i;

    for (i = 0; i < 1000; i++)
    {
        if (OK == mtxlock(test_mtx))
        {
            int v = mtx_counter;
            v++;
            mtx_counter = v;
            mtxunlock(test_mtx);
        }
    }

    {
        irqmask im = disable();
        ktest_done++;
        restore(im);
    }

    return 0;
}

static int run_children(thread (*proc)(void), int n)
{
    tid_typ tid[SHELL_MAXTOK];
    int i;

    ktest_done = 0;

    for (i = 0; i < n; i++)
    {
        tid[i] = create((void *)proc, SHELL_CMDSTK, SHELL_CMDPRIO,
                        "ktest", 0);
        if (SYSERR == tid[i])
        {
            return SYSERR;
        }
    }

    for (i = 0; i < n; i++)
    {
        ready(tid[i], RESCHED_NO);
    }

    /* No se puede esperar un mensaje por hijo: send() solo guarda uno por
     * tarea y dos muertes casi simultáneas perderían el segundo.  El
     * contador compartido es la fuente de verdad; los mensajes de kill() se
     * drenan al final. */
    {
        int waited = 0;

        while (ktest_done < n && waited < 5000)
        {
            sleep(10);
            waited += 10;
        }

        if (ktest_done < n)
        {
            return SYSERR;
        }
    }

    while (recvclr() != NOMSG)
    {
        ;
    }

    return OK;
}

static int test_mutex(void)
{
    mutex m;
    int rc = OK;

    m = mtxcreate();
    if (SYSERR == m)
    {
        printf("mutex: mtxcreate falló\n");
        return SYSERR;
    }

    if (OK != mtxlock(m))
    {
        printf("mutex: primer lock falló\n");
        rc = SYSERR;
    }
    else
    {
        if (SYSERR != mtxlock(m))
        {
            printf("mutex: el doble lock NO devolvió SYSERR\n");
            rc = SYSERR;
        }
        if (OK != mtxunlock(m))
        {
            printf("mutex: unlock falló\n");
            rc = SYSERR;
        }
    }

    if (OK != mtxfree(m))
    {
        printf("mutex: mtxfree falló\n");
        rc = SYSERR;
    }

    mtx_counter = 0;
    test_mtx = mtxcreate();
    if (SYSERR == test_mtx)
    {
        printf("mutex: segundo mtxcreate falló\n");
        return SYSERR;
    }

    if (SYSERR == run_children(mtx_worker, 2))
    {
        printf("mutex: no se pudieron crear los hilos\n");
        rc = SYSERR;
    }
    else if (mtx_counter != 2000)
    {
        printf("mutex: contador=%d (esperado 2000)\n", mtx_counter);
        rc = SYSERR;
    }

    mtxfree(test_mtx);

    printf("mutex: %s (contador=%d)\n", (rc == OK) ? "OK" : "FAIL",
           mtx_counter);

    return rc;
}

struct kmsg
{
    int seq;
    char text[32];
};

static int test_mqueue(void)
{
    mqueue q;
    struct kmsg in;
    struct kmsg out;
    int i;
    int rc = OK;

    q = mqcreate(4, sizeof(struct kmsg));
    if (SYSERR == q)
    {
        printf("mq: mqcreate falló\n");
        return SYSERR;
    }

    for (i = 0; i < 20; i++)
    {
        memset(&in, 0, sizeof(in));
        in.seq = i;
        sprintf(in.text, "msg-%d", i);

        if (OK != mqsend(q, &in, sizeof(in)))
        {
            printf("mq: send %d falló\n", i);
            rc = SYSERR;
            break;
        }

        if ((int)sizeof(out) != mqreceive(q, &out, sizeof(out)))
        {
            printf("mq: receive %d falló\n", i);
            rc = SYSERR;
            break;
        }

        if (out.seq != i || strcmp(out.text, in.text) != 0)
        {
            printf("mq: mensaje %d corrupto\n", i);
            rc = SYSERR;
            break;
        }
    }

    if (mqcount(q) != 0)
    {
        printf("mq: count=%d (esperado 0)\n", mqcount(q));
        rc = SYSERR;
    }

    if (SYSERR != mqtryreceive(q, &out, sizeof(out)))
    {
        printf("mq: mqtryreceive en cola vacía no devolvió SYSERR\n");
        rc = SYSERR;
    }

    if (OK != mqfree(q))
    {
        printf("mq: mqfree falló\n");
        rc = SYSERR;
    }

    printf("mq: %s\n", (rc == OK) ? "OK" : "FAIL");
    return rc;
}

/* Tamaño de clúster del volumen: 32 KiB.  Escribir 33 KiB obliga a la
 * librería a asignar un segundo clúster y a escribir la FAT, que es lo que
 * los comandos de shell (touch/mkdir/mv/rm, metadatos vacíos) no ejercitan.
 * Los búferes son de un sector: el stack del comando no da para más. */
#define FATTEST_CHUNK   512
#define FATTEST_TOTAL   33792U
#define FATTEST_PATH    "/ktest.fat"

static uint8 fattest_pattern(unsigned i)
{
    return (uint8)((i * 31U + 7U) ^ (i >> 8));
}

/**
 * Write >1 cluster, read it back and compare, then append a sector and
 * check the grew size.  Leaves the volume as it found it (file removed).
 */
static int test_fat_io(void)
{
    unsigned char buf[FATTEST_CHUNK];
    void *f;
    unsigned off, i;
    unsigned long bad = 0, first_bad = 0;
    long size;
    int rc = OK;

    if (!fs_mounted())
    {
        printf("fat: no hay volumen montado\n");
        return SYSERR;
    }

    /* --- escribir FATTEST_TOTAL bytes ------------------------------- */
    fl_remove(FATTEST_PATH);
    f = fl_fopen(FATTEST_PATH, "w");
    if (f == NULL)
    {
        printf("fat: fopen(\"%s\", \"w\") falló\n", FATTEST_PATH);
        return SYSERR;
    }

    for (off = 0; off < FATTEST_TOTAL; off += FATTEST_CHUNK)
    {
        for (i = 0; i < FATTEST_CHUNK; i++)
        {
            buf[i] = fattest_pattern(off + i);
        }
        if (fl_fwrite(buf, 1, FATTEST_CHUNK, f) != FATTEST_CHUNK)
        {
            printf("fat: fwrite falló en el offset %u\n", off);
            rc = SYSERR;
            break;
        }
    }

    fl_fflush(f);
    fl_fclose(f);

    /* --- tamaño tal como lo ve el directorio -------------------------- */
    f = fl_fopen(FATTEST_PATH, "r");
    if (f == NULL)
    {
        printf("fat: el fichero no sobrevivió al cierre\n");
        return SYSERR;
    }

    fl_fseek(f, 0, SEEK_END);
    size = fl_ftell(f);
    if (size != (long)FATTEST_TOTAL)
    {
        printf("fat: tamaño=%ld (esperado %u)\n", size, FATTEST_TOTAL);
        rc = SYSERR;
    }

    /* --- leer de vuelta y comparar ----------------------------------- */
    fl_fseek(f, 0, SEEK_SET);
    for (off = 0; rc == OK && off < FATTEST_TOTAL; off += FATTEST_CHUNK)
    {
        if (fl_fread(buf, 1, FATTEST_CHUNK, f) != FATTEST_CHUNK)
        {
            printf("fat: fread se cortó en el offset %u\n", off);
            rc = SYSERR;
            break;
        }
        for (i = 0; i < FATTEST_CHUNK; i++)
        {
            if (buf[i] != fattest_pattern(off + i))
            {
                if (bad == 0)
                {
                    first_bad = off + i;
                }
                bad++;
            }
        }
    }
    fl_fclose(f);

    if (bad != 0)
    {
        printf("fat: %lu byte(s) mal, primero en %lu\n", bad, first_bad);
        rc = SYSERR;
    }

    /* --- append: la librería debe extender el mismo clúster/cadena ---- */
    if (rc == OK)
    {
        f = fl_fopen(FATTEST_PATH, "a");
        if (f == NULL)
        {
            printf("fat: fopen(\"a\") falló\n");
            return SYSERR;
        }
        for (i = 0; i < FATTEST_CHUNK; i++)
        {
            buf[i] = fattest_pattern(FATTEST_TOTAL + i);
        }
        if (fl_fwrite(buf, 1, FATTEST_CHUNK, f) != FATTEST_CHUNK)
        {
            printf("fat: fwrite en append falló\n");
            rc = SYSERR;
        }
        fl_fflush(f);
        fl_fclose(f);

        f = fl_fopen(FATTEST_PATH, "r");
        fl_fseek(f, 0, SEEK_END);
        size = fl_ftell(f);
        fl_fclose(f);

        if (size != (long)(FATTEST_TOTAL + FATTEST_CHUNK))
        {
            printf("fat: tamaño tras append=%ld (esperado %u)\n", size,
                   FATTEST_TOTAL + FATTEST_CHUNK);
            rc = SYSERR;
        }
        else
        {
            printf("fat: %u B + %u B append verificados byte a byte\n",
                   FATTEST_TOTAL, FATTEST_CHUNK);
        }
    }

    if (fl_remove(FATTEST_PATH) != 0)
    {
        printf("fat: no se pudo borrar %s\n", FATTEST_PATH);
        rc = SYSERR;
    }

    printf("fat: %s\n", (rc == OK) ? "OK" : "FAIL");
    return rc;
}

/* El filesystem lo van a usar a la vez el shell y alguna tarea (el arranque,
 * más adelante).  Cada tarea escribe su propio fichero, lo lee y lo compara:
 * si los tres candados (librería FAT -> tarjeta -> bus SDMMC) no serializan
 * bien, las cadenas de clústeres se pisan y la comparación falla o el
 * fopen de lectura no encuentra nada. */
#define FATMT_THREADS   4
#define FATMT_CHUNK     512
#define FATMT_TOTAL     4096U

static volatile int fatmt_done;
static volatile int fatmt_fail;

static uint8 fattest_mt_pattern(int seed, unsigned i)
{
    return (uint8)((i * 31U + (unsigned)seed * 131U) ^ (i >> 8));
}

static thread fatmt_worker(int seed)
{
    unsigned char buf[FATMT_CHUNK];
    char path[24];
    void *f;
    unsigned off, i;
    int bad = 0;

    sprintf(path, "/mt%d.fat", seed);

    /* Deja el nombre libre si una ejecución anterior se quedó a medias:
     * la librería no trunca con "w" sobre un nombre que ya existe. */
    fl_remove(path);

    f = fl_fopen(path, "w");
    if (f == NULL)
    {
        bad++;
    }
    else
    {
        for (off = 0; off < FATMT_TOTAL; off += FATMT_CHUNK)
        {
            for (i = 0; i < FATMT_CHUNK; i++)
            {
                buf[i] = fattest_mt_pattern(seed, off + i);
            }
            if (fl_fwrite(buf, 1, FATMT_CHUNK, f) != FATMT_CHUNK)
            {
                bad++;
                break;
            }
        }
        fl_fflush(f);
        fl_fclose(f);
    }

    f = fl_fopen(path, "r");
    if (f == NULL)
    {
        bad++;
    }
    else
    {
        fl_fseek(f, 0, SEEK_END);
        if (fl_ftell(f) != (long)FATMT_TOTAL)
        {
            bad++;
        }
        fl_fseek(f, 0, SEEK_SET);
        for (off = 0; off < FATMT_TOTAL; off += FATMT_CHUNK)
        {
            if (fl_fread(buf, 1, FATMT_CHUNK, f) != FATMT_CHUNK)
            {
                bad++;
                break;
            }
            for (i = 0; i < FATMT_CHUNK; i++)
            {
                if (buf[i] != fattest_mt_pattern(seed, off + i))
                {
                    bad++;
                }
            }
        }
        fl_fclose(f);
    }

    if (fl_remove(path) != 0)
    {
        bad++;
    }

    {
        irqmask im = disable();
        if (bad != 0)
        {
            fatmt_fail++;
        }
        fatmt_done++;
        restore(im);
    }
    return 0;
}

static int test_fat_mt(void)
{
    tid_typ tid[FATMT_THREADS];
    int i;
    int waited = 0;

    if (!fs_mounted())
    {
        printf("fatmt: no hay volumen montado\n");
        return SYSERR;
    }

    fatmt_done = 0;
    fatmt_fail = 0;

    for (i = 0; i < FATMT_THREADS; i++)
    {
        tid[i] = create((void *)fatmt_worker, SHELL_CMDSTK, SHELL_CMDPRIO,
                        "ktestfat", 1, i);
        if (SYSERR == tid[i])
        {
            printf("fatmt: no se pudo crear la tarea %d\n", i);
            return SYSERR;
        }
    }

    for (i = 0; i < FATMT_THREADS; i++)
    {
        ready(tid[i], RESCHED_NO);
    }

    while (fatmt_done < FATMT_THREADS && waited < 30000)
    {
        sleep(10);
        waited += 10;
    }

    for (i = 0; i < FATMT_THREADS; i++)
    {
        recvclr();
    }

    if (fatmt_done < FATMT_THREADS)
    {
        printf("fatmt: %d de %d tareas terminaron\n", fatmt_done,
               FATMT_THREADS);
        printf("fatmt: FAIL\n");
        return SYSERR;
    }

    printf("fatmt: %d tareas, %u B cada una (%d con errores)\n",
           FATMT_THREADS, FATMT_TOTAL, fatmt_fail);
    printf("fatmt: %s\n", (fatmt_fail == 0) ? "OK" : "FAIL");
    return (fatmt_fail == 0) ? OK : SYSERR;
}

/**
 * @ingroup shell
 *
 * Shell command (ktest).
 */
shellcmd xsh_ktest(int nargs, char *args[])
{
    int rc = 0;

    if (nargs >= 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s [all|mutex|mq|fat|fatmt]\n", args[0]);
        return 0;
    }

    if (nargs == 1 || strcmp(args[1], "all") == 0)
    {
        if (SYSERR == test_mutex()) rc = 1;
        if (SYSERR == test_mqueue()) rc = 1;
        if (SYSERR == test_fat_io()) rc = 1;
        if (SYSERR == test_fat_mt()) rc = 1;
        return rc;
    }

    if (strcmp(args[1], "mutex") == 0)
    {
        return (SYSERR == test_mutex()) ? 1 : 0;
    }

    if (strcmp(args[1], "mq") == 0)
    {
        return (SYSERR == test_mqueue()) ? 1 : 0;
    }

    if (strcmp(args[1], "fat") == 0)
    {
        return (SYSERR == test_fat_io()) ? 1 : 0;
    }

    if (strcmp(args[1], "fatmt") == 0)
    {
        return (SYSERR == test_fat_mt()) ? 1 : 0;
    }

    fprintf(stderr, "%s: opción desconocida '%s'\n", args[0], args[1]);
    return 1;
}