/*
 * heap.c — objetos y arrays para la JVM (p4-sdk)
 *
 * Las "referencias" Java son ids enteros de una tabla de slots:
 * portable (no depende del tamaño del puntero), y da chequeo de
 * límites gratis. Slot 0 = null.
 *
 *   kind 1 = array de int   (data: int[len])
 *   kind 2 = objeto         (data: int[nfields], campos en orden de
 *                            declaración, sin estáticos)
 *
 * En el ESP32-P4, malloc() se implementa sobre PSRAM (fase D).
 */

#include "simple_jvm.h"
#include "jvm_map.h"

#define HEAP_SLOTS JVM_HEAP_SLOTS

/* Los tipos de slot (HEAP_KIND_*) estan en simple_jvm.h, junto al struct,
 * porque gc.c tambien los usa. */

/* Los slots del heap (y los estaticos y el nombre de clase) viven en el
 * estado de la instancia (simple_jvm.h: JvmHeapSlot); se accede a ellos
 * por el macro `heap`, que resuelve al estado de la tarea actual. */

static void heap_die(const char *msg, int ref)
{
    fprintf(stderr, "[jvm] heap: %s (ref=%d)\n", msg, ref);
    exit(6);
}

static int heap_pick(void)
{
    int i;
    for (i = 1; i < HEAP_SLOTS; i++) {
        if (heap[i].kind == HEAP_KIND_FREE)
            return i;
    }
    return -1;
}

static int heap_new_slot(int kind, int len, int *data)
{
    int i = heap_pick();

    if (i < 0) {
        /* tabla llena: ultimo barrido antes de rendirse.  Es emergency porque
         * ocurre en medio de una expresion, con raices solo en la pila C. */
        if (jvm_gc(1) == 0 || (i = heap_pick()) < 0) {
            fprintf(stderr, "[jvm] heap: slots agotados (%d)\n", HEAP_SLOTS);
            exit(6);
        }
    }
    heap[i].kind = kind;
    heap[i].len = len;
    heap[i].cap = 0;
    heap[i].data = data;
    heap[i].text = 0;
    heap[i].gen = ++jvm->gc_alloc;     /* proteccion del GC de emergencia */
    jvm->heap_used++;
    return i;
}

static JvmHeapSlot *heap_slot(int ref)
{
    if (ref <= 0 || ref >= HEAP_SLOTS || heap[ref].kind == HEAP_KIND_FREE)
        heap_die("referencia invalida (null o fuera de rango)", ref);
    return &heap[ref];
}

int heap_new_int_array(int len)
{
    int *data;
    if (len < 0) {
        fprintf(stderr, "[jvm] heap: longitud negativa (%d)\n", len);
        exit(6);
    }
    data = (int *) calloc((size_t) len, sizeof(int));
    if (data == 0) {
        fprintf(stderr, "[jvm] heap: OOM creando array de %d ints\n", len);
        exit(6);
    }
    return heap_new_slot(HEAP_KIND_IARRAY, len, data);
}

/* byte[] real: un byte por elemento (no un int).  `text` guarda los bytes. */
int heap_new_byte_array(int len)
{
    int n;
    char *b;

    if (len < 0) {
        fprintf(stderr, "[jvm] heap: longitud negativa (%d)\n", len);
        exit(6);
    }
    n = (len > 0) ? len : 1;                 /* len 0: 1 byte de relleno */
    b = (char *) calloc((size_t) n, 1);
    if (b == 0) {
        fprintf(stderr, "[jvm] heap: OOM creando byte[%d]\n", len);
        exit(6);
    }
    {
        int ref = heap_new_slot(HEAP_KIND_BARRAY, len, 0);
        heap[ref].text = b;
        heap[ref].cap = n;
        return ref;
    }
}

int heap_array_load(int ref, int idx)
{
    JvmHeapSlot *s = heap_slot(ref);
    if (s->kind != HEAP_KIND_IARRAY && s->kind != HEAP_KIND_BARRAY)
        heap_die("no es un array", ref);
    if (idx < 0 || idx >= s->len) {
        fprintf(stderr, "[jvm] heap: indice %d fuera de [0..%d)\n", idx, s->len);
        exit(6);
    }
    if (s->kind == HEAP_KIND_BARRAY)
        return (unsigned char) s->text[idx];   /* byte[]: baload 0..255 */
    return s->data[idx];
}

