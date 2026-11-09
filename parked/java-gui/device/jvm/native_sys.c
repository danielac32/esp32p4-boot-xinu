/*
 * native_sys.c — natives de esp32p4.Sys en el ESP32-P4.
 *
 * Sys.run() crea una tarea jvmTask (la VM es reentrante: cada tarea Java
 * tiene su instancia) y espera a que termine, como hace el comando java
 * del shell.  El resto son llamadas directas al kernel de Xinu.  En el
 * host lo implementa jvm/esp32p4_sys.c (run ejecuta en línea).
 *
 * Convención de pila: el ÚLTIMO argumento está en la cima.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <kernel.h>
#include <stdio.h>
#include <string.h>
#include <thread.h>
#include <stack.h>

#include "simple_jvm.h"
#include "java_lib.h"
#include "jvm_io.h"

/* device/jvm/jvmglue.c */
extern thread jvmTask(char *path, int argc, char **argv);

#define JAVA_DIR "/java"

static void pop_text(StackFrame *stack, SimpleConstantPool *p, char *out, int max)
{
    jvm_pop_text(stack, p, out, max);
}

/* resuelve un elemento de String[] (REF del pool u OBJ del heap) a texto */
static void ref_to_text(SimpleConstantPool *p, int val, int type,
                        char *out, int max)
{
    out[0] = 0;
    if (type == STACK_ENTRY_OBJ)
    {
        if (heap_is_string(val))
        {
            const char *t = heap_string_text(val);
            int n = (int) strlen(t);
            if (n > max - 1)
            {
                n = max - 1;
            }
            memcpy(out, t, (size_t) n);
            out[n] = 0;
        }
    }
    else if (type == STACK_ENTRY_REF)
    {
        ConstantStringRef *s = findStringRef(p, val);
        if (s != 0)
        {
            getUTF8String(p, s->stringIndex, max - 1, out);
        }
    }
}

int esp32p4_sys_run(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char cls[128], path[192];
    char store[8][128];
    char *argv[8];
    int arrref, n, i, t, v, len;
    tid_typ child;

    (void) type;
    arrref = popInt(stack);                 /* String[] args (último) */
    pop_text(stack, p, cls, sizeof(cls));

    n = (arrref > 0) ? heap_array_length(arrref) : 0;
    if (n > 8)
    {
        n = 8;
    }
    for (i = 0; i < n; i++)
    {
        v = heap_ref_array_load(arrref, i, &t);
        ref_to_text(p, v, t, store[i], (int) sizeof(store[i]));
        argv[i] = store[i];
    }

    if (cls[0] == '/')
    {
        strlcpy(path, cls, sizeof(path));
    }
    else
    {
        len = (int) strlen(cls);
        if (len > 6 && strcmp(cls + len - 6, ".class") == 0)
        {
            sprintf(path, "%s/%s", JAVA_DIR, cls);
        }
        else
        {
            sprintf(path, "%s/%s.class", JAVA_DIR, cls);
        }
    }

    /* La VM necesita una pila holgada (JVM_STK), no la del llamante.
     * argv apunta a locales de este frame: siguen válidos mientras
     * esperamos en receive() a que el hijo termine. */
    child = create(jvmTask, JVM_STK, INITPRIO, "JVM", 3, path, n,
                   (n > 0) ? argv : 0);
    if (SYSERR == child)
    {
        pushInt(stack, 0);
        return 0;
    }

    while (recvclr() != NOMSG)
        ;
    ready(child, RESCHED_YES);

    /* Espera a que jvmTask termine (kill() envía el tid al padre). */
    while (receive() != child)
        ;

    pushInt(stack, 1);
    return 0;
}

/*
 * Sys.runHere(cls, args): ejecuta una clase en la tarea ACTUAL (sin crear
 * una tarea nueva).  Reusa jvm_run_path() (alloc/free de un Jvm state
 * separado) y guarda/restaura jvm_current.  Bloqueante: no retorna hasta
 * que la clase termine.  Permite que Workspace lance apps in-process.
 */
int esp32p4_sys_runHere(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char cls[128], path[192];
    char store[8][128];
    char *argv[8];
    int arrref, n, i, t, v, len;
    int saved;
    int rc;

    (void) type;
    arrref = popInt(stack);                 /* String[] args (ultimo) */
    pop_text(stack, p, cls, sizeof(cls));

    n = (arrref > 0) ? heap_array_length(arrref) : 0;
    if (n > 8)
        n = 8;
    for (i = 0; i < n; i++)
    {
        v = heap_ref_array_load(arrref, i, &t);
        ref_to_text(p, v, t, store[i], (int) sizeof(store[i]));
        argv[i] = store[i];
    }

    if (cls[0] == '/')
    {
        strlcpy(path, cls, sizeof(path));
    }
    else
    {
        len = (int) strlen(cls);
        if (len > 6 && strcmp(cls + len - 6, ".class") == 0)
            sprintf(path, "%s/%s", JAVA_DIR, cls);
        else
            sprintf(path, "%s/%s.class", JAVA_DIR, cls);
    }

    /* Ejecuta en la tarea actual: guarda el Jvm activo, corre, restaura. */
    saved = (int) jvm_current_get();
    rc = jvm_run_path(path, n, (n > 0) ? argv : 0);
    jvm_current_set((Jvm *) saved);

    pushInt(stack, (rc == OK) ? 1 : 0);
    return 0;
}

