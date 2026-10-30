/**
 * @file xsh_ktest.c
 *
 * Pruebas de humo del kernel: mutex no recursivo, mqueue de tamaño variable.
 *
 *   ktest [all|mutex|mq]
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <thread.h>
#include <interrupt.h>
#include <shell.h>
#include <mutex.h>
#include <mqueue.h>
#include <memory.h>

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
        printf("Usage: %s [all|mutex|mq]\n", args[0]);
        return 0;
    }

    if (nargs == 1 || strcmp(args[1], "all") == 0)
    {
        if (SYSERR == test_mutex()) rc = 1;
        if (SYSERR == test_mqueue()) rc = 1;
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

    fprintf(stderr, "%s: opción desconocida '%s'\n", args[0], args[1]);
    return 1;
}