void heap_array_store(int ref, int idx, int value)
{
    JvmHeapSlot *s = heap_slot(ref);
    if (s->kind != HEAP_KIND_IARRAY && s->kind != HEAP_KIND_BARRAY)
        heap_die("no es un array", ref);
    if (idx < 0 || idx >= s->len) {
        fprintf(stderr, "[jvm] heap: indice %d fuera de [0..%d)\n", idx, s->len);
        exit(6);
    }
    if (s->kind == HEAP_KIND_BARRAY)
        s->text[idx] = (char) (value & 0xff);   /* bastore: byte bajo */
    else
        s->data[idx] = value;
}

int heap_array_length(int ref)
{
    JvmHeapSlot *s = heap_slot(ref);
    if (s->kind != HEAP_KIND_IARRAY && s->kind != HEAP_KIND_REFARRAY &&
        s->kind != HEAP_KIND_BARRAY)
        heap_die("arraylength sobre no-array", ref);
    return s->len;
}

/* ---- arrays de referencias (String[]/Object[]) --------------------
 * Cada elemento guarda (valor, tipo STACK_ENTRY_*) para no perder si la
 * referencia es del pool (REF) o del heap (OBJ). data = 2 ints/elem. */

int heap_new_ref_array(int len)
{
    int n = (len > 0) ? len : 1;                 /* len 0: 1 par de relleno */
    int *data = (int *) calloc((size_t) n * 2, sizeof(int));
    if (data == 0) {
        fprintf(stderr, "[jvm] heap: OOM creando array de %d refs\n", len);
        exit(6);
    }
    return heap_new_slot(HEAP_KIND_REFARRAY, len, data);
}

int heap_ref_array_load(int ref, int idx, int *type)
{
    JvmHeapSlot *s = heap_slot(ref);
    if (s->kind != HEAP_KIND_REFARRAY)
        heap_die("no es array de referencias", ref);
    if (idx < 0 || idx >= s->len) {
        fprintf(stderr, "[jvm] heap: indice %d fuera de [0..%d)\n", idx, s->len);
        exit(6);
    }
    *type = s->data[idx * 2 + 1];
    return s->data[idx * 2];
}

void heap_ref_array_store(int ref, int idx, int value, int type)
{
    JvmHeapSlot *s = heap_slot(ref);
    if (s->kind != HEAP_KIND_REFARRAY)
        heap_die("no es array de referencias", ref);
    if (idx < 0 || idx >= s->len) {
        fprintf(stderr, "[jvm] heap: indice %d fuera de [0..%d)\n", idx, s->len);
        exit(6);
    }
    s->data[idx * 2] = value;
    s->data[idx * 2 + 1] = type;
}

int heap_new_object(int nfields)
{
    int *data = (int *) calloc((size_t) (nfields > 0 ? nfields : 1), sizeof(int));
    if (data == 0) {
        fprintf(stderr, "[jvm] heap: OOM creando objeto de %d campos\n", nfields);
        exit(6);
    }
    return heap_new_slot(HEAP_KIND_OBJECT, nfields, data);
}

int heap_object_get(int ref, int idx)
{
    JvmHeapSlot *s = heap_slot(ref);
    if (s->kind != HEAP_KIND_OBJECT)
        heap_die("getfield sobre no-objeto", ref);
    if (idx < 0 || idx >= s->len)
        heap_die("indice de campo invalido", idx);
    return s->data[idx];
}

void heap_object_put(int ref, int idx, int value)
{
    JvmHeapSlot *s = heap_slot(ref);
    if (s->kind != HEAP_KIND_OBJECT)
        heap_die("putfield sobre no-objeto", ref);
    if (idx < 0 || idx >= s->len)
        heap_die("indice de campo invalido", idx);
    s->data[idx] = value;
}

