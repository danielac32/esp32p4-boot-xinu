/*
 * Simple Java Virtual Machine Implementation
 *
 * Copyright (C) 2013 Chun-Yu Wang <wicanr2@gmail.com>
 */

#ifndef SIMPLE_JVM_H
#define SIMPLE_JVM_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jvm_io.h"

typedef unsigned short u2;
typedef unsigned char byte;

/* Java Class File */
typedef struct _ClassFileFormat { 
    byte magic_number[4];
    u2   minor_version;
    u2   major_version;
    u2   constant_pool_count;
    /* constant pool */
    u2   access_flags;
    u2   this_class;
    u2   super_class;

    u2   interface_count;
    /* interface pool */
    u2   fields_count;
    /* fields pool */
    u2   methods_count;
    /* method pool */
    u2   attributes_count;
    /* attributes pool */
} ClassFileFormat;

/*
 *  TAG 
 *  1 UTF-8 String
 *  3 Integer
 *  4 Float
 *  5 Long
 *  6 Double
 *  7 Class reference
 *  8 String reference
 *  9 Field reference
 *  10 Method reference
 *  11 Interface method reference
 *  12 Name and type descriptor
 * */
#define CONSTANT_UTF8           1
#define CONSTANT_INTEGER        3
#define CONSTANT_FLOAT          4
#define CONSTANT_LONG           5
#define CONSTANT_DOUBLE         6

#define CONSTANT_CLASS          7
#define CONSTANT_STRING_REF     8
#define CONSTANT_FIELD_REF      9
#define CONSTANT_METHOD_REF     10
#define CONSTANT_INTERFACE_REF  11
#define CONSTANT_NAME_AND_TYPE  12

typedef struct _ConstantUTF8 {
    int index;
    unsigned char tag;
    int additional_byte_size;
    u2 string_size;
    unsigned char *ptr;
} ConstantUTF8;

typedef struct _ConstantInteger {
    int index;
    unsigned char tag;
    int additional_byte_size;
    int value;
} ConstantInteger;

typedef struct _ConstantFloat {
    int index;
    unsigned char tag;
    int additional_byte_size;
    float value;
} ConstantFloat;

typedef struct _ConstantLong {
    int index;
    unsigned char tag;
    int additional_byte_size;
    long long value;
} ConstantLong;

typedef struct _ConstantDouble {
    int index;
    unsigned char tag;
    int additional_byte_size;
    double value;
} ConstantDouble;

typedef struct _ConstantClassRef {
    int index;
    unsigned char tag;
    int additional_byte_size;
    u2 stringIndex;
} ConstantClassRef;

typedef struct _ConstantStringRef {
    int index;
    unsigned char tag;
    int additional_byte_size;
    u2 stringIndex;
} ConstantStringRef;

typedef struct _ConstantFieldRef {
    int index;
    unsigned char tag;
    int additional_byte_size;
    u2 classIndex;
    u2 nameAndTypeIndex;
} ConstantFieldRef;

typedef struct _ConstantMethodRef {
    int index;
    unsigned char tag;
    int additional_byte_size;
    u2 classIndex;
    u2 nameAndTypeIndex;
} ConstantMethodRef;

typedef struct _ConstantInterfaceRef {
    int index;
    unsigned char tag;
    int additional_byte_size;
    u2 classIndex;
    u2 nameAndTypeIndex;
} ConstantInterfaceRef;

typedef struct _ConstantNameAndType {
    int index;
    unsigned char tag;
    int additional_byte_size;
    u2 nameIndex;
    u2 typeIndex;
} ConstantNameAndType;

typedef struct _SimpleConstantPool {
    /* UTF-8 String */
    ConstantUTF8 utf8CP[200];
    int utf8_used; 

    /* Integer */
    ConstantInteger integerCP[100];
    int integer_used;

    /* Float */
    ConstantFloat floatCP[100];
    int float_used; 

    /* Long */
    ConstantLong longCP[100];
    int long_used;
    
    /* Double */
    ConstantDouble doubleCP[100];
    int double_used;

    /* Class Reference */
    ConstantClassRef clasz[100];
    int clasz_used;

    /* String Reference */
    ConstantStringRef stringRef[100];
    int stringRef_used;  

    /* Field Reference */
    ConstantFieldRef field[100];
    int field_used;

    /* Method Reference */
    ConstantMethodRef method[100];
    int method_used;

    /* Interface Reference */
    ConstantInterfaceRef interface[100];
    int interface_used;

    /* Name And Type Reference */
    ConstantNameAndType  name_and_type[100];
    int name_and_type_used;
} SimpleConstantPool;

