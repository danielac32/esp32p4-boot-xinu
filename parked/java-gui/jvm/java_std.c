/*
 * java_std.c — natives PURAS de la libreria estandar de Java (p4-sdk).
 *
 * No dependen de plataforma (nada de ficheros, reloj, GPIO): se compilan
 * igual para el host (suite de regresion) y para el P4.  A diferencia de
 * esp32p4_hw.c / native_hw.c, que si necesitan los drivers.
 *
 * Registradas en java_lib.c: Integer.parseInt/toString, String.valueOf,
 * Math.abs/max/min, Boolean/Float/Character.toString y metodos de instancia
 * de String (substring, indexOf, toUpperCase/LowerCase, trim, startsWith,
 * endsWith, concat).
 *
 * Convencion de pila (metodos estaticos): los argumentos se empujaron de
 * izquierda a derecha, asi que el ULTIMO queda arriba; el retorno se
 * empuja al final.  Los metodos de instancia tienen el receiver debajo.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <string.h>
#include "simple_jvm.h"
#include "jvm_map.h"

/* ---- utilidades --------------------------------------------------- */

/* numero de argumentos de un descriptor "(...)...". */
static int desc_args(const char *type)
{
    int n = 0;
    const char *s;

    if (type == 0)
        return 0;
    s = strchr(type, '(');
    if (s == 0)
        return 0;
    s++;
    while (*s != 0 && *s != ')') {
        if (*s == '[') {                    /* array: cuenta 1 */
            while (*s == '[') s++;
            if (*s == 'L') { while (*s && *s != ';') s++; if (*s) s++; }
            else s++;
        } else if (*s == 'L') {
            while (*s && *s != ';') s++;
            if (*s) s++;
        } else {
            s++;
        }
        n++;
    }
    return n;
}

static int parse_int(const char *s, int *ok)
{
    int sign = 1, v = 0, any = 0;

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; any = 1; }
    if (ok) *ok = any;
    return sign * v;
}

/* saca el receiver (String literal o del heap) a `out`. */
static void pop_receiver(StackFrame *stack, SimpleConstantPool *p,
                         char *out, int max)
{
    jvm_pop_text(stack, p, out, max);
}

/* ---- Integer ------------------------------------------------------ */

int java_lang_integer_parseInt(StackFrame *stack, SimpleConstantPool *p,
                               char *type)
{
    char buf[32];
    int ok = 0;

    (void) type;
    jvm_pop_text(stack, p, buf, sizeof(buf));
    pushInt(stack, parse_int(buf, &ok));
    return 0;
}

int java_lang_integer_toString(StackFrame *stack, SimpleConstantPool *p,
                               char *type)
{
    char buf[24];

    (void) p;
    (void) type;
    snprintf(buf, sizeof(buf), "%d", popInt(stack));
    pushObj(stack, heap_new_string(buf));
    return 0;
}

/* ---- String.valueOf(<tipo>) --------------------------------------- */

int java_lang_string_valueOf(StackFrame *stack, SimpleConstantPool *p,
                             char *type)
{
    char buf[96];
    char t = (type != 0) ? type[1] : 'I';

    if (t == 'Z') {                          /* boolean */
        int v = popInt(stack);
        const char *s = v ? "true" : "false";
        int n = (int) strlen(s);
        memcpy(buf, s, (size_t) n + 1);
    } else if (t == 'C') {                   /* char */
        buf[0] = (char) popInt(stack);
        buf[1] = 0;
    } else {                                 /* int/float/objeto -> texto */
        jvm_entry_text(popEntry(stack), p, buf, sizeof(buf));
    }
    pushObj(stack, heap_new_string(buf));
    return 0;
}

/* ---- Boolean / Float / Character .toString ------------------------ */

int java_lang_boolean_toString(StackFrame *stack, SimpleConstantPool *p,
                               char *type)
{
    int v = popInt(stack);

    (void) p;
    (void) type;
    pushObj(stack, heap_new_string(v ? "true" : "false"));
    return 0;
}

int java_lang_float_toString(StackFrame *stack, SimpleConstantPool *p,
                             char *type)
{
    char buf[48];

    (void) p;
    (void) type;
    snprintf(buf, sizeof(buf), "%g", (double) popFloat(stack));
    pushObj(stack, heap_new_string(buf));
    return 0;
}

int java_lang_character_toString(StackFrame *stack, SimpleConstantPool *p,
                                 char *type)
{
    char buf[2];

    (void) p;
    (void) type;
    buf[0] = (char) popInt(stack);
    buf[1] = 0;
    pushObj(stack, heap_new_string(buf));
    return 0;
}

/* ---- Math.abs / max / min ----------------------------------------- */

int java_lang_math_abs(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char t = (type != 0) ? type[1] : 'I';

    (void) p;
    if (t == 'F') { float f = popFloat(stack); pushFloat(stack, f < 0 ? -f : f); }
    else if (t == 'D') { double d = popDouble(stack); pushDouble(stack, d < 0 ? -d : d); }
    else { int v = popInt(stack); pushInt(stack, v < 0 ? -v : v); }
    return 0;
}

int java_lang_math_max(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char t = (type != 0) ? type[1] : 'I';

    (void) p;
    if (t == 'F') { float b = popFloat(stack), a = popFloat(stack); pushFloat(stack, a > b ? a : b); }
    else if (t == 'D') { double b = popDouble(stack), a = popDouble(stack); pushDouble(stack, a > b ? a : b); }
    else { int b = popInt(stack); int a = popInt(stack); pushInt(stack, a > b ? a : b); }
    return 0;
}

