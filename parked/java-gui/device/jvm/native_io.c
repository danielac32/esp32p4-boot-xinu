/*
 * native_io.c — natives de esp32p4.IO en el ESP32-P4 (consola + texto).
 *
 * print/println/readChar/printBuf usan la consola de Xinu (printf/getchar
 * en contexto de tarea).  En el host lo implementa jvm/esp32p4_io.c.
 *
 * Convención de pila: el ÚLTIMO argumento está en la cima.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <kernel.h>
#include <stdio.h>
#include <string.h>

#include "simple_jvm.h"
#include "java_lib.h"

int esp32p4_io_print(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char buf[1024];

    (void) type;
    jvm_pop_string(stack, p, buf, (int) sizeof(buf));
    printf("%s", buf);
    return 0;
}

int esp32p4_io_println(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char buf[1024];

    (void) type;
    jvm_pop_string(stack, p, buf, (int) sizeof(buf));
    printf("%s\n", buf);
    return 0;
}

/* readChar(): bloquea hasta leer un carácter; -1 al final de entrada. */
int esp32p4_io_readChar(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int c;

    (void) p;
    (void) type;
    c = getchar();
    pushInt(stack, (c >= 0) ? c : -1);
    return 0;
}

int esp32p4_io_strLen(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char buf[1024];

    (void) type;
    jvm_pop_text(stack, p, buf, (int) sizeof(buf));
    pushInt(stack, (int) strlen(buf));
    return 0;
}

int esp32p4_io_strAt(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char buf[1024];
    int idx;
    int len;
    int code;

    (void) type;
    idx = popInt(stack);
    jvm_pop_text(stack, p, buf, (int) sizeof(buf));
    len = (int) strlen(buf);
    code = (idx >= 0 && idx < len) ? (unsigned char) buf[idx] : -1;
    pushInt(stack, code);
    return 0;
}

int esp32p4_io_printBuf(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int len = popInt(stack);
    int bufref = popInt(stack);
    int i;

    (void) p;
    (void) type;
    for (i = 0; i < len; i++)
    {
        putchar(heap_array_load(bufref, i));
    }
    return 0;
}
