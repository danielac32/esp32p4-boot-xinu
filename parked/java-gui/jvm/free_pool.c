/*
 * Simple Java Virtual Machine Implementation
 *
 * Copyright (C) 2013 Chun-Yu Wang <wicanr2@gmail.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "simple_jvm.h"
#include "jvm_map.h"


/* Suelta lo que copiaron los parsers del .class en el heap de la instancia.
 * Cada ejecucion vuelve a memsetear estos pools al cargar la clase (jvm_run.c),
 * pero el heap de PSRAM no se resetea: lo que no se libera aqui se pierde para
 * siempre, y con una hebra residente (el launcher) que invoca metodos todo el
 * dia la fuga se come los 32 MB. */
void free_pools(void)
{
    int i, j;
    MethodInfo *method = 0;
    FieldInfo *field = 0;
    AttributeInfo *attr = 0;

    for (i = 0; i < simpleMethodPool.method_used; i++) {
        method = &simpleMethodPool.method[i];
        for (j = 0; j < method->attributes_count ; j++) {
            attr = &method->attributes[j];
            free(attr->info);
            memset(attr, 0, sizeof(AttributeInfo));
        }
        free(method->attributes);
        memset(method, 0, sizeof(MethodInfo));
    }

    /* Campos: mismos atributos (ConstantValue), mismos punteros que devolver. */
    for (i = 0; i < simpleFieldPool.field_used; i++) {
        field = &simpleFieldPool.field[i];
        for (j = 0; j < field->attributes_count ; j++) {
            attr = &field->attributes[j];
            free(attr->info);
            memset(attr, 0, sizeof(AttributeInfo));
        }
        free(field->attributes);
        memset(field, 0, sizeof(FieldInfo));
    }

    /* Las cadenas del constant pool (nombres de metodos, descriptores, textos
     * del "ldc"): cada una es un malloc aparte. */
    for (i = 0; i < simpleConstantPool.utf8_used; i++) {
        free(simpleConstantPool.utf8CP[i].ptr);
        memset(&simpleConstantPool.utf8CP[i], 0, sizeof(ConstantUTF8));
    }
}