int java_lang_math_min(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char t = (type != 0) ? type[1] : 'I';

    (void) p;
    if (t == 'F') { float b = popFloat(stack), a = popFloat(stack); pushFloat(stack, a < b ? a : b); }
    else if (t == 'D') { double b = popDouble(stack), a = popDouble(stack); pushDouble(stack, a < b ? a : b); }
    else { int b = popInt(stack); int a = popInt(stack); pushInt(stack, a < b ? a : b); }
    return 0;
}

/* ---- String (metodos de instancia) -------------------------------- */

/* substring(int) / substring(int, int) */
int java_lang_string_substring(StackFrame *stack, SimpleConstantPool *p,
                               char *type)
{
    char text[1024];
    int begin, end, len;

    if (desc_args(type) >= 2) {
        end = popInt(stack);
        begin = popInt(stack);
    } else {
        end = -1;
        begin = popInt(stack);
    }
    pop_receiver(stack, p, text, sizeof(text));
    len = (int) strlen(text);
    if (begin < 0) begin = 0;
    if (begin > len) begin = len;
    if (end < 0 || end > len) end = len;
    if (end < begin) end = begin;
    text[end] = 0;
    pushObj(stack, heap_new_string(text + begin));
    return 0;
}

/* indexOf(String) / indexOf(String,int) / indexOf(int) -> posicion o -1 */
int java_lang_string_indexOf(StackFrame *stack, SimpleConstantPool *p,
                             char *type)
{
    char text[1024], needle[1024];
    char t = (type != 0) ? type[1] : 'L';
    int from = 0, lt, ln, i, j;

    if (desc_args(type) >= 2)
        from = popInt(stack);

    if (t == 'I') {                          /* indexOf(int ch) */
        needle[0] = (char) popInt(stack);
        needle[1] = 0;
    } else {
        jvm_pop_text(stack, p, needle, sizeof(needle));
    }
    pop_receiver(stack, p, text, sizeof(text));

    lt = (int) strlen(text);
    ln = (int) strlen(needle);
    if (from < 0) from = 0;
    if (ln == 0) { pushInt(stack, from <= lt ? from : lt); return 0; }
    for (i = from; i + ln <= lt; i++) {
        for (j = 0; j < ln; j++)
            if (text[i + j] != needle[j]) break;
        if (j == ln) { pushInt(stack, i); return 0; }
    }
    pushInt(stack, -1);
    return 0;
}

/* toUpperCase() / toLowerCase() */
int java_lang_string_toUpper(StackFrame *stack, SimpleConstantPool *p,
                             char *type)
{
    char text[1024];
    int i;

    (void) type;
    pop_receiver(stack, p, text, sizeof(text));
    for (i = 0; text[i] != 0; i++)
        if (text[i] >= 'a' && text[i] <= 'z') text[i] = (char) (text[i] - 32);
    pushObj(stack, heap_new_string(text));
    return 0;
}

int java_lang_string_toLower(StackFrame *stack, SimpleConstantPool *p,
                             char *type)
{
    char text[1024];
    int i;

    (void) type;
    pop_receiver(stack, p, text, sizeof(text));
    for (i = 0; text[i] != 0; i++)
        if (text[i] >= 'A' && text[i] <= 'Z') text[i] = (char) (text[i] + 32);
    pushObj(stack, heap_new_string(text));
    return 0;
}

int java_lang_string_trim(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char text[1024];
    int b = 0, e;

    (void) type;
    pop_receiver(stack, p, text, sizeof(text));
    e = (int) strlen(text);
    while (b < e && (text[b] == ' ' || text[b] == '\t' ||
                     text[b] == '\n' || text[b] == '\r')) b++;
    while (e > b && (text[e - 1] == ' ' || text[e - 1] == '\t' ||
                     text[e - 1] == '\n' || text[e - 1] == '\r')) e--;
    text[e] = 0;
    pushObj(stack, heap_new_string(text + b));
    return 0;
}

/* startsWith(String) / endsWith(String) */
int java_lang_string_startsWith(StackFrame *stack, SimpleConstantPool *p,
                                char *type)
{
    char text[1024], pre[1024];
    int lt, lp;

    (void) type;
    jvm_pop_text(stack, p, pre, sizeof(pre));
    pop_receiver(stack, p, text, sizeof(text));
    lt = (int) strlen(text);
    lp = (int) strlen(pre);
    pushInt(stack, (lp <= lt && strncmp(text, pre, (size_t) lp) == 0) ? 1 : 0);
    return 0;
}

int java_lang_string_endsWith(StackFrame *stack, SimpleConstantPool *p,
                              char *type)
{
    char text[1024], suf[1024];
    int lt, ls;

    (void) type;
    jvm_pop_text(stack, p, suf, sizeof(suf));
    pop_receiver(stack, p, text, sizeof(text));
    lt = (int) strlen(text);
    ls = (int) strlen(suf);
    pushInt(stack, (ls <= lt && strcmp(text + lt - ls, suf) == 0) ? 1 : 0);
    return 0;
}

/* concat(String) -> String (no muta) */
int java_lang_string_concat(StackFrame *stack, SimpleConstantPool *p,
                            char *type)
{
    char a[1024], b[1024];
    int la;

    (void) type;
    jvm_pop_text(stack, p, b, sizeof(b));
    pop_receiver(stack, p, a, sizeof(a));
    la = (int) strlen(a);
    if (la + (int) strlen(b) < (int) sizeof(a))
        memcpy(a + la, b, strlen(b) + 1);
    pushObj(stack, heap_new_string(a));
    return 0;
}
