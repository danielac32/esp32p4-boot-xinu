/*
 * Simple Java Virtual Machine Implementation
 *
 * Copyright (C) 2014 Jim Huang <jserv.tw@gmail.com>
 * Copyright (C) 2013 Chun-Yu Wang <wicanr2@gmail.com>
 */

#include "simple_jvm.h"
#include "jvm_map.h"
#include "java_lib.h"
#include <stdint.h>
#include <string.h>

/* Las operaciones float se escriben en C con tipos `float', NO llamando a
 * __adddf3/__extendsfdf2/__truncdfsf2 a mano moviendo los bits.  Eso estaba
 * aqui antes y solo funcionaba por accidente de ABI: en el Xtensa del ESP32-S3
 * un float viaja por los registros enteros, igual que un `unsigned int', asi
 * que llamar a `__extendsfdf2(unsigned)' coincidia con la firma real.  En
 * RISC-V con -mabi=ilp32f (ESP32-P4) un float viaja por fa0..fa7 y el doble
 * por pares enteros a0:a1, a2:a3; declarar la helper con `unsigned' hace que
 * el llamador entregue los bits en a0 mientras la rutina del ROM --o del
 * libgcc, o la nuestra-- lee fa0.  El resultado no era un trap: era 0, y asi
 * lo_midio `java FloatTest' en la placa (sum/prod/quot = 0).
 *
 * Con `float' de C el compilador elige la ruta correcta para cada objetivo:
 * en el P4 fadd/fsub/fmul/fdiv/f2i/i2f salen de la unidad de simple precision
 * (instrucciones fadd.s, fcvt.w.s...: sin ROM y sin roundeo doble, que es
 * justo lo que Java pide para fadd) y en el host van a la FPU x86.  Las
 * helpers de doble (`dadd', `dmul', `d2i') siguen en C con `double' y, donde
 * no hay extension D, el enlace las resuelve contra el mask ROM. */

/*
 * El estado de la VM (pools, locals, estaticos, profundidad de llamada y
 * base del codigo) es POR INSTANCIA: simple_jvm.h expone accesos que
 * resuelven al estado de la tarea actual (jvm_current_get()).  Por eso ya
 * no hay globals aqui.  `run` solo se lee (nunca se pone a 0 desde el
 * interprete), asi que se queda como constante de fichero.
 */
static int run = 1;

static ConstantFieldRef *findFieldRefByCPIndex(SimpleConstantPool *p, int index)
{
    int i;
    for (i = 0; i < p->field_used; i++)
        if (p->field[i].index == index)
            return &p->field[i];
    return 0;
}

/* busca un metodo en el pool por nombre + descriptor (VM de una clase) */
static MethodInfo *findMethodByNameDesc(SimpleConstantPool *p,
                                        char *name, char *desc)
{
    int i;
    char mname[255], mdesc[255];
    for (i = 0; i < simpleMethodPool.method_used; i++) {
        MethodInfo *m = &simpleMethodPool.method[i];
        getUTF8String(p, m->name_index, 255, mname);
        getUTF8String(p, m->descriptor_index, 255, mdesc);
        if (strcmp(mname, name) == 0 && strcmp(mdesc, desc) == 0)
            return m;
    }
    return 0;
}

/* posicion de un campo entre los estaticos (o entre los de instancia) */
static int field_pos(SimpleConstantPool *p, char *fname, int want_static)
{
    int i, pos = 0;
    char n[255];
    for (i = 0; i < simpleFieldPool.field_used; i++) {
        int is_static = (simpleFieldPool.field[i].access_flags & 0x0008) != 0;
        if (is_static != want_static)
            continue;
        getUTF8String(p, simpleFieldPool.field[i].name_index, 255, n);
        if (strcmp(n, fname) == 0)
            return pos;
        pos++;
    }
    return -1;
}

/* ¿el campo es de tipo float (descriptor "F")? para que getstatic/
 * getfield empujen un FLOAT en vez de un INT con los bits. */
static int field_is_float(SimpleConstantPool *p, char *fname, int want_static)
{
    int i;
    char n[255], d[255];
    for (i = 0; i < simpleFieldPool.field_used; i++) {
        int is_static = (simpleFieldPool.field[i].access_flags & 0x0008) != 0;
        if (is_static != want_static)
            continue;
        getUTF8String(p, simpleFieldPool.field[i].name_index, 255, n);
        if (strcmp(n, fname) == 0) {
            getUTF8String(p, simpleFieldPool.field[i].descriptor_index, 255, d);
            return d[0] == 'F';
        }
    }
    return 0;
}

/* ¿el campo es de referencia (descriptor "L...;" o "[...")?  Hace dos cosas:
 * getfield/getstatic empujan OBJ en vez de INT —si no, un String guardado en
 * un campo o en un estatico llega al local como INT y gc.c no lo ve, con lo
 * que el barrido liberaria un objeto vivo—, y gc.c usa el mismo criterio para
 * recorrer los campos, de modo que un entero normal (coordenadas, handles de
 * LVGL) nunca se confunde con un puntero. */
static int field_is_ref(SimpleConstantPool *p, char *fname, int want_static)
{
    int i;
    char n[255], d[255];
    for (i = 0; i < simpleFieldPool.field_used; i++) {
        int is_static = (simpleFieldPool.field[i].access_flags & 0x0008) != 0;
        if (is_static != want_static)
            continue;
        getUTF8String(p, simpleFieldPool.field[i].name_index, 255, n);
        if (strcmp(n, fname) == 0) {
            getUTF8String(p, simpleFieldPool.field[i].descriptor_index, 255, d);
            return d[0] == 'L' || d[0] == '[';
        }
    }
    return 0;
}

/* cuenta argumentos del descriptor "(IIZ)V" -> 3 */
static int desc_arg_count(const char *d)
{
    int n = 0;
    const char *q = d;
    if (*q != '(')
        return -1;
    for (q++; *q && *q != ')'; q++) {
        while (*q == '[')
            q++;
        if (*q == 'L') {
            while (*q && *q != ';')
                q++;
        }
        n++;
    }
    return (*q == ')') ? n : -1;
}

static int desc_returns_value(const char *d)
{
    const char *q = strchr(d, ')');
    return (q != 0 && q[1] != 'V');
}

/*
 * Ejecuta un metodo resuelto con frame propio (locals + pila):
 * los argumentos se sacan de la pila del llamador (el ultimo arriba),
 * se ejecuta el cuerpo, y el valor de retorno vuelve a la pila del
 * llamador. instance=1 significa que hay un "this" bajo los argumentos.
 */
static int invoke_resolved(MethodInfo *m, StackFrame *caller,
                           SimpleConstantPool *p, int instance)
{
    char desc[255];
    int argc, i, lvl;
    LocalVariables saved;
    StackFrame callee;

    getUTF8String(p, m->descriptor_index, 255, desc);
    argc = desc_arg_count(desc);
    if (argc < 0) {
        fprintf(stderr, "[jvm] descriptor invalido: %s\n", desc);
        exit(4);
    }
    if (++jvm_call_depth > JVM_MAX_CALLS) {
        fprintf(stderr, "[jvm] recursion Java demasiado profunda (> %d)\n",
                JVM_MAX_CALLS);
        exit(5);
    }
    /* `saved` y `callee` son locales C de esta llamada: el GC no los ve si
     * solo barre el estado de la instancia, asi que se registran mientras el
     * callee este en marcha (gc.c: raices de cuadros en pila). */
    lvl = jvm_call_depth;

    saved = localVariables;             /* guarda los locals del llamador */
    jvm->gc_saved[lvl] = &saved;
    memset(&localVariables, 0, sizeof(LocalVariables));
    /* saca los argumentos PRESERVANDO el tipo (INT/REF/OBJ/FLOAT):
     * un REF debe seguir siendo REF en el callee para que las natives
     * de texto lo resuelvan, y un FLOAT conserva sus bits en el slot. */
    for (i = argc - 1; i >= 0; i--) {
        StackEntry *e = popEntry(caller);
        localVariables.integer[i + instance] = EntryToInt(e);
        localVariables.type[i + instance] = e->type;
    }
    if (instance) {
        StackEntry *e = popEntry(caller);           /* this */
        localVariables.integer[0] = EntryToInt(e);
        localVariables.type[0] = e->type;
    }

    stackInit(&callee, JVM_FRAME_STACK);
    jvm->gc_frames[lvl] = &callee;
    executeMethod(m, &callee, p);
    if (desc_returns_value(desc)) {
        const char *rq = strchr(desc, ')');
        char rt = (rq != 0) ? rq[1] : 'V';
        if (rt == 'F')                              /* float */
            pushFloat(caller, popFloat(&callee));
        else if (rt == 'L' || rt == '[') {          /* objeto/String */
            StackEntry *e = popEntry(&callee);
            if (e->type == STACK_ENTRY_OBJ)      pushObj(caller, EntryToInt(e));
            else if (e->type == STACK_ENTRY_REF) pushRef(caller, EntryToInt(e));
            else                                 pushInt(caller, EntryToInt(e));
        } else                                      /* int/boolean/char */
            pushInt(caller, popInt(&callee));
    }
    free(callee.store);

    jvm->gc_frames[lvl] = 0;            /* el cuadro ya no es raiz        */
    jvm->gc_saved[lvl] = 0;
    localVariables = saved;             /* restaura */
    jvm_call_depth--;
    return 0;
}

/* cargar local preservando el tipo (INT/REF/OBJ/FLOAT). Los natives y
 * la resolucion de texto distinguen REF (string del pool) de OBJ (String
 * u objeto del heap) y FLOAT (bits), asi que el tipo debe sobrevivir. */
