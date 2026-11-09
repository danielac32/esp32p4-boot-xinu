/*
 * gc.c — recolector mark & sweep del heap de slots (p4-sdk)
 *
 * La tabla de JVM_HEAP_SLOTS ranuras nunca se liberaba: cada String o
 * StringBuilder que fabrica un ciclo de refresco dejaba una ranura tomada
 * hasta que la app moria (`[jvm] heap: slots agotados (2048)`).  Aqui se
 * marcan los objetos alcanzables y se barren los que no.
 *
 * Raices (precisas, por tipo):
 *   - jvm->locals y los `saved` de cada cuadro en pila.  invoke_resolved()
 *     guarda los locals del llamador como local C, fuera del estado, asi que
 *     los registra en gc_saved[]; lo mismo hacen las pilas de los callee
 *     (su `store` vive en PSRAM) en gc_frames[].
 *   - solo cuentan las entradas STACK_ENTRY_OBJ: una REF es un indice del
 *     pool de constantes, no una ranura del heap.
 *   - estaticos y campos de objeto: el pool de campos dice cuales son
 *     referencia (descriptor 'L' o '['), de modo que un entero normal
 *     (coordenadas, handles de LVGL, tamaños) no se confunde con un puntero.
 *
 * Alcance: OBJECT y REFARRAY recorren su interior; IARRAY/BARRAY son enteros
 * puros y STRING/SBUILDER solo texto, asi que no tienen hijos.
 *
 * Dos momentos de barrido:
 *   1. Punto seguro, entre bytecodes (jvm_gc_safe_point): el estado de la VM
 *      esta completo y no hay ninguna native a medias, con lo que las raices
 *      tipadas son exactas.
 *   2. Emergencia (heap_new_slot ya no encuentra ranuras): una expresion puede
 *      tener referencias solo en un local o un registro C —por ejemplo el
 *      String[] que rellena native_fat—, asi que ademas de lo anterior se
 *      barre conservadoramente la pila C y se protegen las ranuras creadas
 *      despues del ultimo punto seguro, que aun no estaban en ningun sitio.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include "simple_jvm.h"
#include "jvm_map.h"

#define GC_LOW_WATER   96        /* por debajo de estas libres, barrer   */
#define GC_MIN_NEW     64        /* objetos nuevos minimos entre barridos */
#define GC_MAX_FIELDS  100       /* SimpleFieldPool.field[]               */

/* Marca y encola una referencia; ignora null, fuera de rango y libres. */
static void gc_mark(int ref, Jvm *st, int *qt)
{
    unsigned char *b;
    unsigned char m;

    if (ref <= 0 || ref >= JVM_HEAP_SLOTS)
        return;
    if (st->heap_slots[ref].kind == HEAP_KIND_FREE)
        return;
    b = &st->gc_marks[ref >> 3];
    m = (unsigned char) (1 << (ref & 7));
    if (*b & m)
        return;                       /* ya marcado (y en cola) */
    *b |= m;
    if (*qt < JVM_HEAP_SLOTS)
        st->gc_queue[(*qt)++] = ref;
}

static void gc_locals(LocalVariables *lv, Jvm *st, int *qt)
{
    int i;

    for (i = 0; i < JVM_MAX_LOCALS; i++)
        if (lv->type[i] == STACK_ENTRY_OBJ)
            gc_mark(lv->integer[i], st, qt);
}

/* Pila de operandos de un cuadro.  `todo` (emergencia) recorre tambien las
 * entradas ya descartadas: popEntry() no las borra y una native puede seguir
 * usandolas. */
static void gc_frame(StackFrame *f, Jvm *st, int *qt, int todo)
{
    int i, n;

    if (f == 0 || f->store == 0)
        return;
    n = todo ? f->max_size : f->size;
    for (i = 0; i < n; i++)
        if (f->store[i].type == STACK_ENTRY_OBJ)
            gc_mark(EntryToInt(&f->store[i]), st, qt);
}

/* ¿Que posiciones del pool de campos son referencias?  El orden es el mismo
 * que recorren field_pos() y el bytecode `new`: los campos del pool tal cual,
 * separando estaticos (access_flags 0x0008). */
static void gc_field_maps(Jvm *st, unsigned char *inst, unsigned char *stat)
{
    char d[255];
    int i, pi = 0, ps = 0;

    for (i = 0; i < st->fp.field_used && i < GC_MAX_FIELDS; i++) {
        int es_estatico = (st->fp.field[i].access_flags & 0x0008) != 0;
        getUTF8String(&st->cp, st->fp.field[i].descriptor_index, 255, d);
        if (es_estatico) {
            if (ps < JVM_MAX_STATICS && (d[0] == 'L' || d[0] == '['))
                stat[ps] = 1;
            ps++;
        } else {
            if (pi < GC_MAX_FIELDS && (d[0] == 'L' || d[0] == '['))
                inst[pi] = 1;
            pi++;
        }
    }
}