typedef struct _SimpleInterfacePool {
    /* Class Reference */
    ConstantClassRef clasz[100];
    int clasz_used;
} SimpleInterfacePool;

/* Attribute Info */
typedef struct _AttributeInfo {
    u2 attribute_name_index;
    int attribute_length;
    unsigned char *info;
} AttributeInfo;

/* Code Attributes */
typedef struct _CodeAttribute {
        u2 attribute_name_index;
        int attribute_length;
        u2 max_stack;
        u2 max_locals;
        int code_length;
        unsigned char *code; // [code_length];
#if 0
        u2 exception_table_length;
        Exception_table* exception_table; //[exception_table_length];
        u2 attributes_count;
        attribute_info* attributes; //[attributes_count];
#endif
} CodeAttribute;

/* Field Info */
typedef struct _FieldInfo {
    u2   access_flags;
    u2   name_index;
    u2   descriptor_index;
    u2   attributes_count;
    AttributeInfo *attributes;
} FieldInfo;

/* Simple Field Pool */
typedef struct _SimpleFieldPool {
    FieldInfo field[100];
    int field_used;
} SimpleFieldPool;

/* Method Info */
typedef struct _MethodInfo {
    u2 access_flags; 
    u2 name_index; 
    u2 descriptor_index; 
    u2 attributes_count; 
    AttributeInfo *attributes;
} MethodInfo;

/* Simple Method Pool */
typedef struct _SimpleMethodPool {
    MethodInfo method[100];
    int method_used;
} SimpleMethodPool;

/* constant pool parser */
int parseConstantPool(JvmFile *fp, int count);
void printConstantPool(SimpleConstantPool *p);
ConstantUTF8 *findUTF8(SimpleConstantPool *p, int index);
ConstantStringRef *findStringRef(SimpleConstantPool *p, int index);
int getUTF8String(SimpleConstantPool *p, int index, int size, char *out);
ConstantClassRef *findClassRef(SimpleConstantPool *p , int index);
ConstantMethodRef *findMethodRef(SimpleConstantPool *p , int index);
ConstantNameAndType *findNameAndType(SimpleConstantPool *p , int index);

int get_integer_from_constant_pool(SimpleConstantPool *p, int index);
long long get_long_from_constant_pool(SimpleConstantPool *p, int index);
float get_float_from_constant_pool(SimpleConstantPool *p, int index);
double get_double_from_constant_pool(SimpleConstantPool *p, int index);

/* p4-sdk: tag CP de un indice si esta en las tablas que usa ldc
 * (CONSTANT_STRING_REF / INTEGER / FLOAT); 0 si no. Para que ldc sepa
 * si empujar el literal como REF, INT o FLOAT. */
int cp_probe_type(SimpleConstantPool *p, int index);

/* Interface Pool Parser */
int parseInterfacePool(JvmFile *fp, int count);
void printInterfacePool( SimpleConstantPool *p, SimpleInterfacePool *ip);

/* Field Pool Parser */
int parseFieldPool(JvmFile *fp, int count);
void printFieldPool( SimpleConstantPool *p, SimpleFieldPool *fp);

/* Method Pool Parser */
int parseMethodPool(JvmFile *fp, int count);
void printMethodPool( SimpleConstantPool *p, SimpleMethodPool *fp);
MethodInfo *findMethodInPool(SimpleConstantPool *p,
                             SimpleMethodPool *mp,
                             char *method_name, int size);
void printMethodAttributes(SimpleConstantPool *p,
                           MethodInfo *method);

/* Java Class File Parser */
int parseJavaClassFile(char *file, ClassFileFormat *cff);
void printClassFileFormat(ClassFileFormat *cff);

/* Free Pools */
void free_pools(void);

/* Stack Frame */
#define STACK_ENTRY_NONE        0
#define STACK_ENTRY_INT         1
#define STACK_ENTRY_REF         2
#define STACK_ENTRY_LONG        3
#define STACK_ENTRY_DOUBLE      4
#define STACK_ENTRY_FLOAT       5
#define STACK_ENTRY_OBJ         6   /* ref de heap (array/objeto, p4-sdk) */

typedef struct _StackEntry {
    unsigned char entry[8];
    int type;
} StackEntry;

typedef struct _StackFrame {
    int max_size;
    int size;
    StackEntry *store;
} StackFrame;
void stackInit(StackFrame *stack, int entry_size);

void pushInt    (StackFrame *stack, int value);
void pushLong   (StackFrame *stack, long long value);
void pushDouble (StackFrame *stack, double value);
void pushFloat  (StackFrame *stack, float value);
void pushRef    (StackFrame *stack, int value);
void pushObj    (StackFrame *stack, int value);   /* p4-sdk: ref de heap */