static void local_load(int idx, StackFrame *stack)
{
    int v = localVariables.integer[idx];
    switch (localVariables.type[idx]) {
    case STACK_ENTRY_REF:  pushRef(stack, v); break;
    case STACK_ENTRY_OBJ:  pushObj(stack, v); break;
    case STACK_ENTRY_FLOAT: {
        float f;
        memcpy(&f, &v, sizeof(float));   /* bits -> float */
        pushFloat(stack, f);
        break;
    }
    default:               pushInt(stack, v); break;
    }
}

/* guardar local preservando el tipo. LONG/DOUBLE se degradan a INT
 * (los locales aqui tienen 1 slot; no soportamos long/double). */
static void local_store(int idx, StackFrame *stack)
{
    StackEntry *e = popEntry(stack);
    if (e->type == STACK_ENTRY_FLOAT) {          /* float: guarda bits */
        float f = EntryToFloat(e);
        memcpy(&localVariables.integer[idx], &f, sizeof(float));
        localVariables.type[idx] = STACK_ENTRY_FLOAT;
    } else {
        localVariables.integer[idx] = EntryToInt(e);
        localVariables.type[idx] = (e->type == STACK_ENTRY_REF ||
                                    e->type == STACK_ENTRY_OBJ)
                                       ? e->type
                                       : STACK_ENTRY_INT;
    }
}

/* Resuelve una entrada de la pila que representa texto a `out`.
 *  REF  -> literal del pool de constantes
 *  OBJ  -> String real del heap (resultado de concatenar / toString)
 *  FLOAT-> su valor en decimal; INT -> en decimal
 * Devuelve la longitud escrita. Es el corazon de como la VM pasa texto
 * entre bytecodes, natives y el StringBuilder. */
int jvm_entry_text(StackEntry *e, SimpleConstantPool *p,
                   char *out, int max)
{
    out[0] = 0;
    if (e == 0 || max <= 0)
        return 0;
    switch (e->type) {
    case STACK_ENTRY_REF: {
        ConstantStringRef *s = findStringRef(p, EntryToInt(e));
        if (s != 0) {
            getUTF8String(p, s->stringIndex, max - 1, out);
            return (int) strlen(out);
        }
        return 0;
    }
    case STACK_ENTRY_OBJ: {
        int ref = EntryToInt(e);
        if (heap_is_string(ref)) {
            const char *t = heap_string_text(ref);
            int n = (int) strlen(t);
            if (n > max - 1) n = max - 1;
            memcpy(out, t, (size_t) n);
            out[n] = 0;
            return n;
        }
        return snprintf(out, (size_t) max, "<obj@%d>", ref);
    }
    case STACK_ENTRY_FLOAT:
        return snprintf(out, (size_t) max, "%g",
                        (double) EntryToFloat(e));
    case STACK_ENTRY_INT:
    default:
        return snprintf(out, (size_t) max, "%d", EntryToInt(e));
    }
}

/* saca una entrada de la pila y la resuelve a texto (ver jvm_entry_text) */
int jvm_pop_text(StackFrame *stack, SimpleConstantPool *p,
                 char *out, int max)
{
    out[0] = 0;
    if (stack->size <= 0)
        return 0;
    return jvm_entry_text(popEntry(stack), p, out, max);
}

/* las natives de consola (HW.print/println) llaman a esto: ya es solo
 * pop+resolucion, porque la concatenacion deja un String real del heap */
int jvm_pop_string(StackFrame *stack, SimpleConstantPool *p,
                   char *out, int max)
{
    return jvm_pop_text(stack, p, out, max);
}
/* ==== fin bloque p4-sdk ==== */

#if SIMPLE_JVM_DEBUG
static void printCodeAttribute(CodeAttribute *ca, SimpleConstantPool *p);
#endif

static double get_double_parameter(StackFrame *stack, SimpleConstantPool *p)
{
    double value = 0.0f;
    if (is_ref_entry(stack)) {
        int index = popInt(stack);
        value = get_double_from_constant_pool(p, index);
#if SIMPLE_JVM_DEBUG
        printf("index %d\n", index);
        printf("get value from constant pool = %f\n", value);
#endif
    } else {
        value = popDouble(stack);
#if SIMPLE_JVM_DEBUG
        printf("get value from stack = %f\n", value);
#endif
    }
    return value;
}

/* opcode implementation */

/* aload_0 */
static int op_aload_0(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    (void) p;
    local_load(0, stack);
    *opCode = *opCode + 1;
    return 0;
}

/* bipush */
static int op_bipush(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    int value = (signed char) opCode[0][1];   /* con signo (byte) */
    pushInt(stack, value);
#if SIMPLE_JVM_DEBUG
    printf("push a byte %d onto the stack \n", value);
#endif
    *opCode = *opCode + 2;
    return 0;
}

/* dup: duplica la entrada de arriba PRESERVANDO su tipo (antes degradaba
 * OBJ a REF, lo que rompia `new String(...)` y cualquier objeto en el heap
 * que se pasara por dup antes de un constructor). */
static int op_dup(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    StackEntry *entry = popEntry(stack);
    int value = EntryToInt(entry);
    int type = entry->type;
    int k;
    for (k = 0; k < 2; k++) {
        switch (type) {
        case STACK_ENTRY_OBJ:
            pushObj(stack, value);
            break;
        case STACK_ENTRY_REF:
            pushRef(stack, value);
            break;
        case STACK_ENTRY_FLOAT: {
            float f;
            memcpy(&f, &value, sizeof(f));   /* bits -> float */
            pushFloat(stack, f);
            break;
        }
        default:
            pushInt(stack, value);
            break;
        }
    }
#if SIMPLE_JVM_DEBUG
    printf("dup\n");
#endif
    *opCode = *opCode + 1;
    return 0;
}

/* getstatic */
static int op_getstatic(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    u2 field_index ;
    unsigned char tmp[2];
    char clsName[255], fldName[255];
    tmp[0] = opCode[0][1];
    tmp[1] = opCode[0][2];
    field_index = tmp[0] << 8 | tmp[1];
#if SIMPLE_JVM_DEBUG
    printf("getstatic %d\n", field_index);
#endif
    *opCode = *opCode + 3;
    /* campo estatico de nuestra clase? */
    ConstantFieldRef *fRef = findFieldRefByCPIndex(p, field_index);
    if (fRef != 0) {
        ConstantClassRef *clasz = findClassRef(p, fRef->classIndex);
        ConstantNameAndType *nat = findNameAndType(p, fRef->nameAndTypeIndex);
        if (clasz != 0 && nat != 0) {
            getUTF8String(p, clasz->stringIndex, 255, clsName);
            if (strcmp(clsName, jvm_class_name) == 0) {
                int pos;
                getUTF8String(p, nat->nameIndex, 255, fldName);
                pos = field_pos(p, fldName, 1);
                if (pos < 0 || pos >= JVM_MAX_STATICS) {
                    fprintf(stderr, "[jvm] getstatic: campo estatico desconocido %s\n", fldName);
                    exit(4);
                }
                if (field_is_float(p, fldName, 1)) {
                    float f;
                    int bits = jvm_statics[pos];
                    memcpy(&f, &bits, sizeof(float));
                    pushFloat(stack, f);
                } else if (field_is_ref(p, fldName, 1)) {
                    pushObj(stack, jvm_statics[pos]);
                } else {
                    pushInt(stack, jvm_statics[pos]);
                }
                return 0;
            }
        }
    }
    /* otro caso (p.ej. System.out): comportamiento original */
    pushRef(stack, field_index);
    return 0;
}

/* iadd */
static int op_iadd(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    int value1 = popInt(stack);
    int value2 = popInt(stack);
    int result = 0;
    result = value1 + value2;
#if SIMPLE_JVM_DEBUG
    printf("iadd: %d + %d = %d\n", value1, value2, result);
#endif
    pushInt(stack, result);
    *opCode = *opCode + 1;
    return 0;
}

/* iconst_m1 */
static int op_iconst_m1(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    pushInt(stack, -1);
    (void) p;
    *opCode = *opCode + 1;
    return 0;
}

/* iconst_0 */
static int op_iconst_0(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    pushInt(stack, 0);
#if SIMPLE_JVM_DEBUG
    printf("iconst_0: push 0 into stack\n");
#endif
    *opCode = *opCode + 1;
    return 0;
}

/* iconst_1 */
static int op_iconst_1(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    pushInt(stack, 1);
#if SIMPLE_JVM_DEBUG
    printf("iconst_1: push 1 into stack\n");
#endif
    *opCode = *opCode + 1;
    return 0;
}

/* iconst_2 */
static int op_iconst_2(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    pushInt(stack, 2);
#if SIMPLE_JVM_DEBUG
    printf("iconst_2: push 1 into stack\n");
#endif
    *opCode = *opCode + 1;
    return 0;
}

/* iconst_3 */
static int op_iconst_3(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    pushInt(stack, 3);
#if SIMPLE_JVM_DEBUG
    printf("iconst_3: push 1 into stack\n");
#endif
    *opCode = *opCode + 1;
    return 0;
}

/* iconst_4 */
static int op_iconst_4(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    pushInt(stack, 4);
#if SIMPLE_JVM_DEBUG
    printf("iconst_4: push 1 into stack\n");
#endif
    *opCode = *opCode + 1;
    return 0;
}

/* iconst_5 */
static int op_iconst_5(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    pushInt(stack, 5);
#if SIMPLE_JVM_DEBUG
    printf("iconst_5: push 5 into stack\n");
#endif
    *opCode = *opCode + 1;
    return 0;
}

/* 0x0F dconst_1 */
static int op_dconst_1(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    pushDouble(stack, 1.0f);
#if SIMPLE_JVM_DEBUG
    printf("iconst_5: push 1.0f into stack\n");
#endif
    *opCode = *opCode + 1;
    return 0;
}