int esp32p4_sys_kill(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int tid = popInt(stack);

    (void) p;
    (void) type;
    pushInt(stack, (kill((tid_typ) tid) == OK) ? 1 : 0);
    return 0;
}

int esp32p4_sys_sleep(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int ms = popInt(stack);

    (void) p;
    (void) type;
    if (ms > 0)
    {
        sleep((uint) ms);
    }
    pushInt(stack, 1);
    return 0;
}

int esp32p4_sys_yield(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    (void) p;
    (void) type;
    yield();
    pushInt(stack, 1);
    return 0;
}

int esp32p4_sys_tid(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    (void) p;
    (void) type;
    pushInt(stack, (int) gettid());
    return 0;
}

/*
 * Sys.start(cls): lanza /java/<cls>.class como tarea y devuelve el tid sin
 * esperar.  Es la via del launcher: tiene que seguir vivo para pintar su
 * banner y atender el dedo mientras la app corre en frente.
 *
 * Sin argumentos: los argv del `run' bloqueante apuntan a los locales del
 * frame del nativo, que aqui moriria en cuanto retornamos.  Una app que
 * necesite datos del launcher los lee de la SD o se los manda con send().
 *
 * La ruta NO puede ser un local: setupStack() guarda el *valor* de cada
 * argumento (va_arg(ap, ulong)) en el contexto nuevo, no lo que apunta, asi
 * que un auto del nativo ya no existe cuando jvmTask() lo lee y la app abre
 * un fichero vacio.  Se usa un anillo de buffers estaticos; el hijo copia la
 * ruta a su propio buf al entrar, por lo que una ranura queda libre despues
 * de START_SLOT lanzamientos.
 */
#define START_SLOT 4
#define START_PATH_LEN 192

static char start_path[START_SLOT][START_PATH_LEN];
static int start_next;

int esp32p4_sys_start(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char cls[128];
    char *destino = start_path[start_next];
    int len;
    tid_typ child;

    (void) type;
    pop_text(stack, p, cls, sizeof(cls));

    if (cls[0] == '/')
    {
        strlcpy(destino, cls, START_PATH_LEN);
    }
    else
    {
        len = (int) strlen(cls);
        if (len > 6 && strcmp(cls + len - 6, ".class") == 0)
        {
            snprintf(destino, START_PATH_LEN, "%s/%s", JAVA_DIR, cls);
        }
        else
        {
            snprintf(destino, START_PATH_LEN, "%s/%s.class", JAVA_DIR, cls);
        }
    }
    start_next = (start_next + 1) % START_SLOT;

    /* create() deja la hebra suspendida (INITRET); hay que ponerla ready para
     * que arranque.  Con RESCHED_NO el launcher termina de pintar su transicion
     * antes de que la app le pise la pantalla. */
    child = create(jvmTask, JVM_STK, INITPRIO, "JVM", 3, destino, 0, 0);
    if (SYSERR == child)
    {
        pushInt(stack, 0);
        return 0;
    }
    ready(child, RESCHED_NO);
    pushInt(stack, (int) child);
    return 0;
}

/* Sys.send(tid, value): mensaje de una tarea java a otra (el launcher pide
 * cerrar, la app contestando se va ella sola y el kernel le limpia la UI). */
int esp32p4_sys_send(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int value = popInt(stack);
    int tid = popInt(stack);

    (void) p;
    (void) type;
    pushInt(stack, (send((tid_typ) tid, (int32) value) == OK) ? 1 : 0);
    return 0;
}

/* Sys.recv() -> valor del mensaje pendiente, o -1 si no hay ninguno.  No
 * bloquea: una app en primer plano no puede dormirse esperando al launcher. */
int esp32p4_sys_recv(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int32 msg;

    (void) p;
    (void) type;
    msg = recvclr();
    pushInt(stack, (msg == NOMSG) ? -1 : (int) msg);
    return 0;
}

/* Sys.parent() -> tid de la hebra que creo a esta.  create() lo apunta en
 * thrtab[], y como el launcher lanza cada app desde su propia hebra, el padre
 * de una app ES el workspace: asi la app le pide minimizar (MSG_HIDE) sin que
 * el kernel lleve la cuenta de quien es el launcher. */
int esp32p4_sys_parent(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    (void) p;
    (void) type;
    pushInt(stack, (int) thrtab[gettid()].parent);
    return 0;
}

/* Sys.stackPeak() / Sys.stackTotal() -> pico observado y tamano de la pila de
 * ESTA tarea, en bytes.  El launcher no puede verlos (el glue de la VM los
 * imprime al morir la clase, y eso solo sirve por consola); aqui los consume
 * la app en vivo, que es lo que necesita el monitor y lo que falta para
 * ajustar JVM_STK sin tener que cerrar la app. */
int esp32p4_sys_stackPeak(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    (void) p;
    (void) type;
    pushInt(stack, (int) stack_peak(gettid()));
    return 0;
}

int esp32p4_sys_stackTotal(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    (void) p;
    (void) type;
    pushInt(stack, (int) stack_total(gettid()));
    return 0;
}

/* Sys.alive(tid) -> 1 si la tarea sigue existiendo.  El launcher lo usa para
 * saber si una app en segundo plano ya se cerro sola. */
int esp32p4_sys_alive(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int tid = popInt(stack);

    (void) p;
    (void) type;
    if (tid < 0 || tid >= NTHREAD || THRFREE == thrtab[tid].state)
    {
        pushInt(stack, 0);
    }
    else
    {
        pushInt(stack, 1);
    }
    return 0;
}