int         popInt      (StackFrame *stack);
long long   popLong     (StackFrame *stack);
double      popDouble   (StackFrame *stack);
float       popFloat    (StackFrame *stack);
StackEntry *popEntry    (StackFrame *stack);

int     EntryToInt(StackEntry *entry );
double  EntryToDouble(StackEntry *entry);
float   EntryToFloat(StackEntry *entry);   /* p4-sdk: para floats */
int     is_ref_entry(StackFrame *stack);

/* local variable */
#define JVM_MAX_LOCALS 16
typedef struct _LocalVariables {
    int integer[JVM_MAX_LOCALS];
    int type[JVM_MAX_LOCALS];   /* STACK_ENTRY_* guardado por local */
} LocalVariables;

/* byte Codes */
typedef int (*opCodeFunc)(unsigned char **opCode, StackFrame *stack, SimpleConstantPool *p);

typedef struct _byteCode {
    char *name;
    unsigned char opCode;
    int offset;
    opCodeFunc func; 
} byteCode;
int executeMethod(MethodInfo *startup, StackFrame *stack, SimpleConstantPool *p);

/* ---- p4-sdk: frames, heap y estado de la VM ---- */
#define JVM_MAX_CALLS    32      /* profundidad maxima de llamadas Java
                                    (GUI apps usan <10 niveles; 32 da
                                    margen y reduce el stack a ~64 KB) */
#define JVM_FRAME_STACK  64      /* entradas de pila por frame */
#define JVM_MAX_STATICS  64      /* campos estaticos */
#define JVM_HEAP_SLOTS   2048    /* slots del heap (arrays/objetos/Strings) */

/* Un slot del heap: id entero = indice en la tabla (0 = null). */
typedef struct _JvmHeapSlot {
    int kind;
    int len;        /* iarray/object: nº elems; string: longitud; sb: usado */
    int cap;        /* solo sbuilder: capacidad de text */
    int *data;      /* iarray/object */
    char *text;     /* string/sbuilder (terminado en NUL) */
    unsigned gen;   /* nº de creacion (gc.c): lo creado despues del ultimo
                       punto seguro sigue en manos de la expresion actual */
} JvmHeapSlot;

/* Tipos de slot (heap.c los usa, gc.c los necesita para barrer). */
#define HEAP_KIND_FREE      0
#define HEAP_KIND_IARRAY    1
#define HEAP_KIND_OBJECT    2
#define HEAP_KIND_STRING    3   /* String inmutable (resultado de concatenar) */
#define HEAP_KIND_SBUILDER  4   /* StringBuilder con buffer propio */
#define HEAP_KIND_REFARRAY  5   /* array de referencias (String[]/Object[]):    */
                                /* data = 2 ints/elem (valor, STACK_ENTRY_*)    */
#define HEAP_KIND_BARRAY    6   /* array de bytes (byte[]): text = len bytes    */

/*
 * Todo el estado mutable de una ejecucion de la VM.  Antes eran globales
 * (una sola VM a la vez); ahora vive aqui, uno por instancia, para que
 * varias tareas puedan ejecutar Java a la vez (ver jvm_io.h:
 * jvm_current_get()).
 */
typedef struct _JvmState {
    SimpleConstantPool cp;
    SimpleInterfacePool ip;
    SimpleFieldPool fp;
    SimpleMethodPool mp;
    StackFrame stack;
    LocalVariables locals;
    JvmHeapSlot heap_slots[JVM_HEAP_SLOTS];
    char cls_name[256];             /* "pkg/Clase" cargada                 */
    int statics_arr[JVM_MAX_STATICS];
    int depth;                      /* profundidad de llamadas Java        */
    unsigned char *code_base;       /* base de .code (alineacion switches) */

    /* ---- GC mark & sweep del heap de objetos (gc.c) ----
     * gc_frames/gc_saved apuntan a los cuadros que invoke_resolved() tiene
     * como locales C: sin ese registro el barrido no veria las pilas ni los
     * locals de los cuadros llamantes (viven en la pila C, no aqui). */
    int heap_used;                  /* ranuras ocupadas (agua del heap)      */
    unsigned gc_alloc;              /* nº de ranuras concedidas desde 1      */
    unsigned gc_gen;                /* gc_alloc en el ultimo punto seguro    */
    unsigned gc_last_run;           /* gc_alloc cuando barriera por ultima   */
    unsigned char gc_marks[(JVM_HEAP_SLOTS + 7) / 8];
    int gc_queue[JVM_HEAP_SLOTS];
    StackFrame *gc_frames[JVM_MAX_CALLS + 2];
    LocalVariables *gc_saved[JVM_MAX_CALLS + 2];
} Jvm;