/* idiv */
static int op_idiv(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    int value2 = popInt(stack);
    int value1 = popInt(stack);
    int result = 0;
    result = value1 / value2;
#if SIMPLE_JVM_DEBUG
    printf("idiv: %d / %d = %d\n", value1, value2, result);
#endif
    pushInt(stack, result);
    *opCode = *opCode + 1;
    return 0;
}

/* iload */
static int op_iload(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    int index = opCode[0][1];
    (void) p;
    local_load(index, stack);
    *opCode = *opCode + 2;
    return 0;
}

/* iload_1 */
static int op_iload_1(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    (void) p;
    local_load(1, stack);
    *opCode = *opCode + 1;
    return 0;
}

/* iload_2 */
static int op_iload_2(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    (void) p;
    local_load(2, stack);
    *opCode = *opCode + 1;
    return 0;
}

/* iload_3 */
static int op_iload_3(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    (void) p;
    local_load(3, stack);
    *opCode = *opCode + 1;
    return 0;
}

/* imul */
static int op_imul(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    int value1 = popInt(stack);
    int value2 = popInt(stack);
    int result = 0;
    result = value1 * value2;
#if SIMPLE_JVM_DEBUG
    printf("imul: %d * %d = %d\n", value1, value2, result);
#endif
    pushInt(stack, result);
    *opCode = *opCode + 1;
    return 0;
}

/* 0x63 dadd */
static int op_dadd(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    double value1 = get_double_parameter(stack, p);
    double value2 = get_double_parameter(stack, p);
    double result = 0;
    result = value1 + value2;
#if SIMPLE_JVM_DEBUG
    printf("dadd: %f + %f = %f\n", value1, value2, result);
#endif
    pushDouble(stack, result);
    *opCode = *opCode + 1;
    return 0;
}

/* 0x6B dmul */
static int op_dmul(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    double value1 = get_double_parameter(stack, p);
    double value2 = get_double_parameter(stack, p);
    double result = 0;
    result = value1 * value2;
#if SIMPLE_JVM_DEBUG
    printf("dmul: %f * %f = %f\n", value1, value2, result);
#endif
    pushDouble(stack, result);
    *opCode = *opCode + 1;
    return 0;
}

/* 0x8e d2i */
static int op_d2i(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    double value1 = popDouble(stack);
    int result = 0;
    result = (int)value1;
#if SIMPLE_JVM_DEBUG
    printf("d2i: %d <-- %f\n", result, value1);
#endif
    pushInt(stack, result);
    *opCode = *opCode + 1;
    return 0;
}

/* irem */
static int op_irem(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    int value1 = popInt(stack);
    int value2 = popInt(stack);
    int result = 0;
    result = value2 % value1;
#if SIMPLE_JVM_DEBUG
    printf("irem: %d mod %d = %d\n", value2, value1, result);
#endif
    pushInt(stack, result);
    *opCode = *opCode + 1;
    return 0;
}

/* 0x78 ishl */
static int __attribute__((section(".slowtext"), noinline)) op_ishl(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    int shift = popInt(stack);
    int value = popInt(stack);
    int result = value << (shift & 0x1F);
    (void) p;
#if SIMPLE_JVM_DEBUG
    printf("ishl: %d << %d = %d\n", value, shift, result);
#endif
    pushInt(stack, result);
    *opCode = *opCode + 1;
    return 0;
}

/* 0x7A ishr */
static int __attribute__((section(".slowtext"), noinline)) op_ishr(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    int shift = popInt(stack);
    int value = popInt(stack);
    int result = value >> (shift & 0x1F);
    (void) p;
#if SIMPLE_JVM_DEBUG
    printf("ishr: %d >> %d = %d\n", value, shift, result);
#endif
    pushInt(stack, result);
    *opCode = *opCode + 1;
    return 0;
}

/* 0x7C iushr */
static int __attribute__((section(".slowtext"), noinline)) op_iushr(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    int shift = popInt(stack);
    int value = popInt(stack);
    int result = (int)((unsigned int)value >> (shift & 0x1F));
    (void) p;
#if SIMPLE_JVM_DEBUG
    printf("iushr: %u >> %d = %d\n", (unsigned)value, shift, result);
#endif
    pushInt(stack, result);
    *opCode = *opCode + 1;
    return 0;
}

/* 0x7E iand */
static int __attribute__((section(".slowtext"), noinline)) op_iand(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    int value2 = popInt(stack);
    int value1 = popInt(stack);
    int result = value1 & value2;
    (void) p;
#if SIMPLE_JVM_DEBUG
    printf("iand: %d & %d = %d\n", value1, value2, result);
#endif
    pushInt(stack, result);
    *opCode = *opCode + 1;
    return 0;
}

/* 0x80 ior */
static int __attribute__((section(".slowtext"), noinline)) op_ior(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    int value2 = popInt(stack);
    int value1 = popInt(stack);
    int result = value1 | value2;
    (void) p;
#if SIMPLE_JVM_DEBUG
    printf("ior: %d | %d = %d\n", value1, value2, result);
#endif
    pushInt(stack, result);
    *opCode = *opCode + 1;
    return 0;
}

/* 0x82 ixor */
static int __attribute__((section(".slowtext"), noinline)) op_ixor(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    int value2 = popInt(stack);
    int value1 = popInt(stack);
    int result = value1 ^ value2;
    (void) p;
#if SIMPLE_JVM_DEBUG
    printf("ixor: %d ^ %d = %d\n", value1, value2, result);
#endif
    pushInt(stack, result);
    *opCode = *opCode + 1;
    return 0;
}

/* istore */
static int op_istore(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    int index = opCode[0][1];
    (void) p;
    local_store(index, stack);
    *opCode = *opCode + 2;
    return 0;
}

/* istore_1 */
static int op_istore_1(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    (void) p;
    local_store(1, stack);
    *opCode = *opCode + 1;
    return 0;
}

/* istore_2 */
static int op_istore_2(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    (void) p;
    local_store(2, stack);
    *opCode = *opCode + 1;
    return 0;
}

/* istore_3 */
static int op_istore_3(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    (void) p;
    local_store(3, stack);
    *opCode = *opCode + 1;
    return 0;
}

/* isub */
static int op_isub(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    int value2 = popInt(stack);
    int value1 = popInt(stack);
    int result = 0;
    result = value1 - value2;
#if SIMPLE_JVM_DEBUG
    printf("isub : %d - %d = %d\n", value1, value2, result);
#endif
    pushInt(stack, result);
    *opCode = *opCode + 1;
    return 0;
}

/* invokespecial: constructores y metodos de instancia (no-virtual) */
static int op_invokespecial(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    u2 method_index;
    unsigned char tmp[2];
    char method_name[255], clsName[255], method_type[255];
    MethodInfo *m;

    tmp[0] = opCode[0][1];
    tmp[1] = opCode[0][2];
    method_index = tmp[0] << 8 | tmp[1];
    *opCode = *opCode + 3;

    ConstantMethodRef *mRef = findMethodRef(p, method_index);
    if (mRef == 0) return 0;
    ConstantClassRef *clasz = findClassRef(p, mRef->classIndex);
    ConstantNameAndType *nat = findNameAndType(p, mRef->nameAndTypeIndex);
    if (clasz == 0 || nat == 0) return -1;
    getUTF8String(p, clasz->stringIndex, 255, clsName);
    getUTF8String(p, nat->nameIndex, 255, method_name);
    getUTF8String(p, nat->typeIndex, 255, method_type);

    /* super() de Object: no hay cadena de herencia, solo descarta this */
    if (strcmp(clsName, "java/lang/Object") == 0 &&
        strcmp(method_name, "<init>") == 0) {
        popInt(stack);
        return 0;
    }
    /* constructor del StringBuilder (hack de strings) */
    if (strcmp(clsName, "java/lang/StringBuilder") == 0 &&
        strcmp(method_name, "<init>") == 0) {
        popInt(stack);
        return 0;
    }
    /* constructor de String: `new` dejo un objeto (slot) y el `dup` una
     * segunda referencia al MISMO slot; lo mutamos a String real. */
    if (strcmp(clsName, "java/lang/String") == 0 &&
        strcmp(method_name, "<init>") == 0) {
        if (strcmp(method_type, "()V") == 0) {
            heap_init_string(popInt(stack), "");
            return 0;
        }
        if (strcmp(method_type, "(Ljava/lang/String;)V") == 0) {
            char txt[1024];
            jvm_pop_text(stack, p, txt, sizeof(txt));   /* el arg (arriba) */
            heap_init_string(popInt(stack), txt);       /* this (debajo)   */
            return 0;
        }
        if (strcmp(method_type, "([C)V") == 0 ||
            strcmp(method_type, "([B)V") == 0) {
            char txt[1024];
            int n, i, arr = popInt(stack);              /* el array (arriba) */
            n = heap_array_length(arr);
            if (n > (int) sizeof(txt) - 1)
                n = (int) sizeof(txt) - 1;
            for (i = 0; i < n; i++)
                txt[i] = (char) heap_array_load(arr, i);
            txt[n] = 0;
            heap_init_string(popInt(stack), txt);       /* this */
            return 0;
        }
        /* otra firma de String.<init>: cae al error de no soportado */
    }
    /* metodo de nuestra clase: ejecutarlo con frame propio */
    m = findMethodByNameDesc(p, method_name, method_type);
    if (m != 0)
        return invoke_resolved(m, stack, p, 1);
    fprintf(stderr, "[jvm] invokespecial no soportado: %s.%s%s\n",
            clsName, method_name, method_type);
    exit(4);
}

static char *clzNamePrint = "java/io/PrintStream";
static char *clzNameStrBuilder = "java/lang/StringBuilder";

