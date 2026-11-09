/**
 * @file thread.h
 *
 */
/* Embedded Xinu, Copyright (C) 2009.  All rights reserved. */

#ifndef _THREAD_H_
#define _THREAD_H_

#ifndef __ASSEMBLER__
#include <interrupt.h>
#include <semaphore.h>
#include <debug.h>
#include <stddef.h>
#include <memory.h>
#endif /* __ASSEMBLER__ */

/* unusual value marks the top of the thread stack                      */
#define STACKMAGIC  0x0A0AAAA9

/* thread state constants                                               */
#define THRCURR     1           /**< thread is currently running        */
#define THRFREE     2           /**< thread slot is free                */
#define THRREADY    3           /**< thread is on ready queue           */
#define THRRECV     4           /**< thread waiting for message         */
#define THRSLEEP    5           /**< thread is sleeping                 */
#define THRSUSP     6           /**< thread is suspended                */
#define THRWAIT     7           /**< thread is on semaphore queue       */
#define THRTMOUT    8           /**< thread is receiving with timeout   */
#define THRMIGRATE  9           /**< thread is being migrated           */

/* miscellaneous thread definitions                                     */
#define TNMLEN      16          /**< length of thread "name"            */
#define NULLTHREAD  0           /**< id of the null thread              */
#define BADTID      (-1)        /**< used when invalid tid needed       */

/* thread initialization constants */
#ifndef INITSTK
#define INITSTK     65536       /**< initial thread stack size          */
#endif
#define INITPRIO    20          /**< initial thread priority            */

/* Suelo de pila que impone create() (system/create.c redondea hacia aqui).
 * El upstream usa 16 KB, pero en el P4 el heap esta en L2MEM: 432320 B en
 * total y, con las hebras que ya viven al arrancar (shell, bootTask, dash,
 * tcpTimer, cada una con INITSTK), quedan ~65 KB libres.  Con 16 KB de suelo
 * solo cabian 4 hebras y la red no arrancaba: create() devolvia SYSERR y la
 * shell imprimia SHELL_CHILDERR.  Se baja a 4 KB para que cada hebra pida lo
 * que de verdad necesita; el techo de cada una se mide con `ps', que imprime
 * el pico observado por tarea (stack_peak(), system/stack.c).  El tcpTimer ya
 * no usa INITSTK: pide TCP_TIMER_STK (include/network.h). */
#define MINSTK      4 * 1024    /**< minimum thread stack size          */

/* JVM_STK y JVM_STK_LAUNCH (16 KB para una hebra java, 32 KB para el launcher)
 * vivian aqui con su medicion de picos en placa.  Se van con la pila grafica,
 * aparcada en parked/java-gui/: la documentacion lleva los dos valores y el
 * por que.  Sin JVM en el kernel no hay hebra que los use. */
#ifdef JTAG_DEBUG
#define INITRET   debugret      /**< threads return address for debug   */
#else                           /* not JTAG_DEBUG */
#define INITRET   userret       /**< threads return address             */
#endif                          /* JTAG_DEBUG */

/* Reschedule constants for ready  */
#define RESCHED_YES 1           /**< tell ready to reschedule           */
#define RESCHED_NO  0           /**< tell ready not to reschedule       */

/* Check for invalid thread ids.  Note that interrupts must be disabled */
/* for the condition to hold true between statements.                   */
#define isbadtid(x) ((x)>=NTHREAD || (x)<0 || THRFREE == thrtab[(x)].state)

/** Maximum number of file descriptors a thread can hold */
#define NDESC       5

/** Maximum number of local devices */
#define NLOCDEV     10

/* Expose sizeof(struct thrent) and offsetof(struct thrent, stkdiv) to 
 * assembly files. */
#define THRENTSIZE 160
#define STKDIVOFFSET 104

#ifndef __ASSEMBLER__

/**
 * Defines what an entry in the thread table looks like.
 */
struct thrent
{
    uchar state;                /**< thread state: THRCURR, etc.        */
    int prio;                   /**< thread priority                    */
    void *stkptr;               /**< saved stack pointer                */
    void *stkbase;              /**< base of run time stack             */
    ulong stklen;               /**< stack length in bytes              */
    char name[TNMLEN];          /**< thread name                        */
    irqmask intmask;            /**< saved interrupt mask               */
    semaphore sem;              /**< semaphore waiting for              */
    tid_typ parent;             /**< tid for the parent thread          */
    message msg;                /**< message sent to this thread        */
    bool hasmsg;                /**< nonzero iff msg is valid           */
    struct memblock memlist;    /**< free memory list of thread         */
    int fdesc[NDESC];           /**< device descriptors for thread      */

    /* ---- instrumentación / contabilidad (2026) ---- */
    uint32_t cpu_ticks;         /**< ticks de 1 ms cargados a la tarea  */
    uint32_t born_ticks;        /**< clktotal al crear la tarea         */
    uint32_t gen;               /**< generación del slot (detecta reuso) */
    uint32_t stk_peak;          /**< uso máximo observado de pila       */
    uint8_t stk_overflow;       /**< TRUE si el SP salió de la pila     */
    uint8_t stk_pad[3];

    /* ---- imagen ELF de la hebra: la libera kill() (system/kill.c) ---- */
    bool elf_task;              /**< TRUE si la hebra corre un app ELF   */
    void *elf_img;              /**< cabecera del bloque PSRAM del app   */
    ulong elf_size;             /**< bytes del bloque, cabecera incluida */
};

extern struct thrent thrtab[];
extern int thrcount;            /**< currently active threads           */
extern tid_typ thrcurrent;      /**< currently executing thread         */

/* Inter-Thread Communication prototypes */
syscall send(tid_typ, message);
message receive(void);
message recvclr(void);
message recvtime(int);

/* Thread management function prototypes */

tid_typ create(void *procaddr, uint ssize, int priority,
               const char *name, int nargs, ...);
tid_typ gettid(void);
syscall getprio(tid_typ);
syscall kill(int);
int ready(tid_typ, bool);
int resched(void);
syscall sleep(uint);
syscall unsleep(tid_typ);
syscall yield(void);

/**
 * @ingroup threads
 *
 * Enter some kind of powerdown state (if it exists) that suspends
 * execution until an interrupt is detected.
 */
void pause(void);

void userret(void);

#endif /* __ASSEMBLER__ */

#endif                          /* _THREAD_H_ */
