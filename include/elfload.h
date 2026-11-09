/**
 * @file elfload.h
 *
 * El cargador de apps ELF del kernel, en una sola funcion publica: elf_run().
 * Lee un ELF ET_REL RV32 ('ld -r') de la microSD, lo coloca en PSRAM, lo
 * enlaza contra la tabla de system/exports.c, le parchea las relocaciones y
 * salta a su '_start' en la hebra que llama.  El unico cliente de shell es
 * 'elf' (shell/xsh_elf.c).
 *
 * Modelo de ejecucion, y por que es asi:
 *
 *   - El app es C estándar: 'int main(int argc, char *argv[])'.  El cargador
 *     salta al '_start' con argc en a0 y argv en a1, que es justo como entra
 *     un main normal, y el start.S (examples/p4-elfsdk/apps/lib/start.S) los
 *     pasa tal cual.  argv[0] es la ruta con la que se llamo al app y
 *     argv[argc] es NULL.
 *   - El app corre en la hebra que llama a elf_run(), con su pila; por eso el
 *     start.S solo salva ra y s0.  Las cadenas de argv son las del llamador:
 *     valen mientras el app corre, y no hay que guardarselas para despues.
 *   - 'exit' mata la hebra: es elf_exit() del cargador y su cuerpo es
 *     kill(gettid()).  Como elf_run() le dejo a la hebra los campos
 *     elf_img/elf_size/elf_task, el kill() del kernel suelta el bloque de
 *     PSRAM del app antes de irse, y la hebra no ejecuta nada mas.  Si el app
 *     volviera sin llamar a exit (un _start propio), elf_run() suelta el
 *     bloque igual antes de devolver.  El modelo es de un nivel: un app que
 *     lance a otro con elf_run() y cuyo hijo salga con exit() se lleva la
 *     hebra entera y deja el bloque del app de fuera reservado.
 *   - El codigo y los datos quedan en la ventana DIRECTA de PSRAM (0x88......)
 *     y ahi se ejecutan: por esa ventana no hay linea de icache que valga,
 *     asi que el fetch lee la celda tal cual.  Eso NO evita la operacion de
 *     cache: el kernel deja lineas SUCIAS de esas mismas celdas (las lineas
 *     son de 64 B y se comparten con la cabecera que escribe psram_malloc) y
 *     el write-back es ASINCRONO -- se vacian cuando quiere el controlador y
 *     tapan lo copiado.  El cargador hace writeback+invalidate antes de copiar
 *     e invalidate despues, y coloca el codigo alineado a 64 con dos lineas de
 *     reserva.  Medido en placa: sin eso, la coda del crt0 amanecia con
 *     0xaaaa... y mcause=2.
 *   - Un fallo de enlazado aborta ANTES de saltar.  Una relocacion sin aplicar
 *     apunta a 0x0000xxxx y en la placa eso es mcause=2 o mcause=5, que es el
 *     cuelgue de siempre: mejor un mensaje y SYSERR.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _ELFLOAD_H_
#define _ELFLOAD_H_

/** Maximos del app que se admite; son topes de sanidad, no de rendimiento. */
#define ELF_MAX_SECC    256     /* secciones ALLOC + tablas .rela             */
#define ELF_MAX_SIMB    2048    /* entradas de .symtab                        */
#define ELF_MAX_NOMBRE  32      /* etiqueta que se imprime en el volcado      */
#define ELF_MAX_ARGS    32      /* argc admitido (SHELL_MAXTOK es 32)         */

/**
 * El cargador entero: saca el ELF de la microSD, lo coloca en PSRAM, lo
 * enlaza contra system/exports.c, le parchea las relocaciones y ejecuta su
 * '_start' en la hebra que llama.  Si el app llama a 'exit', la hebra muere
 * con kill(gettid()) y el kernel suelta su bloque de PSRAM; si el app
 * devuelve, el bloque se suelta aqui y elf_run() vuelve.
 *
 * @param ruta   ruta del ELF en el volumen FAT montado (fl_fopen)
 * @param argc   argumentos QUE SIGUEN a la ruta, como en la shell; el cargador
 *               pone 'ruta' en argv[0] del app
 * @param argv   punteros a las cadenas del llamador (no se copian)
 * @return OK si el app volvio sin exit() y su PSRAM quedo libre, SYSERR si no
 *         se pudo cargar (nada queda reservado)
 */
int elf_run(const char *ruta, int argc, char *argv[]);
int elf_run_task(const char *ruta, int argc, char *argv[]);
/* ---- quien es el dueno de una direccion ---------------------------------------
 * Un trap con 'mepc=0x88......' no dice nada por si solo: puede ser el codigo de
 * un app y aqui se traduce a nombre y rango.  Las funciones de lectura no cogen
 * monitor ni reservan nada, asi que la ruta del trap las puede llamar (las
 * referencia system/arch/riscv/panic.c como simbolos debiles).
 */

/** Nombre de la imagen que corre ahora mismo si 'dir' cae en su bloque, o NULL. */
const char *elf_quien(uintptr_t dir);

/** La imagen que corre ahora mismo, por kprintf; lo llama el volcado de trap. */
void elf_dump_trap(void);

#endif                          /* _ELFLOAD_H_ */