/*
 * Acceso al estado de la tarea actual.  Los nombres coinciden con los
 * antiguos globales, de modo que el nucleo del interprete (bytecodes.c,
 * los parsers, heap.c, ...) no cambia: cada acceso se resuelve a traves
 * del estado de la tarea que ejecuta.  Los campos del struct NO se llaman
 * como estos macros (locals/cls_name/...) para poder escribir `st->locals`
 * en jvm_run.c sin que el preprocesador interfiera.
 */
#define jvm                 (jvm_current_get())
#define simpleConstantPool  (jvm->cp)
#define simpleInterfacePool (jvm->ip)
#define simpleFieldPool     (jvm->fp)
#define simpleMethodPool    (jvm->mp)
#define localVariables      (jvm->locals)
#define jvm_class_name      (jvm->cls_name)
#define jvm_statics         (jvm->statics_arr)
#define jvm_call_depth      (jvm->depth)
#define jvm_code_base       (jvm->code_base)
#define heap                (jvm->heap_slots)

/* Ciclo de vida de una instancia + arranque de una clase (jvm_run.c).
 * jvm_run_args construye el String[] que recibe main(); argv puede ser
 * NULL si argc == 0 (siempre se pasa un array, nunca null). */
Jvm *jvm_state_new(void);
void jvm_state_free(Jvm *st);
int jvm_run_args(Jvm *st, char *path, int argc, char **argv);
int jvm_run_file(Jvm *st, char *path);   /* = jvm_run_args(st, path, 0, 0) */

/* Pegamento con el kernel (device/jvm/jvmglue.c): ejecuta una clase en la
 * tarea actual.  Solo existe en el target. */
int jvm_run_path(const char *path, int argc, char **argv);

/* heap.c: arrays de int y objetos con campos (ids = referencias) */
int  heap_new_int_array(int len);
int  heap_new_byte_array(int len);       /* byte[] real: 1 byte/elem */
int  heap_array_load(int ref, int idx);
void heap_array_store(int ref, int idx, int value);
int  heap_array_length(int ref);

/* heap.c: arrays de referencias (String[]/Object[]). Cada elemento
 * guarda su tipo STACK_ENTRY_* para distinguir REF (pool) y OBJ (heap). */
int  heap_new_ref_array(int len);
int  heap_ref_array_load(int ref, int idx, int *type);
void heap_ref_array_store(int ref, int idx, int value, int type);
int  heap_new_object(int nfields);
int  heap_object_get(int ref, int idx);
void heap_object_put(int ref, int idx, int value);

/* heap.c: strings reales (resultado de concatenar) y StringBuilders
 * con buffer propio. La concatenacion deja un String del heap en la
 * pila: se puede guardar en un local, pasar a una native o imprimir. */
int  heap_is_string(int ref);            /* 1 si el slot es un String */
int  heap_new_string(const char *s);     /* copia inmutable */
void heap_init_string(int ref, const char *s); /* convierte un slot en String */
char *heap_string_text(int ref);         /* contenido (NUL-terminated) */
int  heap_new_sbuilder(void);            /* StringBuilder vacio */
void heap_sbuilder_append(int ref, const char *s);
char *heap_sbuilder_text(int ref);
int  heap_sbuilder_len(int ref);

/* gc.c: mark & sweep del heap de slots.  jvm_gc() devuelve ranuras liberadas;
 * `conservative` anade raices desde la pila C (lo usa heap_new_slot cuando ya
 * no queda ninguna ranura libre y una expresion esta a medias).  Las raices
 * normales son los tipos STACK_ENTRY_OBJ de locals/pilas/estaticos-campos. */
int  jvm_gc(int conservative);
/* Punto seguro del interprete (entre bytecodes): barre si queda poca agua. */
void jvm_gc_safe_point(void);

/* bytecodes.c: resolucion unificada de texto. jvm_entry_text acepta
 * REF (literal del pool), OBJ (String del heap), INT y FLOAT, y escribe
 * su representacion en out. jvm_pop_text saca una entrada de la pila y
 * la resuelve. */
int  jvm_entry_text(StackEntry *e, SimpleConstantPool *p,
                    char *out, int max);
int  jvm_pop_text(StackFrame *stack, SimpleConstantPool *p,
                  char *out, int max);

/* bytecodes.c: extrae un String de la pila para las natives de consola.
 * Saca una entrada y la resuelve a texto (ver jvm_entry_text); es un
 * alias de jvm_pop_text. Devuelve la longitud escrita. */
int  jvm_pop_string(StackFrame *stack, SimpleConstantPool *p,
                    char *out, int max);

#endif
