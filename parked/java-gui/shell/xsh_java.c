/**
 * @file xsh_java.c
 *
 * Orden `java': ejecuta una clase .class en la JVM residente (portada del
 * arbol xinu-s3-amp).  Las clases viven en /java de la microSD.
 *
 * Porque la clase se busca por nombre en el FAT, el comando no puede correr
 * en la hebra del shell: cada comando no-built-in se lanza con SHELL_CMDSTK
 * (8 KB, include/shell.h:16) y el interprete de jvm/bytecodes.c recursa por
 * cada invoke.  Se crea una hebra propia con JVM_STK (include/thread.h) y se
 * espera a que termine.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <thread.h>
#include <shell.h>

/* device/jvm/jvmglue.c */
extern thread jvmTask(char *path, int argc, char **argv);

#define JAVA_DIR "/java"

static void java_usage(const char *cmd)
{
    printf("Uso: %s [--help] <Clase> [args...]\n\n", cmd);
    printf("Corre /java/<Clase>.class en la JVM.\n");
    printf("Los argumentos extra llegan a main(String[]).\n\n");
    printf("Ejemplos:\n");
    printf("\t%s Hello\n", cmd);
    printf("\t%s /java/Echo.class hola mundo\n", cmd);
}

/**
 * @ingroup shell
 *
 * Shell command (java).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_java(int nargs, char *args[])
{
    char path[128];
    const char *name;
    tid_typ child;
    size_t len;
    int jargc;
    char **jargv;

    if (nargs == 2 &&
        (strcmp(args[1], "--help") == 0 || strcmp(args[1], "-h") == 0))
    {
        java_usage(args[0]);
        return OK;
    }
    if (nargs < 2)
    {
        java_usage(args[0]);
        return 1;
    }

    name = args[1];

    /* Acepta "Hello", "Hello.class" o una ruta absoluta ya hecha. */
    if (name[0] == '/')
    {
        strlcpy(path, name, sizeof(path));
    }
    else
    {
        len = strlen(name);
        if (len > 6 && strcmp(name + len - 6, ".class") == 0)
        {
            snprintf(path, sizeof(path), "%s/%s", JAVA_DIR, name);
        }
        else
        {
            snprintf(path, sizeof(path), "%s/%s.class", JAVA_DIR, name);
        }
    }

    /* Argumentos para main(String[]): lo que venga detras del nombre. */
    jargc = nargs - 2;
    jargv = (jargc > 0) ? &args[2] : 0;

    /* La VM necesita una pila holgada (JVM_STK), no los 8 KB del comando. */
    child = create(jvmTask, JVM_STK, INITPRIO, "JVM", 3, path, jargc, jargv);
    if (SYSERR == child)
    {
        printf("java: no se pudo crear la hebra de la VM\n");
        return SYSERR;
    }

    /* La hebra JVM tiene que escribir donde escribe quien la llamó: la
     * UART, una sesion de telnet o el webterm.  create() pone CONSOLE por
     * defecto, asi que se copian los descriptores del comando actual. */
    thrtab[child].fdesc[0] = stdin;
    thrtab[child].fdesc[1] = stdout;
    thrtab[child].fdesc[2] = stderr;

    while (recvclr() != NOMSG)
        ;
    ready(child, RESCHED_YES);

    /* Espera al final de jvmTask: kill() envia el tid de la muerta al
     * padre, y aqui el padre es esta hebra de comando. */
    while (receive() != child)
        ;

    return OK;
}