/* 0xb8 invokestatic */
static int op_invokestatic(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    u2 method_index;
    unsigned char tmp[2];
    char method_name[255], clsName[255], method_type[255];
    MethodInfo *m;

    tmp[0] = opCode[0][1];
    tmp[1] = opCode[0][2];
    method_index = tmp[0] << 8 | tmp[1];
    *opCode = *opCode + 3;

    ConstantMethodRef *mRef = findMethodRef(p, method_index);
    if (mRef == 0) return 0;
    ConstantClassRef *clasz = findClassRef(p, mRef->classIndex);
    ConstantNameAndType *nat = findNameAndType(p, mRef->nameAndTypeIndex);
    if (clasz == 0 || nat == 0) return -1;
    getUTF8String(p, clasz->stringIndex, 255, clsName);
    getUTF8String(p, nat->nameIndex, 255, method_name);
    getUTF8String(p, nat->typeIndex, 255, method_type);
#if SIMPLE_JVM_DEBUG
    printf("invokestatic %s.%s%s\n", clsName, method_name, method_type);
#endif
    /* natives (java.lang.* y libreria esp32p4.HW) */
    if (invoke_java_lang_library(stack, p, clsName, method_name, method_type))
        return 0;
    /* metodo Java real del pool */
    m = findMethodByNameDesc(p, method_name, method_type);
    if (m != 0)
        return invoke_resolved(m, stack, p, 0);
    fprintf(stderr, "[jvm] metodo no encontrado: %s.%s%s\n",
            clsName, method_name, method_type);
    exit(4);
}

/* invokevirtual */
static int op_invokevirtual(unsigned char **opCode, StackFrame *stack,
                            SimpleConstantPool *p)
{
    u2 object_ref;
    unsigned char tmp[2];
    char clsName[255];
    char method_name[255];
    char method_type[255];
    tmp[0] = opCode[0][1];
    tmp[1] = opCode[0][2];
    object_ref = tmp[0] << 8 | tmp[1];
#if SIMPLE_JVM_DEBUG
    printf("call object_ref %d\n", object_ref);
#endif
    *opCode = *opCode + 3;
    ConstantMethodRef *mRef = findMethodRef(p, object_ref);
    if (mRef != 0) {
        ConstantClassRef *clasz = findClassRef(p, mRef->classIndex);
        ConstantNameAndType *nat = findNameAndType(p, mRef->nameAndTypeIndex);
        if (clasz == 0 || nat == 0) return -1;
        getUTF8String(p, clasz->stringIndex, 255, clsName);
        getUTF8String(p, nat->nameIndex, 255, method_name);
        getUTF8String(p, nat->typeIndex, 255, method_type);
#if SIMPLE_JVM_DEBUG
        printf("call object ref class %s\n", clsName);
#endif
        if (strcmp(clzNamePrint, clsName) == 0) {
            /* System.out.print/println: resuelve el argumento (literal
             * del pool, String del heap por concatenacion, int o float)
             * y lo escribe. Tambien descarta el objectref (System.out)
             * que empuja getstatic, para no dejar basura en la pila. */
            char text[1024];
            if (strcmp(method_type, "()V") != 0)
                jvm_entry_text(popEntry(stack), p, text, sizeof(text));
            else
                text[0] = 0;
            popEntry(stack);                /* descarta System.out */
            if (strcmp(method_name, "println") == 0)
                printf("%s\n", text);
            else
                printf("%s", text);
        } else if (strcmp(clzNameStrBuilder, clsName) == 0) {
            /* StringBuilder real: cada objeto tiene su propio buffer y
             * toString deja un String del heap en la pila. */
            if (strcmp(method_name, "append") == 0) {
                StackEntry *arg = popEntry(stack);
                StackEntry *sbe = popEntry(stack);
                int ref = EntryToInt(sbe);
                char text[1024];
                jvm_entry_text(arg, p, text, sizeof(text));
                heap_sbuilder_append(ref, text);
                pushObj(stack, ref);        /* append devuelve this */
            } else if (strcmp(method_name, "toString") == 0) {
                StackEntry *sbe = popEntry(stack);
                pushObj(stack,
                        heap_new_string(heap_sbuilder_text(EntryToInt(sbe))));
            } else if (strcmp(method_name, "length") == 0) {
                StackEntry *sbe = popEntry(stack);
                pushInt(stack, heap_sbuilder_len(EntryToInt(sbe)));
            } else {
                fprintf(stderr, "[jvm] StringBuilder.%s no soportado\n",
                        method_name);
                exit(4);
            }
        } else if (strcmp(clsName, "java/lang/String") == 0) {
            /* metodos de String sobre el receiver (literal del pool o
             * String del heap producido por concatenar) */
            char text[1024];
            if (strcmp(method_name, "length") == 0) {
                jvm_entry_text(popEntry(stack), p, text, sizeof(text));
                pushInt(stack, (int) strlen(text));
            } else if (strcmp(method_name, "isEmpty") == 0) {
                jvm_entry_text(popEntry(stack), p, text, sizeof(text));
                pushInt(stack, text[0] == 0 ? 1 : 0);
            } else if (strcmp(method_name, "charAt") == 0) {
                int idx = popInt(stack);
                int len;
                jvm_entry_text(popEntry(stack), p, text, sizeof(text));
                len = (int) strlen(text);
                pushInt(stack, (idx >= 0 && idx < len)
                                   ? (unsigned char) text[idx] : 0);
            } else if (strcmp(method_name, "equals") == 0) {
                char other[1024];
                StackEntry *arg = popEntry(stack);
                jvm_entry_text(popEntry(stack), p, text, sizeof(text));
                jvm_entry_text(arg, p, other, sizeof(other));
                pushInt(stack, strcmp(text, other) == 0 ? 1 : 0);
            } else if (invoke_java_lang_library(stack, p, clsName,
                                                method_name, method_type)) {
                /* metodo de String registrado en la tabla (java_std.c):
                 * substring, indexOf, toUpperCase, trim, ... */
            } else {
                fprintf(stderr, "[jvm] String.%s no soportado\n", method_name);
                exit(4);
            }
        } else {
            /* metodo de instancia de nuestra clase */
            MethodInfo *m = findMethodByNameDesc(p, method_name, method_type);
            if (m != 0)
                return invoke_resolved(m, stack, p, 1);
            if (invoke_java_lang_library(stack, p, clsName, method_name,
                                         method_type))
                return 0;
            fprintf(stderr, "[jvm] invokevirtual no soportado: %s.%s%s\n",
                    clsName, method_name, method_type);
            exit(4);
        }
    }
    return 0;
}

/* ldc: resuelve la constante segun su tag. String -> REF al pool (se
 * resuelve luego al imprimir/concatenar); Integer -> su valor; Float ->
 * empuja un FLOAT real. (Antes siempre empujaba REF, lo que rompia los
 * enteros grandes y los floats.) */
static int op_ldc(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    int index = opCode[0][1];
    int tag = cp_probe_type(p, index);

    if (tag == CONSTANT_FLOAT)
        pushFloat(stack, get_float_from_constant_pool(p, index));
    else if (tag == CONSTANT_INTEGER)
        pushInt(stack, get_integer_from_constant_pool(p, index));
    else
        pushRef(stack, index);      /* string u otro: REF al pool */
#if SIMPLE_JVM_DEBUG
    printf("ldc: push constant index %d (tag %d)\n", index, tag);
#endif
    *opCode = *opCode + 2;
    return 0;
}

/* 0x13 ldc_w: lo mismo que ldc pero con indice de dos bytes.  javac cambia a
 * esta forma solo cuando el constant pool pasa de 255 entradas --con apps de
 * dos pantallas y sus literales se llega--, asi que sin esto la app arranca y
 * aborta en mitad de main. */
static int op_ldc_w(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    int index = (opCode[0][1] << 8) | opCode[0][2];
    int tag = cp_probe_type(p, index);

    if (tag == CONSTANT_FLOAT)
        pushFloat(stack, get_float_from_constant_pool(p, index));
    else if (tag == CONSTANT_INTEGER)
        pushInt(stack, get_integer_from_constant_pool(p, index));
    else
        pushRef(stack, index);      /* string u otro: REF al pool */
#if SIMPLE_JVM_DEBUG
    printf("ldc_w: push constant index %d (tag %d)\n", index, tag);
#endif
    *opCode = *opCode + 3;
    return 0;
}

/* 0x14 ldc2_w */
static int op_ldc2_w(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    unsigned char index1 = opCode[0][1];
    unsigned char index2 = opCode[0][2];
    int index = (index1 << 8) | index2;
    pushRef(stack, index);
#if SIMPLE_JVM_DEBUG
    printf("ldc2_w: push a constant index %d onto the stack \n", index);
#endif
    *opCode = *opCode + 3;
    return 0;
}

/* 0x11 op_sipush */
static int op_sipush(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    short value;
    unsigned char tmp[2];
    tmp[0] = opCode[0][1];
    tmp[1] = opCode[0][2];
    value = tmp[0] << 8 | tmp[1];
#if SIMPLE_JVM_DEBUG
    printf("sipush value %d\n", value);
#endif
    pushInt(stack, value);
    *opCode = *opCode + 3;
    return 0;
}

/* op_new: StringBuilder -> slot de builder; si no, objeto con un slot
 * por campo de instancia */
