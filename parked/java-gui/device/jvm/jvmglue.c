/*
 * jvmglue.c — pegamento de la VM con el kernel (device/jvm).
 *
 * jvm_run_path() ejecuta una clase .class en la tarea actual: asigna el
 * estado en PSRAM, corre la VM (con sus argumentos) y lo libera.  jvmTask()
 * es la entrada de tarea (create(jvmTask, ..., "JVM", 3, path, argc, argv));
 * así cada ejecución de Java corre en su propia tarea y varias pueden
 * coexistir (el estado es por instancia, ver jvm_io_target.c).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <kernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread.h>
#include <stack.h>

#include "simple_jvm.h"
#include "jvm_io.h"
#include <fat.h>
#include <lvgl.h>

int jvm_run_path(const char *path, int argc, char **argv)
{
    Jvm *st;
    char buf[128];
    int rc;

    if (path == 0)
    {
        return SYSERR;
    }
    strlcpy(buf, path, sizeof(buf));
    /* En el P4 el FAT lo monta el device `fat' durante el arranque (FAT0
     * está en el devtab); no existe fat_mount() como en el S3, sólo se
     * comprueba que la monta ocurrió. */
    if (!fatMounted()) { printf("[jvm] FAT no montado\r\n"); return SYSERR; }

    st = jvm_state_new();
    if (st == 0)
    {
        printf("[jvm] sin memoria en PSRAM para el estado\r\n");
        return SYSERR;
    }

    rc = jvm_run_args(st, buf, argc, argv);
    /* Pico de pila observado por ESTA hebra, medido antes de liberar el
     * estado.  Se imprime aqui y no con `ps' porque `ps' solo ve la hebra
     * mientras vive, y el servidor telnet no acepta una segunda conexion
     * durante la ejecucion (queda sin backlog, tarea #67): sin esta linea no
     * hay forma de ajustar JVM_STK (include/thread.h) con datos de la placa.
     * Importa porque la pila sale del heap de L2MEM --223 KB libres--, no de
     * PSRAM donde vive el estado de la VM. */
    printf("[jvm] %s: pico de pila %u de %u B\r\n", buf,
           stack_peak(gettid()), stack_total(gettid()));
    jvm_state_free(st);
    jvm_current_set(0);

    if (rc == -2)
    {
        printf("[jvm] no se pudo abrir %s\r\n", buf);
    }
    else if (rc == -3)
    {
        printf("[jvm] %s: no tiene main()\r\n", buf);
    }
    return (rc == 0) ? OK : SYSERR;
}

/* Entrada de tarea: create(jvmTask, ..., "JVM", 3, path, argc, argv). */
thread jvmTask(char *path, int argc, char **argv)
{
    int rc = jvm_run_path(path, argc, argv);

    /* La UI de esta clase muere con la clase: sin esto el arbol de LVGL
     * quedaria con los objetos de una app ya idas y el launcher heredaria
     * handles colgando (lvgl_forget_task toma el monitor el solo). */
    lvgl_forget_task((int) gettid());
    return rc;
}
