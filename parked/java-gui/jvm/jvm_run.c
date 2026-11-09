/*
 * jvm_run.c — instancia y arranque de una clase (compartido host / target).
 *
 * Todo el estado de una ejecucion vive en un `Jvm` (simple_jvm.h) asignado
 * por la plataforma (host: malloc; P4: PSRAM).  jvm_run_file() resetea el
 * estado, parsea el .class, corre <clinit> (si lo hay) y main().
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <string.h>
#include <kernel.h>
#include "simple_jvm.h"
#include "jvm_map.h"

/* Crea una instancia vacia (en PSRAM, en el target). */
Jvm *jvm_state_new(void)
{
    return (Jvm *) jvm_calloc(1, sizeof(Jvm));
}

/* Libera una instancia y todo lo que cuelga de ella (heap, pila de frames,
 * atributos de metodos). */
void jvm_state_free(Jvm *st)
{
    int i;

    if (st == 0)
        return;

    jvm_current_set(st);
    free_pools();               /* atributos de metodos */

    for (i = 1; i < JVM_HEAP_SLOTS; i++)
    {
        if (st->heap_slots[i].data != 0)
            jvm_free(st->heap_slots[i].data);
        if (st->heap_slots[i].text != 0)
            jvm_free(st->heap_slots[i].text);
    }
    if (st->stack.store != 0)
        jvm_free(st->stack.store);
    jvm_free(st);
}

/*
 * Carga y ejecuta una clase: resetea el estado, parsea @p path (un .class),
 * inicializa estaticos y ejecuta main(String[]).  Devuelve 0 si fue bien,
 * o <0 (fichero ilegible, sin main, ...).  El estado no se libera aqui.
 *
 * @p argc/@p argv son los argumentos de la linea de comandos (sin el nombre
 * de la clase).  main() SIEMPRE recibe un String[] no nulo (vacio si
 * argc == 0), como manda la JVM.
 */
int jvm_run_args(Jvm *st, char *path, int argc, char **argv)
{
    ClassFileFormat cff;
    ConstantClassRef *cls;
    MethodInfo *clinit;
    MethodInfo *entry;
    int i;

    if (st == 0)
        return -1;
    if (argc < 0 || (argc > 0 && argv == 0))
        argc = 0;

    jvm_current_set(st);

    memset(&cff, 0, sizeof(ClassFileFormat));
    memset(&st->cp, 0, sizeof(SimpleConstantPool));
    memset(&st->ip, 0, sizeof(SimpleInterfacePool));
    memset(&st->fp, 0, sizeof(SimpleFieldPool));
    memset(&st->mp, 0, sizeof(SimpleMethodPool));
    memset(&st->locals, 0, sizeof(LocalVariables));
    memset(st->heap_slots, 0, sizeof(st->heap_slots));
    memset(st->cls_name, 0, sizeof(st->cls_name));
    memset(st->statics_arr, 0, sizeof(st->statics_arr));
    st->depth = 0;
    st->code_base = 0;
    st->heap_used = 0;                  /* contadores del GC (gc.c)     */
    st->gc_alloc = 0;
    st->gc_gen = 0;
    st->gc_last_run = 0;
    memset(st->gc_frames, 0, sizeof(st->gc_frames));
    memset(st->gc_saved, 0, sizeof(st->gc_saved));

    if (parseJavaClassFile(path, &cff) != 0)
        return -2;

    /* nombre de la clase cargada, p.ej. "DelayDemo" o "pkg/App" */
    cls = findClassRef(&st->cp, cff.this_class);
    if (cls != 0)
        getUTF8String(&st->cp, cls->stringIndex, 255, st->cls_name);

    stackInit(&st->stack, 500);

    /* inicializadores de campos estaticos: static int x = ...; */
    clinit = findMethodInPool(&st->cp, &st->mp, "<clinit>", 8);
    if (clinit != 0)
        executeMethod(clinit, &st->stack, &st->cp);

    /* punto de entrada real: public static void main(String[]) */
    entry = findMethodInPool(&st->cp, &st->mp, "main", 4);
    if (entry == 0)
        return -3;

    /* String[] args para main: siempre un array (vacio si argc==0),
     * con cada argumento como String del heap.  Se deja en el local 0
     * justo antes de entrar (clinit pudo usar los locales). */
    {
        int argarr = heap_new_ref_array(argc);
        for (i = 0; i < argc; i++)
            heap_ref_array_store(argarr, i, heap_new_string(argv[i]),
                                 STACK_ENTRY_OBJ);
        memset(&st->locals, 0, sizeof(LocalVariables));
        st->locals.integer[0] = argarr;
        st->locals.type[0] = STACK_ENTRY_OBJ;
    }

    executeMethod(entry, &st->stack, &st->cp);

    free_pools();
    jvm_free(st->stack.store);
    st->stack.store = 0;
    return 0;
}

/* Sin argumentos: main(String[0]). */
int jvm_run_file(Jvm *st, char *path)
{
    return jvm_run_args(st, path, 0, 0);
}