static int op_new(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    int i, nfields = 0;
    int classIndex = (opCode[0][1] << 8) | opCode[0][2];
    char clsName[255];
    ConstantClassRef *cr;

    clsName[0] = 0;
    cr = findClassRef(p, classIndex);
    if (cr != 0)
        getUTF8String(p, cr->stringIndex, 254, clsName);
    if (strcmp(clsName, "java/lang/StringBuilder") == 0) {
        pushObj(stack, heap_new_sbuilder());
        *opCode = *opCode + 3;
        return 0;
    }
    for (i = 0; i < simpleFieldPool.field_used; i++)
        if (!(simpleFieldPool.field[i].access_flags & 0x0008))
            nfields++;
    pushObj(stack, heap_new_object(nfields));
#if SIMPLE_JVM_DEBUG
    printf("new: objeto con %d campos\n", nfields);
#endif
    *opCode = *opCode + 3;
    return 0;
}

/* return */
static int op_return(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
#if SIMPLE_JVM_DEBUG
    printf("return: \n");
#endif
    *opCode = *opCode + 1;
    return -1;
}

/* ==== p4-sdk: ramas, locals, arrays, campos y switches ==== */

static int branch_take(unsigned char **opCode, int cond)
{
    short off = (short) ((opCode[0][1] << 8) | opCode[0][2]);
    *opCode = cond ? *opCode + off : *opCode + 3;
    return 0;
}

static int op_goto(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    short off = (short) ((opCode[0][1] << 8) | opCode[0][2]);
    (void) stack;
    (void) p;
    *opCode = *opCode + off;
    return 0;
}

#define DEFINE_IF0(opname, expr)                                     \
static int opname(unsigned char **opCode, StackFrame *stack,         \
                  SimpleConstantPool *p)                             \
{                                                                    \
    int v = popInt(stack);                                           \
    (void) p;                                                        \
    return branch_take(opCode, (expr));                              \
}

DEFINE_IF0(op_ifeq,  v == 0)
DEFINE_IF0(op_ifne,  v != 0)
DEFINE_IF0(op_iflt,  v < 0)
DEFINE_IF0(op_ifle,  v <= 0)
DEFINE_IF0(op_ifgt,  v > 0)
DEFINE_IF0(op_ifge,  v >= 0)

#define DEFINE_IFCMP(opname, expr)                                   \
static int opname(unsigned char **opCode, StackFrame *stack,         \
                  SimpleConstantPool *p)                             \
{                                                                    \
    int v2 = popInt(stack);                                          \
    int v1 = popInt(stack);                                          \
    (void) p;                                                        \
    return branch_take(opCode, (expr));                              \
}

DEFINE_IFCMP(op_if_icmpeq, v1 == v2)
DEFINE_IFCMP(op_if_icmpne, v1 != v2)
DEFINE_IFCMP(op_if_icmplt, v1 < v2)
DEFINE_IFCMP(op_if_icmple, v1 <= v2)
DEFINE_IFCMP(op_if_icmpgt, v1 > v2)
DEFINE_IFCMP(op_if_icmpge, v1 >= v2)
DEFINE_IFCMP(op_if_acmpeq, v1 == v2)
DEFINE_IFCMP(op_if_acmpne, v1 != v2)

static int op_ifnull(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    int v = popInt(stack);
    (void) p;
    return branch_take(opCode, v == 0);
}

static int op_ifnonnull(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    int v = popInt(stack);
    (void) p;
    return branch_take(opCode, v != 0);
}

/* locals: variantes _0.._3 y genericas (int y ref son lo mismo aqui) */
static int op_iload_0(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    (void) p;
    local_load(0, stack);
    *opCode = *opCode + 1;
    return 0;
}

static int op_istore_0(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    (void) p;
    local_store(0, stack);
    *opCode = *opCode + 1;
    return 0;
}

static int op_aload(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    int index = opCode[0][1];
    (void) p;
    local_load(index, stack);
    *opCode = *opCode + 2;
    return 0;
}

static int op_aload_1(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    (void) p;
    local_load(1, stack);
    *opCode = *opCode + 1;
    return 0;
}

static int op_aload_2(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    (void) p;
    local_load(2, stack);
    *opCode = *opCode + 1;
    return 0;
}

static int op_aload_3(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    (void) p;
    local_load(3, stack);
    *opCode = *opCode + 1;
    return 0;
}

static int op_astore(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    int index = opCode[0][1];
    (void) p;
    local_store(index, stack);
    *opCode = *opCode + 2;
    return 0;
}

static int op_astore_0(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    (void) p;
    local_store(0, stack);
    *opCode = *opCode + 1;
    return 0;
}

static int op_astore_1(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    (void) p;
    local_store(1, stack);
    *opCode = *opCode + 1;
    return 0;
}

static int op_astore_2(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    (void) p;
    local_store(2, stack);
    *opCode = *opCode + 1;
    return 0;
}

static int op_astore_3(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    (void) p;
    local_store(3, stack);
    *opCode = *opCode + 1;
    return 0;
}

/* pila */
static int op_nop(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    (void) stack;
    (void) p;
    *opCode = *opCode + 1;
    return 0;
}

static int op_aconst_null(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    (void) p;
    pushRef(stack, 0);
    *opCode = *opCode + 1;
    return 0;
}

static int op_pop(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    (void) p;
    popEntry(stack);
    *opCode = *opCode + 1;
    return 0;
}

static int op_pop2(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    (void) p;
    popEntry(stack);
    popEntry(stack);
    *opCode = *opCode + 1;
    return 0;
}

/* empuja `v` con el tipo de entrada que tenia: un OBJ (referencia del heap)
 * no puede degradarse a REF (indice del pool) o gc.c dejaria de verlo. */
static void push_as(StackFrame *stack, int v, int type)
{
    switch (type) {
    case STACK_ENTRY_OBJ:   pushObj(stack, v); break;
    case STACK_ENTRY_REF:   pushRef(stack, v); break;
    case STACK_ENTRY_FLOAT: {
        float f;
        memcpy(&f, &v, sizeof(f));
        pushFloat(stack, f);
        break;
    }
    default:                pushInt(stack, v); break;
    }
}

static int op_swap(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    StackEntry *e1 = popEntry(stack);
    StackEntry *e2 = popEntry(stack);
    int v1 = EntryToInt(e1);
    int v2 = EntryToInt(e2);
    int t1 = e1->type;
    int t2 = e2->type;
    (void) p;
    push_as(stack, v1, t1);
    push_as(stack, v2, t2);
    *opCode = *opCode + 1;
    return 0;
}

/* returns: el valor se queda en la pila del frame; invoke_resolved lo recoge */
static int op_ireturn(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    (void) stack;
    (void) p;
    *opCode = *opCode + 1;
    return -1;
}

static int op_areturn(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    (void) stack;
    (void) p;
    *opCode = *opCode + 1;
    return -1;
}

/* iinc: locals[idx] += delta (delta con signo de 8 bits) */
static int op_iinc(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    (void) stack;
    (void) p;
    localVariables.integer[opCode[0][1]] += (signed char) opCode[0][2];
    *opCode = *opCode + 3;
    return 0;
}

/* tableswitch / lookupswitch (offsets big-endian, alineados a 4) */
static int32_t rd4(unsigned char *q)
{
    return (int32_t) (((uint32_t) q[0] << 24) | ((uint32_t) q[1] << 16) |
                      ((uint32_t) q[2] << 8) | (uint32_t) q[3]);
}

static int op_tableswitch(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    unsigned char *ins = *opCode;
    int pad = 3 - ((int) (ins - jvm_code_base) & 3);
    unsigned char *q = ins + 1 + pad;
    int32_t def = rd4(q), low, high, idx;
    (void) p;
    q += 4;
    low = rd4(q);
    q += 4;
    high = rd4(q);
    q += 4;
    idx = popInt(stack);
    if (idx < low || idx > high)
        *opCode = ins + def;
    else
        *opCode = ins + rd4(q + (idx - low) * 4);
    return 0;
}

static int op_lookupswitch(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    unsigned char *ins = *opCode;
    int pad = 3 - ((int) (ins - jvm_code_base) & 3);
    unsigned char *q = ins + 1 + pad;
    int32_t def = rd4(q), npairs, key, i;
    (void) p;
    q += 4;
    npairs = rd4(q);
    q += 4;
    key = popInt(stack);
    for (i = 0; i < npairs; i++) {
        if (rd4(q) == key) {
            *opCode = ins + rd4(q + 4);
            return 0;
        }
        q += 8;
    }
    *opCode = ins + def;
    return 0;
}

/* arrays: atype=byte (T_BYTE=8) crea un byte[] real (1 byte/elem); el resto
 * (int/char/short/boolean/float) sigue siendo un int[]. */
static int op_newarray(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    int atype = (int) opCode[0][1];
    int len = popInt(stack);
    (void) p;
    if (atype == 8)                     /* T_BYTE */
        pushObj(stack, heap_new_byte_array(len));
    else
        pushObj(stack, heap_new_int_array(len));
    *opCode = *opCode + 2;
    return 0;
}

static int op_iaload(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    int idx = popInt(stack);
    int ref = popInt(stack);
    (void) p;
    pushInt(stack, heap_array_load(ref, idx));
    *opCode = *opCode + 1;
    return 0;
}

static int op_iastore(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    int val = popInt(stack);
    int idx = popInt(stack);
    int ref = popInt(stack);
    (void) p;
    heap_array_store(ref, idx, val);
    *opCode = *opCode + 1;
    return 0;
}

static int op_arraylength(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    int ref = popInt(stack);
    (void) p;
    pushInt(stack, heap_array_length(ref));
    *opCode = *opCode + 1;
    return 0;
}

/* arrays de referencias (String[]/Object[]): aaload/aastore guardan el
 * tipo del elemento (REF del pool u OBJ del heap) para poder devolverlo
 * intacto; anewarray solo necesita la longitud (el tipo de elemento se
 * marca en el bytecode pero no hay comprobacion de tipos en esta VM). */