/* ---- Strings reales y StringBuilders (p4-sdk) ----------------------
 * Un String del heap es inmutable: se crea una vez (literal resuelto o
 * resultado de toString) y sus chars no cambian. El StringBuilder tiene
 * un buffer propio que crece.  Ni unos ni otros se recuperan aqui: el
 * que los pierde lo barre gc.c por raices (mark & sweep). */

/* ¿es este id un String? No mata: devuelve 0 para null/otro kind. */
int heap_is_string(int ref)
{
    if (ref <= 0 || ref >= HEAP_SLOTS)
        return 0;
    return heap[ref].kind == HEAP_KIND_STRING;
}

int heap_new_string(const char *s)
{
    int n = (int) strlen(s);
    char *t = (char *) malloc((size_t) n + 1);
    int ref;
    if (t == 0) {
        fprintf(stderr, "[jvm] heap: OOM creando String de %d chars\n", n);
        exit(6);
    }
    memcpy(t, s, (size_t) n + 1);
    ref = heap_new_slot(HEAP_KIND_STRING, n, 0);
    heap[ref].text = t;
    heap[ref].cap = n + 1;
    return ref;
}

/* Convierte un slot YA reservado en un String con el texto dado.  Se usa
 * en el constructor <init> de java/lang/String: `new` dejo un objeto y el
 * `dup` una segunda referencia al MISMO slot, asi que mutamos ese slot (no
 * creamos otro) para que la referencia que sobrevive apunte al String. */
void heap_init_string(int ref, const char *s)
{
    JvmHeapSlot *h = heap_slot(ref);
    int n = (int) strlen(s);
    char *t = (char *) malloc((size_t) n + 1);
    if (t == 0) {
        fprintf(stderr, "[jvm] heap: OOM creando String de %d chars\n", n);
        exit(6);
    }
    memcpy(t, s, (size_t) n + 1);
    /* libera lo que tuviera el slot (el objeto vacio de `new`) */
    if (h->data != 0)
        free(h->data);
    if (h->text != 0)
        free(h->text);
    h->kind = HEAP_KIND_STRING;
    h->len = n;
    h->cap = n + 1;
    h->data = 0;
    h->text = t;
}

char *heap_string_text(int ref)
{
    JvmHeapSlot *s = heap_slot(ref);
    if (s->kind != HEAP_KIND_STRING)
        heap_die("no es un String", ref);
    return s->text;
}

int heap_new_sbuilder(void)
{
    int ref = heap_new_slot(HEAP_KIND_SBUILDER, 0, 0);
    heap[ref].cap = 64;
    heap[ref].text = (char *) malloc(64);
    if (heap[ref].text == 0) {
        fprintf(stderr, "[jvm] heap: OOM creando StringBuilder\n");
        exit(6);
    }
    heap[ref].text[0] = 0;
    return ref;
}

void heap_sbuilder_append(int ref, const char *s)
{
    JvmHeapSlot *h = heap_slot(ref);
    int add = (int) strlen(s);
    if (h->kind != HEAP_KIND_SBUILDER)
        heap_die("append sobre no-StringBuilder", ref);
    if (h->len + add + 1 > h->cap) {         /* crece (duplicando) */
        int cap = h->cap;
        char *t;
        while (h->len + add + 1 > cap)
            cap *= 2;
        t = (char *) realloc(h->text, (size_t) cap);
        if (t == 0) {
            fprintf(stderr, "[jvm] heap: OOM creciendo StringBuilder\n");
            exit(6);
        }
        h->text = t;
        h->cap = cap;
    }
    memcpy(h->text + h->len, s, (size_t) add);
    h->len += add;
    h->text[h->len] = 0;
}

char *heap_sbuilder_text(int ref)
{
    JvmHeapSlot *h = heap_slot(ref);
    if (h->kind != HEAP_KIND_SBUILDER)
        heap_die("toString sobre no-StringBuilder", ref);
    return h->text;
}

int heap_sbuilder_len(int ref)
{
    JvmHeapSlot *h = heap_slot(ref);
    if (h->kind != HEAP_KIND_SBUILDER)
        heap_die("length sobre no-StringBuilder", ref);
    return h->len;
}