static void gc_children(int ref, Jvm *st, int *qt, const unsigned char *inst)
{
    JvmHeapSlot *s = &st->heap_slots[ref];
    int i;

    if (s->data == 0)
        return;
    if (s->kind == HEAP_KIND_OBJECT) {
        for (i = 0; i < s->len && i < GC_MAX_FIELDS; i++)
            if (inst[i])
                gc_mark(s->data[i], st, qt);
    } else if (s->kind == HEAP_KIND_REFARRAY) {
        for (i = 0; i < s->len; i++)
            if (s->data[i * 2 + 1] == STACK_ENTRY_OBJ)
                gc_mark(s->data[i * 2], st, qt);
    }
}

/* Barrido conservador de la pila C, solo en emergencia: cualquier palabra que
 * sea un id de ranura existente se toma como referencia.  Se llama despues de
 * __builtin_unwind_init(), que derrama los registros salvados dentro del
 * propio cuadro de esta funcion. */
static void gc_cstack(Jvm *st, int *qt)
{
    unsigned long *p;
    unsigned long *top;
    void *limite = jvm_stack_top();

    if (limite == 0)
        return;
    p = (unsigned long *) __builtin_frame_address(0);
    p = (unsigned long *) (((unsigned long) p) & ~3ul);
    top = (unsigned long *) limite;
    while (p < top) {
        gc_mark((int) *p, st, qt);
        p++;
    }
}

int jvm_gc(int conservative)
{
    Jvm *st = jvm_current_get();
    unsigned char inst[GC_MAX_FIELDS];
    unsigned char stat[GC_MAX_FIELDS];
    int qt = 0, head = 0, i;
    int usados = 0, vivos = 0;

    memset(inst, 0, sizeof(inst));
    memset(stat, 0, sizeof(stat));
    gc_field_maps(st, inst, stat);

    if (conservative)
        __builtin_unwind_init();

    memset(st->gc_marks, 0, sizeof(st->gc_marks));

    gc_locals(&st->locals, st, &qt);
    gc_frame(&st->stack, st, &qt, conservative);
    for (i = 0; i < JVM_MAX_CALLS + 2; i++) {
        if (st->gc_saved[i] != 0)
            gc_locals(st->gc_saved[i], st, &qt);
        if (st->gc_frames[i] != 0)
            gc_frame(st->gc_frames[i], st, &qt, conservative);
    }
    for (i = 0; i < JVM_MAX_STATICS; i++)
        if (stat[i])
            gc_mark(st->statics_arr[i], st, &qt);

    if (conservative) {
        /* ranuras concedidas desde el ultimo punto seguro: las tiene en la
         * mano una expresion a medias y aun no estan guardadas en ningun sitio */
        for (i = 1; i < JVM_HEAP_SLOTS; i++)
            if (st->heap_slots[i].kind != HEAP_KIND_FREE &&
                (unsigned) (st->heap_slots[i].gen - st->gc_gen) < 0x80000000u)
                gc_mark(i, st, &qt);
        gc_cstack(st, &qt);
    }

    while (head < qt)
        gc_children(st->gc_queue[head++], st, &qt, inst);

    for (i = 1; i < JVM_HEAP_SLOTS; i++) {
        JvmHeapSlot *s = &st->heap_slots[i];
        unsigned char m = (unsigned char) (1 << (i & 7));

        if (s->kind == HEAP_KIND_FREE)
            continue;
        usados++;
        if (st->gc_marks[i >> 3] & m) {
            vivos++;
            continue;
        }
        if (s->data != 0)
            free(s->data);
        if (s->text != 0)
            free(s->text);
        memset(s, 0, sizeof(*s));
    }
    st->heap_used = vivos;

    fprintf(stderr, "[jvm] gc: en uso %d -> %d (%s)\n", usados, vivos,
            conservative ? "emergencia" : "punto seguro");
    return usados - vivos;
}

/* Punto seguro del interprete: entre bytecodes no queda ninguna expresion a
 * medias, asi que el barrido es exacto.  Solo barre cuando el agua esta baja
 * y han aparecido objetos nuevos desde la ultima vez. */
void jvm_gc_safe_point(void)
{
    Jvm *st = jvm_current_get();

    st->gc_gen = st->gc_alloc;
    if (JVM_HEAP_SLOTS - st->heap_used >= GC_LOW_WATER)
        return;
    if ((unsigned) (st->gc_alloc - st->gc_last_run) < GC_MIN_NEW)
        return;
    st->gc_last_run = st->gc_alloc;
    jvm_gc(0);
}