static int op_aaload(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    int idx = popInt(stack);
    int ref = popInt(stack);
    int type = STACK_ENTRY_INT;
    int v;
    (void) p;
    v = heap_ref_array_load(ref, idx, &type);
    if (type == STACK_ENTRY_REF)
        pushRef(stack, v);
    else if (type == STACK_ENTRY_OBJ)
        pushObj(stack, v);
    else
        pushInt(stack, v);
    *opCode = *opCode + 1;
    return 0;
}

static int op_aastore(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    StackEntry *val = popEntry(stack);
    int idx = popInt(stack);
    int ref = popInt(stack);
    (void) p;
    heap_ref_array_store(ref, idx, EntryToInt(val), val->type);
    *opCode = *opCode + 1;
    return 0;
}

static int op_anewarray(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    int len = popInt(stack);
    (void) p;
    pushObj(stack, heap_new_ref_array(len));
    *opCode = *opCode + 3;              /* opcode + u2 class index */
    return 0;
}

/* campos: getfield/putfield (instancia) y putstatic (estaticos) */
static void resolve_fieldref(SimpleConstantPool *p, u2 index,
                             char *clsName, char *fldName)
{
    ConstantFieldRef *fRef = findFieldRefByCPIndex(p, index);
    ConstantClassRef *clasz;
    ConstantNameAndType *nat;
    if (fRef == 0) {
        fprintf(stderr, "[jvm] fieldref %d ilegible\n", index);
        exit(4);
    }
    clasz = findClassRef(p, fRef->classIndex);
    nat = findNameAndType(p, fRef->nameAndTypeIndex);
    if (clasz == 0 || nat == 0) {
        fprintf(stderr, "[jvm] fieldref %d incompleto\n", index);
        exit(4);
    }
    getUTF8String(p, clasz->stringIndex, 255, clsName);
    getUTF8String(p, nat->nameIndex, 255, fldName);
}

static int op_getfield(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    char clsName[255], fldName[255];
    int pos, ref;
    resolve_fieldref(p, (u2) ((opCode[0][1] << 8) | opCode[0][2]), clsName, fldName);
    if (strcmp(clsName, jvm_class_name) != 0) {
        fprintf(stderr, "[jvm] getfield de otra clase: %s.%s\n", clsName, fldName);
        exit(4);
    }
    pos = field_pos(p, fldName, 0);
    if (pos < 0) {
        fprintf(stderr, "[jvm] getfield: campo inexistente %s\n", fldName);
        exit(4);
    }
    ref = popInt(stack);
    if (field_is_float(p, fldName, 0)) {
        float f;
        int bits = heap_object_get(ref, pos);
        memcpy(&f, &bits, sizeof(float));
        pushFloat(stack, f);
    } else if (field_is_ref(p, fldName, 0)) {
        pushObj(stack, heap_object_get(ref, pos));
    } else {
        pushInt(stack, heap_object_get(ref, pos));
    }
    *opCode = *opCode + 3;
    return 0;
}

static int op_putfield(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    char clsName[255], fldName[255];
    int pos, val, ref;
    resolve_fieldref(p, (u2) ((opCode[0][1] << 8) | opCode[0][2]), clsName, fldName);
    if (strcmp(clsName, jvm_class_name) != 0) {
        fprintf(stderr, "[jvm] putfield de otra clase: %s.%s\n", clsName, fldName);
        exit(4);
    }
    pos = field_pos(p, fldName, 0);
    if (pos < 0) {
        fprintf(stderr, "[jvm] putfield: campo inexistente %s\n", fldName);
        exit(4);
    }
    val = popInt(stack);
    ref = popInt(stack);
    heap_object_put(ref, pos, val);
    *opCode = *opCode + 3;
    return 0;
}

static int op_putstatic(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    char clsName[255], fldName[255];
    int pos;
    resolve_fieldref(p, (u2) ((opCode[0][1] << 8) | opCode[0][2]), clsName, fldName);
    if (strcmp(clsName, jvm_class_name) != 0) {
        /* estatico ajeno: se ignora (solo existe System.out de lectura) */
        *opCode = *opCode + 3;
        return 0;
    }
    pos = field_pos(p, fldName, 1);
    if (pos < 0 || pos >= JVM_MAX_STATICS) {
        fprintf(stderr, "[jvm] putstatic: campo estatico inexistente %s\n", fldName);
        exit(4);
    }
    jvm_statics[pos] = popInt(stack);
    *opCode = *opCode + 3;
    return 0;
}

/* sin-op utiles: checkcast (no hay clases) e instanceof (todo es posible) */
static int op_checkcast(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    (void) stack;
    (void) p;
    *opCode = *opCode + 3;
    return 0;
}

static int op_instanceof(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    (void) popInt(stack);
    (void) p;
    pushInt(stack, 1);
    *opCode = *opCode + 3;
    return 0;
}
/* ---- float (p4-sdk): constants, locales, aritmetica, comparacion,
 * conversiones y return. El float vive en el mismo hueco de 4 bytes del
 * slot; STACK_ENTRY_FLOAT marca su presencia. (frem/fmod y long/double
 * no se soportan: sin libreria math en el objetivo.) */

