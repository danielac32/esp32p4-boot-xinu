/**
 * @file taskmon.h
 *
 * Tabla de tareas para apps cargadas como ELF.  El kernel rellena una lista
 * con las tareas que no estan THRFREE; un app de "monitor de tareas" la
 * recorre como quiera.  Una sola llamada por refresco; no hay memoria que
 * reservar ni liberar.
 *
 * Los estados son los de thread.h (THRCURR=1, THRREADY=3, ...); los nombres
 * para pintarlos en pantalla se quedan en el app.
 */
#ifndef _TASKMON_H_
#define _TASKMON_H_

#define TASKMON_MAX   24           /* cuantas filas caben en pantalla          */
#define TASKMON_NAME  16

struct taskmon_row
{
    int tid;
    int state;                  /* THRCURR, THRREADY, ...                     */
    int prio;
    int parent;
    unsigned long cpu_ticks;    /* ms de CPU cargados a la tarea (1 ms tick)  */
    unsigned long born_ticks;   /* clktotal al nacer                           */
    unsigned long stk_peak;     /* bytes de pila usados como maximo           */
    unsigned long stk_total;    /* bytes de pila reservados                   */
    int elf_task;               /* 1 si la tarea corre un app ELF             */
    char name[TASKMON_NAME];
};

/**
 * Rellena @out con hasta @max filas de tareas no THRFREE, en orden de tid.
 * @return cuantas filas escribio (0 si no hay ninguna o el puntero es NULL)
 */
int taskmon_get(struct taskmon_row *out, int max);

#endif /* _TASKMON_H_ */
