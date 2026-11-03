/**
 * @file xsh_irqmap.c
 *
 * CARGADOR DE REFERENCIA, NO COMPILADO.  Vease README.md de este directorio; si
 * algun dia se quiere el comando en la consola, hay que copiar este .c a shell/ y
 * alambrearlo (Makerules + commandtab + include/shell.h) y reconstruir limpio.
 *
 * Esta version es autocontenida a proposito: lee el CLIC con su propia definicion
 * de registros en vez de llamar a una funcion nueva en
 * system/platforms/esp32p4/clic.c, para no dejar huella en el kernel.  Las
 * constantes (P4_CLIC_BASE, P4_CLIC_NIRQ, UART0_IRQn, IRQ_TIMER, mtime/mtimecmp
 * del CLINT) si vienen de las cabeceras del proyecto.
 *
 * Para que sirve: ver QUE interrupciones hay puestas antes de culpar al codigo que
 * se estaba ejecutando.
 *
 * Motivo concreto: 'runblob thrash' paro la placa con
 *
 *     **** irq with null vector: mcause=0xb8000007 irq=7
 *     **** clic ip=0x01 ie=0x01 attr=0xc1
 *     **** interruptVector[7]=0x00000000
 *
 * El id 7 del mcause NO es un periferico: es IRQ_TIMER (include/conf.h:75), el
 * machine timer de RISC-V.  Lo habilita clkinit() (system/clkinit.c:56-57) y lo
 * genera el CLINT al comparar mtime contra mtimecmp
 * (system/platforms/esp32p4/riscvclk.c:39-46).  No confundir con PMU1_IRQn = 7
 * (machine/sbl/esp32p4.h:30, copia en include/esp32p4.h:108): ahi el 7 es el numero
 * de fuente ETS del enum de ESP-IDF, otro espacio de nombres y otra cosa.  Este
 * comando existe para no volver a adivinar: aqui se lee el controlador.
 *
 * Lo anomalo de aquella medida es el vector 0 con ie=1.  En este arbol
 * interruptVector[] solo lo escriben tres sitios: clic_init() la llena de
 * p4_irq_default, clkinit() pone clkhandler en la 7 y uartHwInit() pone el del
 * device en su linea.  Ninguno escribe nunca 0, asi que un vector nulo significa
 * que el .bss fue pisado -- y encaja con el resto de la medida: en las corridas
 * anteriores el nucleo habia ejecutado instrucciones de basura sacadas de una linea
 * L2 rancia (mtval=0xaaaaaaab), y una instruccion de basura puede guardar en una
 * direccion cualquiera.
 *
 * Paseo recomendado: 'irqmap' tras un arranque limpio -- se espera la linea 7 con
 * ie=1 y su clkhandler, y la 31 (UART0_IRQn, include/esp32p4.h:121) con el handler
 * del device -- y 'irqmap' otra vez tras cualquier trap.  Si la 7 salia bien y
 * luego sale 0, la corrupcion la causo ejecutar basura, no el arbol.
 *
 * Y la regla que deja escrita para el SDIO/WiFi: ninguna linea puede quedar con
 * ie=1 sin su interruptVector[] puesto ANTES, porque el dispatch cae en
 * irq_missing() (system/platforms/esp32p4/clic.c:61), que termina en for(;;) y para
 * la placa hasta un reset.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <kernel.h>
#include <interrupt.h>                  /* interruptVector[], irqmask            */
#include <clock.h>                      /* clkticks, clktotal                    */
#include <riscv.h>                      /* read_csr(mcause)                      */
#include <esp32p4.h>                    /* P4_CLIC_BASE, P4_CLIC_NIRQ, CLINT     */

/** Bloque de control por linea: 4 bytes en 0x20801000 + 4*id, verificado contra
 *  soc/clic_reg.h del HAL.  +0 ip (pendiente), +1 ie (habilitada), +2 attr
 *  (bit 0 SHV = vectored por hardware, bits 2:1 TRIG, bits 7:6 MODE=11 en M-mode
 *  y son de solo lectura), +3 ct (prioridad, reset 0x1f). */
typedef struct
{
    volatile uint8_t ip;
    volatile uint8_t ie;
    volatile uint8_t attr;
    volatile uint8_t ct;
} clicint_t;

typedef struct
{
    volatile uint32_t mcliccfg;
    volatile uint32_t clicinfo;
    volatile uint32_t mintthresh;
    clicint_t interrupt[P4_CLIC_NIRQ] __attribute__((aligned(0x1000)));
} clic_t;

#define REF_CLIC     ((volatile clic_t *)P4_CLIC_BASE)
#define REF_NIRQ     P4_CLIC_NIRQ
#define REF_EXT_OFF  16                 /* primera linea externa                 */

/** Matriz de enrutamiento: el valor es la fuente ETS que sube a esa linea; 0
 *  significa que la linea no tiene fuente asignada y solo la puede disparar un
 *  numero de interrupcion privilegiado (p.ej. el machine timer, id 7). */
#define REF_INTMTX(n) (*(volatile uint32_t *)(INTERRUPT_CORE0_BASE + 4UL * (n)))

shellcmd xsh_irqmap(int nargs, char *args[])
{
    uint32_t id;
    uint32_t armadas = 0;

    (void)nargs;
    (void)args;

    kprintf("CLIC@0x%08x  mcliccfg=0x%08x  mintthresh=0x%02x  mcause=0x%08x\r\n",
            (uint32)P4_CLIC_BASE, (uint32)REF_CLIC->mcliccfg,
            (uint32)(REF_CLIC->mintthresh & 0xFFU), (uint32)read_csr(mcause));
    kprintf("mtime.lo=0x%08x  mtimecmp.lo=0x%08x  clkticks=%lu  clktotal=%lu\r\n",
            (uint32)P4_MTIME_LO, (uint32)P4_MTIMECMP_LO,
            (ulong)clkticks, (ulong)clktotal);
    kprintf("id ip ie attr ct  vector      mtx  nota\r\n");

    for (id = 0; id < REF_NIRQ; id++)
    {
        uint32_t ip = REF_CLIC->interrupt[id].ip;
        uint32_t ie = REF_CLIC->interrupt[id].ie;
        const char *nota = "";

        if (id == IRQ_TIMER)
        {
            nota = "<- IRQ_TIMER (machine timer, mtimecmp)";
        }
        else if (id == UART0_IRQn)
        {
            nota = "<- UART0 (consola)";
        }

        /* Se listan las lineas con ip o ie armados, y siempre la 7 y la 31 para
         * tener referencia.  El interes esta en cruzar ie con interruptVector[]:
         * ie=1 con vector 0 es la combinacion que para la placa. */
        if (ip == 0U && ie == 0U && id != IRQ_TIMER && id != UART0_IRQn)
        {
            continue;
        }
        if (ip != 0U || ie != 0U)
        {
            armadas++;
        }
        kprintf("%2lu %lu  %lu  0x%02x 0x%02x  0x%08x  %d   %s\r\n",
                (unsigned long)id, (unsigned long)ip, (unsigned long)ie,
                (uint32)REF_CLIC->interrupt[id].attr,
                (uint32)REF_CLIC->interrupt[id].ct,
                (uint32)(uintptr_t)interruptVector[id],
                (id >= REF_EXT_OFF) ? (int)REF_INTMTX(id) : -1,
                nota);
    }

    kprintf("lineas con ip o ie armadas: %lu\r\n", (unsigned long)armadas);

    return OK;
}