static int op_fconst_0(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{ (void) p; pushFloat(stack, 0.0f); *opCode += 1; return 0; }
static int op_fconst_1(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{ (void) p; pushFloat(stack, 1.0f); *opCode += 1; return 0; }
static int op_fconst_2(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{ (void) p; pushFloat(stack, 2.0f); *opCode += 1; return 0; }

static int op_fload(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{ (void) p; local_load(opCode[0][1], stack); *opCode += 2; return 0; }
static int op_fload_0(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{ (void) p; local_load(0, stack); *opCode += 1; return 0; }
static int op_fload_1(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{ (void) p; local_load(1, stack); *opCode += 1; return 0; }
static int op_fload_2(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{ (void) p; local_load(2, stack); *opCode += 1; return 0; }
static int op_fload_3(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{ (void) p; local_load(3, stack); *opCode += 1; return 0; }

static int op_fstore(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{ (void) p; local_store(opCode[0][1], stack); *opCode += 2; return 0; }
static int op_fstore_0(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{ (void) p; local_store(0, stack); *opCode += 1; return 0; }
static int op_fstore_1(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{ (void) p; local_store(1, stack); *opCode += 1; return 0; }
static int op_fstore_2(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{ (void) p; local_store(2, stack); *opCode += 1; return 0; }
static int op_fstore_3(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{ (void) p; local_store(3, stack); *opCode += 1; return 0; }

static int op_fadd(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    float b = popFloat(stack);
    float a = popFloat(stack);
    (void) p;
    pushFloat(stack, a + b);
    *opCode += 1;
    return 0;
}
static int op_fsub(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    float b = popFloat(stack);
    float a = popFloat(stack);
    (void) p;
    pushFloat(stack, a - b);
    *opCode += 1;
    return 0;
}
static int op_fmul(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    float b = popFloat(stack);
    float a = popFloat(stack);
    (void) p;
    pushFloat(stack, a * b);
    *opCode += 1;
    return 0;
}
static int op_fdiv(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    float b = popFloat(stack);
    float a = popFloat(stack);
    (void) p;
    pushFloat(stack, a / b);
    *opCode += 1;
    return 0;
}
static int op_fneg(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{ float a = popFloat(stack); (void) p; unsigned int u; memcpy(&u, &a, 4); u ^= 0x80000000u; memcpy(&a, &u, 4); pushFloat(stack, a); *opCode += 1; return 0; }

/* fcmpl/fcmpg: empujan -1/0/1 (NaN -> -1 en fcmpl, 1 en fcmpg) para
 * alimentar a iflt/ifge/ifgt/ifle/ifeq/ifne.  Las tres comparaciones C sobre
 * `float' son las que definen el orden: con NaN las tres dan falso, y ahi cae
 * la rama extra. */
static int op_fcmpl(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    float b = popFloat(stack);
    float a = popFloat(stack);
    int r;
    (void) p;
    if (a < b) r = -1;
    else if (a > b) r = 1;
    else if (a == b) r = 0;
    else r = -1;      /* NaN */
    pushInt(stack, r);
    *opCode += 1;
    return 0;
}
static int op_fcmpg(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    float b = popFloat(stack);
    float a = popFloat(stack);
    int r;
    (void) p;
    if (a < b) r = -1;
    else if (a > b) r = 1;
    else if (a == b) r = 0;
    else r = 1;       /* NaN */
    pushInt(stack, r);
    *opCode += 1;
    return 0;
}

static int op_i2f(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{ int v = popInt(stack); (void) p; pushFloat(stack, (float) v); *opCode += 1; return 0; }
static int op_f2i(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{ float f = popFloat(stack); (void) p; pushInt(stack, (int) f); *opCode += 1; return 0; }

/* conversiones estrechas (byte/char/short): truncar el int.
 * i2b -> (byte) signo, i2c -> (char) 0..65535, i2s -> (short) signo. */
static int op_i2b(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{ int v = popInt(stack); (void) p; pushInt(stack, (signed char) v); *opCode += 1; return 0; }
static int op_i2c(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{ int v = popInt(stack); (void) p; pushInt(stack, (int) (unsigned short) v); *opCode += 1; return 0; }
static int op_i2s(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{ int v = popInt(stack); (void) p; pushInt(stack, (short) v); *opCode += 1; return 0; }

/* freturn: el valor queda en la pila del frame; invoke_resolved lo
 * recoge con popFloat segun el descriptor de retorno */
static int op_freturn(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p)
{
    (void) stack;
    (void) p;
    *opCode += 1;
    return -1;
}
/* ==== fin instrucciones p4-sdk ==== */

static const byteCode byteCodes[] = {
    { "aload_0"         , 0x2A, 1,  op_aload_0          },
    { "bipush"          , 0x10, 2,  op_bipush           },
    { "dup"             , 0x59, 1,  op_dup              },
    { "getstatic"       , 0xB2, 3,  op_getstatic        },
    { "iadd"            , 0x60, 1,  op_iadd             },
    { "iconst_m1"       , 0x02, 1,  op_iconst_m1        },
    { "iconst_0"        , 0x03, 1,  op_iconst_0         },
    { "iconst_1"        , 0x04, 1,  op_iconst_1         },
    { "iconst_2"        , 0x05, 1,  op_iconst_2         },
    { "iconst_3"        , 0x06, 1,  op_iconst_3         },
    { "iconst_4"        , 0x07, 1,  op_iconst_4         },
    { "iconst_5"        , 0x08, 1,  op_iconst_5         },
    { "dconst_1"        , 0x0F, 1,  op_dconst_1         },
    { "idiv"            , 0x6C, 1,  op_idiv             },
    { "imul"            , 0x68, 1,  op_imul             },
    { "dadd"            , 0x63, 1,  op_dadd             },
    { "dmul"            , 0x6B, 1,  op_dmul             },
    { "d2i"             , 0x8e, 1,  op_d2i              },
    { "invokespecial"   , 0xB7, 3,  op_invokespecial    },
    { "invokevirtual"   , 0xB6, 3,  op_invokevirtual    },
    { "invokestatic"    , 0xB8, 3,  op_invokestatic     },
    { "iload"           , 0x15, 2,  op_iload            },
    { "iload_1"         , 0x1B, 1,  op_iload_1          },
    { "iload_2"         , 0x1C, 1,  op_iload_2          },
    { "iload_3"         , 0x1D, 1,  op_iload_3          },
    { "istore"          , 0x36, 2,  op_istore           },
    { "istore_1"        , 0x3C, 1,  op_istore_1         },
    { "istore_2"        , 0x3D, 1,  op_istore_2         },
    { "istore_3"        , 0x3E, 1,  op_istore_3         },
    { "isub"            , 0x64, 1,  op_isub             },
    { "ldc"             , 0x12, 2,  op_ldc              },
    { "ldc_w"           , 0x13, 3,  op_ldc_w            },
    { "ldc2_w"          , 0x14, 3,  op_ldc2_w           },
    { "new"             , 0xBB, 3,  op_new              },
    { "irem"            , 0x70, 1,  op_irem             },
    { "ishl"            , 0x78, 1,  op_ishl             },
    { "ishr"            , 0x7A, 1,  op_ishr             },
    { "iushr"           , 0x7C, 1,  op_iushr            },
    { "iand"            , 0x7E, 1,  op_iand             },
    { "ior"             , 0x80, 1,  op_ior              },
    { "ixor"            , 0x82, 1,  op_ixor             },
    { "sipush"          , 0x11, 3,  op_sipush           },
    { "return"          , 0xB1, 1,  op_return           },
    /* ---- p4-sdk: control de flujo, locals, pila, arrays, campos ---- */
    { "nop"             , 0x00, 1,  op_nop              },
    { "aconst_null"     , 0x01, 1,  op_aconst_null      },
    { "iload_0"         , 0x1A, 1,  op_iload_0          },
    { "istore_0"        , 0x3B, 1,  op_istore_0         },
    { "aload"           , 0x19, 2,  op_aload            },
    { "aload_1"         , 0x2B, 1,  op_aload_1          },
    { "aload_2"         , 0x2C, 1,  op_aload_2          },
    { "aload_3"         , 0x2D, 1,  op_aload_3          },
    { "astore"          , 0x3A, 2,  op_astore           },
    { "astore_0"        , 0x4B, 1,  op_astore_0         },
    { "astore_1"        , 0x4C, 1,  op_astore_1         },
    { "astore_2"        , 0x4D, 1,  op_astore_2         },
    { "astore_3"        , 0x4E, 1,  op_astore_3         },
    { "pop"             , 0x57, 1,  op_pop              },
    { "pop2"            , 0x58, 1,  op_pop2             },
    { "swap"            , 0x5F, 1,  op_swap             },
    { "ireturn"         , 0xAC, 1,  op_ireturn          },
    { "areturn"         , 0xB0, 1,  op_areturn          },
    { "goto"            , 0xA7, 3,  op_goto             },
    { "ifeq"            , 0x99, 3,  op_ifeq             },
    { "ifne"            , 0x9A, 3,  op_ifne             },
    { "iflt"            , 0x9B, 3,  op_iflt             },
    { "ifge"            , 0x9C, 3,  op_ifge             },
    { "ifgt"            , 0x9D, 3,  op_ifgt             },
    { "ifle"            , 0x9E, 3,  op_ifle             },
    { "if_icmpeq"       , 0x9F, 3,  op_if_icmpeq        },
    { "if_icmpne"       , 0xA0, 3,  op_if_icmpne        },
    { "if_icmplt"       , 0xA1, 3,  op_if_icmplt        },
    { "if_icmpge"       , 0xA2, 3,  op_if_icmpge        },
    { "if_icmpgt"       , 0xA3, 3,  op_if_icmpgt        },
    { "if_icmple"       , 0xA4, 3,  op_if_icmple        },
    { "if_acmpeq"       , 0xA5, 3,  op_if_acmpeq        },
    { "if_acmpne"       , 0xA6, 3,  op_if_acmpne        },
    { "ifnull"          , 0xC6, 3,  op_ifnull           },
    { "ifnonnull"       , 0xC7, 3,  op_ifnonnull        },
    { "iinc"            , 0x84, 3,  op_iinc             },
    { "tableswitch"     , 0xAA, 0,  op_tableswitch      },
    { "lookupswitch"    , 0xAB, 0,  op_lookupswitch     },
    { "newarray"        , 0xBC, 2,  op_newarray         },
    { "iaload"          , 0x2E, 1,  op_iaload           },
    { "iastore"         , 0x4F, 1,  op_iastore          },
    /* arrays byte/boolean (baload/bastore), char (caload/castore) y
     * short (saload/sastore): en esta VM todos son int[] => mismo codigo. */
    { "baload"          , 0x33, 1,  op_iaload           },
    { "caload"          , 0x34, 1,  op_iaload           },
    { "saload"          , 0x35, 1,  op_iaload           },
    { "bastore"         , 0x54, 1,  op_iastore          },
    { "castore"         , 0x55, 1,  op_iastore          },
    { "sastore"         , 0x56, 1,  op_iastore          },
    { "aaload"          , 0x32, 1,  op_aaload           },
    { "aastore"         , 0x53, 1,  op_aastore          },
    { "anewarray"       , 0xBD, 3,  op_anewarray        },
    { "arraylength"     , 0xBE, 1,  op_arraylength      },
    { "getfield"        , 0xB4, 3,  op_getfield         },
    { "putfield"        , 0xB5, 3,  op_putfield         },
    { "putstatic"       , 0xB3, 3,  op_putstatic        },
    { "checkcast"       , 0xC0, 3,  op_checkcast        },
    { "instanceof"      , 0xC1, 3,  op_instanceof       },
    /* ---- p4-sdk: float ---- */
    { "fconst_0"        , 0x0B, 1,  op_fconst_0         },
    { "fconst_1"        , 0x0C, 1,  op_fconst_1         },
    { "fconst_2"        , 0x0D, 1,  op_fconst_2         },
    { "fload"           , 0x17, 2,  op_fload            },
    { "fload_0"         , 0x22, 1,  op_fload_0          },
    { "fload_1"         , 0x23, 1,  op_fload_1          },
    { "fload_2"         , 0x24, 1,  op_fload_2          },
    { "fload_3"         , 0x25, 1,  op_fload_3          },
    { "fstore"          , 0x38, 2,  op_fstore           },
    { "fstore_0"        , 0x43, 1,  op_fstore_0         },
    { "fstore_1"        , 0x44, 1,  op_fstore_1         },
    { "fstore_2"        , 0x45, 1,  op_fstore_2         },
    { "fstore_3"        , 0x46, 1,  op_fstore_3         },
    { "fadd"            , 0x62, 1,  op_fadd             },
    { "fsub"            , 0x66, 1,  op_fsub             },
    { "fmul"            , 0x6A, 1,  op_fmul             },
    { "fdiv"            , 0x6E, 1,  op_fdiv             },
    { "fneg"            , 0x76, 1,  op_fneg             },
    { "fcmpl"           , 0x95, 1,  op_fcmpl            },
    { "fcmpg"           , 0x96, 1,  op_fcmpg            },
    { "i2f"             , 0x86, 1,  op_i2f              },
    { "i2b"             , 0x91, 1,  op_i2b              },
    { "i2c"             , 0x92, 1,  op_i2c              },
    { "i2s"             , 0x93, 1,  op_i2s              },
    { "f2i"             , 0x8B, 1,  op_f2i              },
    { "freturn"         , 0xAE, 1,  op_freturn          }
};
static size_t byteCode_size = sizeof(byteCodes) / sizeof(byteCode);
static byteCode byteCodes_backup[sizeof(byteCodes) / sizeof(byteCode)];
static int byteCodes_backup_ready = 0;

static opCodeFunc findOpCodeFunc(unsigned char op)
{
    switch (op)
    {
    case 0x2A: return op_aload_0;
    case 0x10: return op_bipush;
    case 0x59: return op_dup;
    case 0xB2: return op_getstatic;
    case 0x60: return op_iadd;
    case 0x02: return op_iconst_m1;
    case 0x03: return op_iconst_0;
    case 0x04: return op_iconst_1;
    case 0x05: return op_iconst_2;
    case 0x06: return op_iconst_3;
    case 0x07: return op_iconst_4;
    case 0x08: return op_iconst_5;
    case 0x0F: return op_dconst_1;
    case 0x6C: return op_idiv;
    case 0x68: return op_imul;
    case 0x63: return op_dadd;
    case 0x6B: return op_dmul;
    case 0x8e: return op_d2i;
    case 0xB7: return op_invokespecial;
    case 0xB6: return op_invokevirtual;
    case 0xB8: return op_invokestatic;
    case 0x15: return op_iload;
    case 0x1B: return op_iload_1;
    case 0x1C: return op_iload_2;
    case 0x1D: return op_iload_3;
    case 0x36: return op_istore;
    case 0x3C: return op_istore_1;
    case 0x3D: return op_istore_2;
    case 0x3E: return op_istore_3;
    case 0x64: return op_isub;
    case 0x12: return op_ldc;
    case 0x13: return op_ldc_w;
    case 0x14: return op_ldc2_w;
    case 0xBB: return op_new;
    case 0x70: return op_irem;
    case 0x78: return op_ishl;
    case 0x7A: return op_ishr;
    case 0x7C: return op_iushr;
    case 0x7E: return op_iand;
    case 0x80: return op_ior;
    case 0x82: return op_ixor;
    case 0x11: return op_sipush;
    case 0xB1: return op_return;
    case 0x00: return op_nop;
    case 0x01: return op_aconst_null;
    case 0x1A: return op_iload_0;
    case 0x3B: return op_istore_0;
    case 0x19: return op_aload;
    case 0x2B: return op_aload_1;
    case 0x2C: return op_aload_2;
    case 0x2D: return op_aload_3;
    case 0x3A: return op_astore;
    case 0x4B: return op_astore_0;
    case 0x4C: return op_astore_1;
    case 0x4D: return op_astore_2;
    case 0x4E: return op_astore_3;
    case 0x57: return op_pop;
    case 0x58: return op_pop2;
    case 0x5F: return op_swap;
    case 0xAC: return op_ireturn;
    case 0xB0: return op_areturn;
    case 0xA7: return op_goto;
    case 0x99: return op_ifeq;
    case 0x9A: return op_ifne;
    case 0x9B: return op_iflt;
    case 0x9C: return op_ifge;
    case 0x9D: return op_ifgt;
    case 0x9E: return op_ifle;
    case 0x9F: return op_if_icmpeq;
    case 0xA0: return op_if_icmpne;
    case 0xA1: return op_if_icmplt;
    case 0xA2: return op_if_icmpge;
    case 0xA3: return op_if_icmpgt;
    case 0xA4: return op_if_icmple;
    case 0xA5: return op_if_acmpeq;
    case 0xA6: return op_if_acmpne;
    case 0xC6: return op_ifnull;
    case 0xC7: return op_ifnonnull;
    case 0x84: return op_iinc;
    case 0xAA: return op_tableswitch;
    case 0xAB: return op_lookupswitch;
    case 0xBC: return op_newarray;
    case 0x2E: return op_iaload;
    case 0x4F: return op_iastore;
    case 0x33: return op_iaload;
    case 0x34: return op_iaload;
    case 0x35: return op_iaload;
    case 0x54: return op_iastore;
    case 0x55: return op_iastore;
    case 0x56: return op_iastore;
    case 0x32: return op_aaload;
    case 0x53: return op_aastore;
    case 0xBD: return op_anewarray;
    case 0xBE: return op_arraylength;
    case 0xB4: return op_getfield;
    case 0xB5: return op_putfield;
    case 0xB3: return op_putstatic;
    case 0xC0: return op_checkcast;
    case 0xC1: return op_instanceof;
    case 0x0B: return op_fconst_0;
    case 0x0C: return op_fconst_1;
    case 0x0D: return op_fconst_2;
    case 0x17: return op_fload;
    case 0x22: return op_fload_0;
    case 0x23: return op_fload_1;
    case 0x24: return op_fload_2;
    case 0x25: return op_fload_3;
    case 0x38: return op_fstore;
    case 0x43: return op_fstore_0;
    case 0x44: return op_fstore_1;
    case 0x45: return op_fstore_2;
    case 0x46: return op_fstore_3;
    case 0x62: return op_fadd;
    case 0x66: return op_fsub;
    case 0x6A: return op_fmul;
    case 0x6E: return op_fdiv;
    case 0x76: return op_fneg;
    case 0x95: return op_fcmpl;
    case 0x96: return op_fcmpg;
    case 0x86: return op_i2f;
    case 0x91: return op_i2b;
    case 0x92: return op_i2c;
    case 0x93: return op_i2s;
    case 0x8B: return op_f2i;
    case 0xAE: return op_freturn;
    default: return 0;
    }
}

#if SIMPLE_JVM_DEBUG
static int findOpCodeOffset(unsigned char op)
{
    int i;
    for (i = 0; i < byteCode_size ; i++)
        if (op == byteCodes[i].opCode)
            return byteCodes[i].offset;
    return 0;
}
#endif

static int convertToCodeAttribute(CodeAttribute *ca, AttributeInfo *attr)
{
    int info_p = 0;
    unsigned char tmp[4];
    ca->attribute_name_index = attr->attribute_name_index;
    ca->attribute_length = attr->attribute_length;
    tmp[0] = attr->info[info_p++];
    tmp[1] = attr->info[info_p++];
    ca->max_stack = tmp[0] << 8 | tmp[1];
    tmp[0] = attr->info[info_p++];
    tmp[1] = attr->info[info_p++];
    ca->max_locals = tmp[0] << 8 | tmp[1];
    tmp[0] = attr->info[info_p++];
    tmp[1] = attr->info[info_p++];
    tmp[2] = attr->info[info_p++];
    tmp[3] = attr->info[info_p++];
    ca->code_length = tmp[0] << 24 | tmp[1] << 16 | tmp[2] << 8 | tmp[3];
    if (ca->code_length < 0 || ca->code_length > 65536)
    {
        fprintf(stderr, "[jvm] bad code_length %d, aborting\n", ca->code_length);
        return -1;
    }
    ca->code = (unsigned char *) malloc(sizeof(unsigned char) * ca->code_length);
    if (ca->code == NULL)
    {
        fprintf(stderr, "[jvm] malloc failed, aborting\n");
        return -1;
    }
    memcpy(ca->code, attr->info + info_p, ca->code_length);
    return 0;
}

int executeMethod(MethodInfo *startup, StackFrame *stack, SimpleConstantPool *p)
{
    int i = 0;
    int j = 0;
    int found = 0;
    char name[255];
    char mname[255];
    unsigned char *base_prev;
    CodeAttribute ca;
    memset(&ca, 0 , sizeof(CodeAttribute));
    for (j = 0 ; j < startup->attributes_count ; j++) {
        /* Se comprueba el nombre ANTES de convertir: un atributo que no es
         * "Code" (LineNumberTable, ConstantValue...) tiene otros bytes y
         * convertirlo reservaba una copia de tamano al azar. */
        getUTF8String(p, startup->attributes[j].attribute_name_index, 255, name);
        if (memcmp(name, "Code", 4) != 0) continue;
        if (convertToCodeAttribute(&ca, &startup->attributes[j]) != 0) continue;
        found = 1;
#if SIMPLE_JVM_DEBUG
        printf("----------------------------------------\n");
        printf("code dump\n");
        printCodeAttribute(&ca, p);
        printf("----------------------------------------\n");
#endif
        unsigned char *pc = ca.code;
        base_prev = jvm_code_base;
        jvm_code_base = ca.code;   /* base para tableswitch/lookupswitch */
        if (run == 0)
            exit(1);
        do {
            opCodeFunc func = findOpCodeFunc(pc[0]);
            if (func == 0) {
                getUTF8String(p, startup->name_index, 255, mname);
                fprintf(stderr,
                        "[jvm] opcode 0x%02X no implementado en %s (offset %d)\n",
                        pc[0], mname, (int) (pc - ca.code));
                exit(3);
            }
            /* aqui el estado de la VM esta completo y no hay ninguna native a
             * medias: es el punto seguro donde gc.c puede barrer. */
            jvm_gc_safe_point();
            i = func(&pc , stack, p);
        } while (i >= 0);
        /* La copia del bytecode es de ESTE cuadro: el invoke de otro metodo
         * entra aqui con su propio `ca` y tiene que devolver la suya.  Sin
         * este free cada llamada a un metodo java se dejaba un array de
         * bytecode en PSRAM para siempre (la fuga que media `monitor`). */
        jvm_code_base = base_prev;
        free(ca.code);
        ca.code = 0;
    }
    if (!found) {
        getUTF8String(p, startup->name_index, 255, mname);
        fprintf(stderr, "[jvm] metodo sin cuerpo (nativo/abstracto?): %s\n",
                mname);
        exit(3);
    }
    return 0;
}

#if SIMPLE_JVM_DEBUG
static char *findOpCode(unsigned char op)
{
    int i;
    for (i = 0; i < byteCode_size ; i++)
        if (op == byteCodes[i].opCode)
            return byteCodes[i].name;
    return 0;
}

static void printCodeAttribute(CodeAttribute *ca, SimpleConstantPool *p)
{
    int i = 0;
    int tmp = 0;
    char name[255];
    getUTF8String(p, ca->attribute_name_index, 255, name);
    printf("attribute name : %s\n", name);
    printf("attribute length: %d\n", ca->attribute_length);

    printf("max_stack: %d\n", ca->max_stack);
    printf("max_locals: %d\n", ca->max_locals);
    printf("code_length: %d\n", ca->code_length);
    unsigned char *pc = ca->code;
    i = 0;
    do {
        char *opName = findOpCode(pc[0]);
        if (opName == 0) {
            printf("Unknow OpCode %02X\n", pc[0]);
            exit(1);
        }
        printf("%s \n", opName);
        tmp = findOpCodeOffset(pc[0]);
        pc += tmp;
        i += tmp;
    } while (i < ca->code_length);
}
#endif
