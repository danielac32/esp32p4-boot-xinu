# El segundo núcleo HP del ESP32-P4 (core1) en este kernel

Documento de diseño. **El API de la sección 4 está dibujado, no implementado**:
hoy el core1 está aparcado y no ejecuta nada. Lo que sí está implementado y
verificado en placa es el aparcado formal (pila propia + `mtvec` + testigo), que
es la base sobre la que se monta todo lo demás.

---

## 1. Estado actual

Cadena de arranque:

1. El SBL inicializa el SoC y **libera core1 de reset** apuntándolo a su propio
   `.L_core1` ([machine/sbl/boot.S:45-56](machine/sbl/boot.S)): escribe
   `BOOT_ADDR_HP_CORE1` (0x50110164), setea el bit 4 de `SOC_CLK_CTRL0`
   (0x500E6014) y limpia el bit 8 de `HP_RST_EN0` (0x500E60C0).
2. Los dos harts caen en el mismo sitio: `__kernel_entry_point` = 0x40005000,
   `_start` del kernel ([machine/sbl/boot.S:64-66](machine/sbl/boot.S)).
3. `reset_handler` lee `mhartid` y, si no es 0, salta a `park`
   ([loader/platforms/esp32p4/start.S:92](loader/platforms/esp32p4/start.S#L92)).
4. `park` ([start.S:150-180](loader/platforms/esp32p4/start.S#L150-L180)) le da la
   pila que el SBL no le puso (`sp = _core1stk_end`), apaga `mstatus.MIE`, le
   pone su propio `mtvec` y entra en un bucle `wfi`. Antes de dormirse escribe un
   testigo `{0xC0DE1234, mhartid, sp}` en `_core1stk`.
5. Esa pila vive en su propia sección de 2 KB, `.core1stk`, detrás de `.bss` y
   antes de `_end`
   ([compile/platforms/esp32p4/ld.script:115-122](compile/platforms/esp32p4/ld.script#L115-L122)),
   para que ni el borrado de `.bss` de core0 ni el heap de L2MEM la toquen.

Comprobación en placa (sin JTAG), con el kernel flasheado:

```
xsh$ memdump -C 0x4ff051c0 16
4ff051c0  34 12 de c0 01 00 00 00  c0 59 f0 4f ...
          ^magic       ^hart=1      ^sp == _core1stk_end
```

Verificado el 30/10/2026: core1 aparcado, core0 con `ps`/`uptime`/reloj
correctos. La dirección exacta sale de `nm compile/xinu.elf | grep _core1stk`.

## 2. Qué es privado de cada hart y qué es compartido

| Recurso | Alcance | Consecuencia práctica |
|---|---|---|
| CSRs: `mtvec` (0x305), `mtvt` (0x307), `mstatus`, `mepc`, `mie` | **por hart** | core0 no puede configurar las trampas de core1; cada hart programa los suyos. Cada hart lo hace al entrar en C: `init_cpu()` (llamada
desde `call_start_cpu0()`, que corre en ambos harts) pone `mtvec`/`mtvt` en
[cpu_start.c:385-390](file:///home/daniel/esp-idf/components/esp_system/port/cpu_start.c#L385-L390);
`call_start_cpu1()`, que corre en core0 para soltar al secundario, hace lo propio
con los de core0 en [:214-219](file:///home/daniel/esp-idf/components/esp_system/port/cpu_start.c#L214-L219).
Ninguno de los dos puede configurar el hart vecino escribiendo el CSR. |
| Bloque CLIC | **por hart, con alias** | Desde *cualquier* hart, `P4_CLIC_BASE` ([include/esp32p4.h:146](include/esp32p4.h#L146)) = 0x20800000 apunta al CLIC del **hart que ejecuta**, y `+0x10000` (0x20810000) al del **otro**. Lo documenta IDF con `DUALCORE_CLIC_CTRL_OFF = 0x10000` ([interrupt_reg.h:17-26](file:///home/daniel/esp-idf/components/soc/esp32p4/include/soc/interrupt_reg.h#L17-L26), [clic_reg.h:15-18](file:///home/daniel/esp-idf/components/soc/esp32p4/include/soc/clic_reg.h#L15-L18)). Nuestro `clic.c` sólo usa la alias actual, o sea la de core0. |
| Matriz de enrutado ETS → línea CLIC | **por hart** | Las dos bases ya están definidas: `INTERRUPT_CORE0_BASE` 0x500D6000 e `INTERRUPT_CORE1_BASE` 0x500D6800 ([include/esp32p4.h:73-74](include/esp32p4.h#L73-L74)). `enable_irq()` escribe en la de core0 ([system/platforms/esp32p4/clic.c:104-116](system/platforms/esp32p4/clic.c#L104-L116)). |
| Excepciones en modo CLIC | — | No se indexan: el hart salta a `MTVEC[31:6] << 6`, un único handler (por eso `_vector_table` del IDF es una sola instrucción con `.balign 0x40`, [vectors_clic.S:41-56](file:///home/daniel/esp-idf/components/riscv/vectors_clic.S#L41-L56)). Sí se indexan las interrupciones: vector en `mtvt + 4*irq`. |
| `mtime`/`mtimecmp` (CLINT) | reloj compartido | `clkcount()`/`clkupdate()` tienen un único consumidor. Ver 7.4. |
| Periféricos (UART, SPI, SDIO, GPIO) | compartidos | Dos harts sobre el mismo `uarttab`/buffer = corrupción. Un periférico por hart sí es seguro. |
| L2MEM y PSRAM | compartidos, **caché por core** | Ver 7.1: hay un precedente ya documentado en este árbol. |

## 3. Lo que puedes hacer y lo que no

**Puedes, sin tocar nada**: dejarlo aparcado (casi no consume en `wfi`) o no
liberarlo en el SBL. Nada más.

**Puedes con un cambio acotado** (secciones 4-6): que core1 ejecute **una única
función C tuya**, aislada del kernel — su pila, su `mtvec`, su CLIC, su
periférico, y un buzón de memoria compartida con core0. Es un coprocesador, no
un segundo planificador.

**No puedes**, sin un proyecto SMP (sección 8):

- Usar semáforos, colas ni `readylist` desde los dos harts. Xinu protege sus
  secciones críticas con `disable()`/`restore()` sobre el `mstatus.MIE` **local**
  ([system/resched.c:45](system/resched.c#L45)): cada hart apaga sus
  interrupciones, no las del vecino, y los dos entran a la vez.
- Llamar a nada del runtime: `printf`/`kprintf` (tty + `uarttab`), `stkget` /
  `memget` / `psram_*` (heap único), `create`/`resched`/`sleep`, drivers.
- Compartir el handler de una IRQ: `p4_irq_entry` despacha contra
  `interruptVector[]` global y `resdefer` global
  ([device/uart-esp32p4/uartInterrupt.c:65](device/uart-esp32p4/uartInterrupt.c#L65)).
- Suponer `cpuid`: es un global que `_start` fija a 0
  ([start.S:144-146](loader/platforms/esp32p4/start.S#L144-L146)).

## 4. API propuesta

Tres toques, sin cambiar el SBL (basta `make flashxinu`):

```
include/core1.h                     API publica (la usa core0)
system/platforms/esp32p4/core1.c    buzon, arranque, telemetria
loader/platforms/esp32p4/start.S    park(): de dormir a mirar el buzon
```

### 4.1 `include/core1.h`

```c
/**
 * core1.h — segundo nucleo HP del ESP32-P4.
 *
 * Modelo: core0 encarga UNA funcion a core1 con un puntero de argumento.
 * La funcion que core1 ejecuta no puede llamar al runtime Xinu: solo MMIO
 * que core0 no use, y memoria que core0 le haya entregado.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _CORE1_H_
#define _CORE1_H_

#include <stddef.h>
#include <stdint.h>

#define CORE1_MAGIC   0xC0DE1234u

/* Barrera de publicacion: no hay helper en include/riscv.h, se define aqui.
 * "fence" sin operandos = iorw,iorew (ordenamiento de memoria completo). */
#define core1_fence()   __asm__ volatile("fence" ::: "memory")

/**
 * Bloque de control.  Un solo writer por campo, salvo magic/hartid/park_sp
 * (core1) y entry/arg/stack_top/req_seq (core0).  Si 7.1 muestra problemas de
 * coherencia, este bloque va en la region no cacheada.
 */
struct core1_block {
    volatile uint32_t magic;        /* CORE1_MAGIC cuando park() ha ejecutado  */
    volatile uint32_t hartid;       /* leido de mhartid por el propio core1    */
    volatile uint32_t park_sp;      /* sp del parking                          */
    volatile uint32_t req_seq;      /* core0 lo incrementa al encargar         */
    volatile uint32_t ack_seq;      /* core1 copia req_seq al arrancar         */
    volatile uint32_t done_seq;     /* core1 copia req_seq al terminar         */
    volatile uint32_t heartbeat;    /* avanza mientras core1 esta en el loop   */
    volatile void   (*entry)(void *);
    volatile void    *arg;
    volatile uint32_t stack_top;    /* pila de trabajo asignada por core0      */
    volatile uint32_t trap_cause;   /* mcause si core1 trapo (0 = nunca)       */
    volatile uint32_t trap_epc;
};

extern struct core1_block core1;

/* Espera el testigo del parking. Devuelve OK si core1 esta aparcado. */
int  core1_bringup(void);

/* Encarga entry(arg) con una pila de stack_bytes (0 = 8192).  Devuelve OK si
 * el encargo fue aceptado; SYSERR si core1 no esta operativo o si hay un
 * trabajo en curso (usar core1_busy() antes). */
int  core1_run(void (*entry)(void *), void *arg, uint32_t stack_bytes);

int  core1_busy(void);              /* 1 si hay un encargo sin terminar */
int  core1_done(void);              /* 1 si el ultimo encargo termino   */
int  core1_alive(void);             /* 1 si el heartbeat avanza         */

/* Devuelve y limpia el trap de core1, si lo hubo. */
int  core1_take_trap(uint32_t *cause, uint32_t *epc);

#endif /* _CORE1_H_ */
```

### 4.2 `system/platforms/esp32p4/core1.c`

```c
#include <kernel.h>
#include <stddef.h>           /* NULL */
#include <memory.h>           /* stkget() */
#include <core1.h>

/* Vive en su propia sección NOLOAD (.core1bss), no en .data ni en .bss:
 * park() corre ANTES del copiado/.bss de core0 (ver 4.5), así que lo limpia
 * el propio core1 antes de publicar el testigo. */
struct core1_block core1 __attribute__((section(".core1bss"), aligned(16)));

int core1_bringup(void)
{
    uint32_t i;

    for (i = 0; i < 200000UL && core1.magic != CORE1_MAGIC; i++)
    {
        core1_fence();
    }
    return (core1.magic == CORE1_MAGIC) ? OK : SYSERR;
}

int core1_busy(void)
{
    return (core1.req_seq != core1.done_seq);
}

int core1_done(void)
{
    return (!core1_busy());
}

int core1_alive(void)
{
    uint32_t prev = core1.heartbeat;

    core1_fence();
    return (core1.heartbeat != prev);
}

int core1_run(void (*entry)(void *), void *arg, uint32_t stack_bytes)
{
    void    *stk;
    uint32_t bytes;

    if (entry == NULL || core1.magic != CORE1_MAGIC)
    {
        return SYSERR;                  /* core1 no esta aparcado/operativo */
    }
    if (core1_busy())
    {
        return SYSERR;                  /* un trabajo en curso */
    }
    bytes = (stack_bytes == 0) ? 8192 : (stack_bytes + 15) & ~15UL;

    /* La pila la asigna core0 del heap de Xinu y core1 solo la usa:
     * nunca la libera (stkfree desde core1 seria un bug de doble dono). */
    if ((stk = stkget(bytes)) == NULL)
    {
        return SYSERR;
    }

    core1.arg         = arg;
    core1.stack_top   = (uint32_t)((uint32_t *)stk + bytes / 4);
    core1.entry       = entry;
    core1_fence();                      /* publicar antes de subir el seq */
    core1.req_seq++;
    return OK;
}

int core1_take_trap(uint32_t *cause, uint32_t *epc)
{
    if (core1.trap_cause == 0)
    {
        return FALSE;
    }
    *cause = core1.trap_cause;
    *epc   = core1.trap_epc;
    core1.trap_cause = 0;
    return TRUE;
}
```

### 4.3 El otro extremo, en `start.S`

`park` se queda igual hasta el testigo, y se reemplaza el bucle `loop:`
([start.S:177-181](loader/platforms/esp32p4/start.S#L177-L181)). Los offsets van
en un bloque `.equ` y **deben casar con `struct core1_block`**:

```asm
/* Offsets de struct core1_block (ver include/core1.h).  Se comprueban en
 * tiempo de compilacion con un _Static_assert en core1.c. */
    .equ C1B_REQ,     12
    .equ C1B_ACK,     16
    .equ C1B_DONE,    20
    .equ C1B_HB,      24
    .equ C1B_ENTRY,   28
    .equ C1B_ARG,     32
    .equ C1B_STKTOP,  36
    .equ C1B_CAUSE,   40
    .equ C1B_EPC,     44

loop:
    la  t1, core1
    lw  t0, C1B_REQ(t1)
    lw  t2, C1B_ACK(t1)
    beq t0, t2, .Lidle                  /* nada encargado */

    fence                               /* ver req_seq antes de leer el resto */
    lw  t3, C1B_ENTRY(t1)
    lw  a0, C1B_ARG(t1)                 /* a0 se carga ANTES de cambiar sp */
    lw  sp, C1B_STKTOP(t1)
    sw  t0, C1B_ACK(t1)
    jalr t3
    la  sp, _core1stk_end               /* de vuelta la pila del parking */
    lw  t0, C1B_REQ(t1)
    sw  t0, C1B_DONE(t1)
    j   loop

.Lidle:
    lw  t0, C1B_HB(t1)
    addi t0, t0, 1
    sw  t0, C1B_HB(t1)
    wfi                                 /* ver 7.2: con MIE=0 puede no despertar */
    j   loop
    .endfunc
```

Y el handler de aparcado, si se quiere el `trap_cause` relleno: en lugar de
`mtvec = ExceptionVectorTable`, un stub propio de core1 al principio de `.init`
(`balign 0x40`) que guarde `mcause`/`mepc` en el bloque y pare el hart en un `j .`
— así core1 nunca ejecuta código del kernel ni toca el estado compartido. Si se
deja `ExceptionVectorTable`, un trap de core1 acaba en `trap_handler()` y en un
`panic()` de core0 con la consola compartida.

Puntos que no son decorativos:

- **Orden de publicación**: core0 escribe `arg`/`entry`/`stack_top`, `fence`, y
  *después* sube `req_seq`; core1 lee `req_seq`, `fence`, y después el resto.
  Sin eso, con caché por core, core1 puede ver un `entry` anterior al `arg`.
- **Doble cambio de pila**: el `jalr` usa la pila de trabajo; al volver se
  restaura `_core1stk_end`, que es la que sostiene el marco de excepción.
- **Nada de `mret`**: `entry` es una llamada normal, no un handler.
- El `entry` no debe devolver nunca con la pila de parking rota: si necesita
  saber si terminó, que escriba en memoria compartida.
- **El bucle no entra con el bloque limpio**: `park` tiene que escribir ceros en
  `req_seq`/`ack_seq`/`done_seq` y su propio `magic`/`hartid`/`park_sp` *antes*
  de mirar el buzón (4.5). Sin eso, el `beq t0, t2` de la primera iteración
  compara basura y el `jalr` salta a una dirección aleatoria.

### 4.4 Uso desde una tarea del shell

```c
static volatile uint32_t fib_result;

static void fib_core1(void *arg)
{
    uint32_t n = (uint32_t)(uintptr_t)arg;
    uint32_t a = 0, b = 1, i, t;

    for (i = 0; i < n; i++)
    {
        t = a + b;
        a = b;
        b = t;
    }
    fib_result = a;                     /* sin printf: resultado en memoria */
}

/* shell command */
if (core1_run(fib_core1, (void *)(uintptr_t)40, 8192) == OK)
{
    while (!core1_done())
    {
        /* poll: core1 no puede enviar una senal; aqui si se puede sleep(),
         * porque el que duerme es core0. */
    }
    kprintf("core1 fib(40) = %u\n", fib_result);
}
```

### 4.5 Dónde puede vivir el bloque (esto no es un detalle)

`reset_handler` bifurca a `park` en su segunda instrucción
([start.S:91-92](loader/platforms/esp32p4/start.S#L91-L92)), mucho antes del
bucle que copia `.data` y del que limpia `.bss`
([start.S:103-122](loader/platforms/esp32p4/start.S#L103-L122)). Consecuencias,
en este orden:

- **En `.data` no**: core1 entraría al bucle leyendo una L2MEM que core0 todavía
  no ha rellenado, es decir `req_seq`/`ack_seq` con lo que hubiera de la sesión
  anterior. El `jalr` a `entry` se convertiría en un salto a basura. Además son
  dos escritores sobre la misma dirección (el `copy` de core0 y el `ack_seq` de
  core1), con caché por core.
- **En `.bss` tampoco**: core0 la borra *después* de que core1 haya publicado su
  testigo, así que el `magic` desaparece y `core1_bringup()` da `SYSERR` sobre
  un core1 perfectamente vivo.
- **En una sección `NOLOAD` propia, sí** — igual que ya se hizo con la pila de
  aparcado ([ld.script:110-122](compile/platforms/esp32p4/ld.script#L110-L122)).
  Ninguno de los dos bucles de arranque la toca, core1 la pone a cero en `park`
  antes de publicar nada, y core0 no la mira hasta ver `magic`. Es el mismo
  razonamiento que dejó la pila fuera de `.bss`.

Boceto del `ld.script` (junto a `.core1stk`, mismo `> HP_L2MEM`):

```
    .core1bss (NOLOAD) : ALIGN(16)
    {
        _core1bss = .;
        . = . + 256;            /* holgado para struct core1_block */
        _core1bss_end = .;
    } > HP_L2MEM
```

## 5. Alternativa: ciclo de vida "duro" (sin buzón)

Para depurar, o si prefieres poder matar y resucitar el hart: resetearlo y
volver a liberarlo apuntando a tu propio código. Son los tres registros que ya
usa el SBL, pero desde el kernel:

```c
*(volatile uint32_t *)0x500E60C0 |= (1UL << 8);   /* HP_RST_EN0 bit8: core1 en reset */
*(volatile uint32_t *)0x50110164 = (uint32_t)core1_cold_start;  /* BOOT_ADDR_HP_CORE1 */
*(volatile uint32_t *)0x500E6014 |= (1UL << 4);   /* SOC_CLK_CTRL0 bit4: reloj del core */
*(volatile uint32_t *)0x500E60C0 &= ~(1UL << 8);  /* soltar reset */
```

Ojo con el orden: en `boot.S` el SBL escribe la dirección **antes** de habilitar
reloj y soltar reset. Conviene replicar esa secuencia y añadir un pequeño retardo
entre escribir `BOOT_ADDR` y soltar el reset.

`core1_cold_start` entra con `sp` basura otra vez (el reset no lo inicializa),
así que sus primeras instrucciones tienen que ser `la sp, <su pila>` y el
`csrw mtvec`. Ventaja frente al buzón: el hart arranca limpio siempre.
Inconveniente: si estaba a mitad de un encargo, ese estado se pierde sin aviso.

## 6. Verificación de cada paso

| Qué | Cómo se comprueba |
|---|---|
| core1 ejecutó `park` | `memdump -C 0x<_core1stk> 16` → magic/hartid/sp (funcionando hoy) |
| core1 vivo en el loop | `core1_alive()`, o `memdump` del campo `heartbeat` dos veces |
| El encargo se ejecutó | `req_seq == ack_seq == done_seq` en el bloque |
| core1 trapó | `core1_take_trap()` → `trap_cause`/`trap_epc` |
| La pila no choca con el heap | `nm`: `_core1stk_end == _end`, y el *valor* que guarda `memheap` (sp de nulluser, `_end + 16K`) queda por encima |
| Coherencia del buzón | prueba explícita de 7.1 antes de fiarse de nada |

## 7. Abiertos y peligros, antes de escribir código

### 7.1 Coherencia de caché entre los dos HP cores (decide el diseño)

No está verificado en este proyecto. Cada core tiene su caché y L2MEM/PSRAM se
acceden a través de ella. Ya hay un precedente: la tabla `mtvt` tuvo que ir a
`.rodata` (flash) porque la CLIC la lee sin pasar por la coherencia del
data-cache y, escrita en L2MEM, "lee cero"
([system/platforms/esp32p4/clic.c:67-78](system/platforms/esp32p4/clic.c#L67-L78)).

Prueba de 20 minutos antes que nada: core0 pone un flag en L2MEM, core1 lo copia
a otro campo del bloque, con y sin `fence`. Si core1 no lo ve, el bloque de
control va en la región no cacheada (localizar el alias correcto en el TRM /
`soc_memory_map` del P4) o con mantenimiento de caché explícito.

### 7.2 `wfi` con `MIE = 0`

`park` apaga `mstatus.MIE` en core1, así que su `wfi` puede no despertarse nunca
por interrupción. Dos modos, hay que elegir uno:

- **Polling** (el boceto de arriba sin `wfi`): simple y correcto, pero core1
  está al 100 % ocupado siempre. Vale para un demo, no para producto.
- **Dormido con despertar**: requiere habilitar MIE en core1 y una línea real en
  su CLIC (alias del hart + matriz en `INTERRUPT_CORE1_BASE`) como trigger por
  software. Hay que comprobar en el TRM qué despierta `WFI` en la CLIC del P4;
  si nada lo despierta con MIE=0, entonces hay que habilitar MIE y **sí** hace
  falta un `mtvt` válido en core1 (ver 7.3).

### 7.3 `mtvt` de core1

Hoy `park` pone `mtvec` pero no `mtvt`: core1 conserva el valor de reset de ese
CSR. Es inofensivo mientras MIE=0 y su bloque CLIC no esté programado. Si se
habilitan interrupciones en core1, una línea con `SHV=1` vectoría por una
dirección basura. La defensa barata es una tabla inerte de 48 entradas × 4 B
(192 B) en flash, todas apuntando a un stub que pare el hart; se puede poner ya,
aunque no se use.

### 7.4 El timer es único

`mtimecmp` es un registro compartido y `clkhandler()` es su único consumidor
([system/clkinit.c:58](system/clkinit.c#L58),
[system/clkhandler.c:29](system/clkhandler.c#L29)). Para que core1 tenga su tick
hace falta saber si el P4 expone un `mtimecmp` por hart (o el CSR equivalente),
cosa que en este proyecto no está comprobada. Sin eso, core1 no puede tener su
propio `sleep()`/timer y debe trabajar por encargo.

### 7.5 Regla operativa de lo que no se toca

Desde la función que corre en core1, prohibido: `printf`/`kprintf`,
`stkget`/`stkfree`/`memget`/`memfree`/`psram_*`, `wait`/`signal`/semáforos,
`create`/`ready`/`resched`/`sleep`, `read`/`write`/`control`/`getc`/`putc`.
Sólo punteros que core0 entregó y MMIO de un periférico que core0 no use.

## 8. Orden de trabajo sugerido

1. Prueba de coherencia (7.1) — 20 min, condiciona todo lo demás.
2. Tabla `mtvt` inerte + stub de trap propio de core1 (defensa, ~200 B).
3. Sección `.core1bss` (NOLOAD) en `ld.script` + `struct core1_block` ahí, con
   el testigo movido al bloque (se lee por nombre), `_Static_assert` de los
   offsets, auto-limpieza del bloque en `park`, y bucle de buzón en modo polling
   con heartbeat (ver 4.5 antes de elegir dónde vive el bloque).
4. `core1_bringup()` en `platforminit`, y un comando de shell que reporte
   magic/heartbeat/trap.
5. Un periférico dedicado a core1 (candidato: un UART/I2C/SPI que la consola no
   use), con su CLIC en la alias del hart y su matriz en 0x500D6800.
6. Anillo SPSC core0↔core1 si el encargo único se queda corto.

SMP de verdad (hilos Xinu planificados en los dos cores) no entra en este
documento: exige spinlocks atómicos (`lr.w`/`sc.w`, disponibles: el kernel se
compila `-march=rv32imafc`) en todas las secciones críticas, `cpuid` por hart,
colas de listos por hart, pila de IRQ por hart, y una auditoría de coherencia de
las estructuras del kernel. Xinu upstream no trae ninguna de esas capas.

## 9. Referencias

- Este árbol: [machine/sbl/boot.S](machine/sbl/boot.S),
  [machine/sbl/SBL.ld](machine/sbl/SBL.ld),
  [loader/platforms/esp32p4/start.S](loader/platforms/esp32p4/start.S),
  [system/platforms/esp32p4/clic.c](system/platforms/esp32p4/clic.c),
  [compile/platforms/esp32p4/ld.script](compile/platforms/esp32p4/ld.script).
- ESP-IDF local, fuente fiable de registros del P4:
  [`soc/clic_reg.h`](file:///home/daniel/esp-idf/components/soc/esp32p4/include/soc/clic_reg.h),
  [`soc/interrupt_reg.h`](file:///home/daniel/esp-idf/components/soc/esp32p4/include/soc/interrupt_reg.h),
  [`riscv/vectors_clic.S`](file:///home/daniel/esp-idf/components/riscv/vectors_clic.S),
  [`esp_system/port/cpu_start.c`](file:///home/daniel/esp-idf/components/esp_system/port/cpu_start.c).
- [Interrupt Allocation — ESP32-P4 (ESP-IDF)](https://docs.espressif.com/projects/esp-idf/en/v5.3.3/esp32p4/api-reference/system/intr_alloc.html):
  dos cores, 32 interrupciones externas cada uno, 3 fuentes locales (CLINT) por
  core, prioridades programables de forma independiente.

---

Construcción y flasheo de lo que viva en el kernel: `cd compile && make` y
`make flashxinu`. Si `esptool` no está en el PATH (venv de ESP-IDF):
`make ESPTOOL=/home/daniel/esp-idf-py/bin/esptool flashxinu`. Los cambios en
`machine/sbl` o `machine/ulp` requieren `make flash` (las tres imágenes) y
re-verificar el arranque con el `memdump` del testigo.
