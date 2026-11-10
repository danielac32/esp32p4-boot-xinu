# SMP real en Xinu-P4: análisis de factibilidad y diseño

Documento de diseño y evaluación. **Nada de esto está implementado ni parcheado.**
La placa estaba desconectada al hacer este análisis; todo lo marcado como
"medido/verificado" lo está en `docs/port-esp32p4/01-memoria-nucleos.md` o en el
código. Lo marcado como **por verificar** requiere la placa y va en la Fase 0.
La decisión de implementarlo es del dueño (ver §11).

---

## 1. Veredicto

**Factible con coste medio-alto, sin cambios imposibles ni pérdidas de placa.**
El hallazgo que decide es el §3.1: la L1-D es **compartida** entre los dos HP
cores (un único `CACHE_MAP_L1_DCACHE = BIT(4)`; las L1-I sí existen por core:
`CACHE_MAP_L1_ICACHE_0 = BIT(0)` e `CACHE_MAP_L1_ICACHE_1 = BIT(1)`). Eso significa
que **no hay problema de coherencia de datos** entre cores: un dato escrito por
un core por su ventana con caché es visible para el otro sin operación especial
(el D-cache es uno). Lo que sí hay que hacer es:

1. Convertir `disable()`/`restore()` en un **gran candado del kernel** (BKL) con
   anidamiento por core y liberación en `ctxsw()`.
2. Estado por core: `thrcurrent`, `readylist`, `resdefer`, `cpuid`, pila de IRQ.
3. Timer por core (mtimecmp por hart, §3.4) o SYSTIMER con remapeo.
4. Inicializar el CLIC en el core1 **por el propio core1**, con su alias.
5. Afinidad de hilos por core para no tener que invalidar la L1-I del otro core.

Lo que **no** hace falta: spinlocks finos en los 82 ficheros que usan
`disable()`, tocar el SBL, reescribir los semáforos/colas/monitores/mutexes,
o tocar LVGL, las apps ELF o el cargador (con afinidad).

Riesgo residual alto: drivers compartidos entre cores (consola, LCD, SDMMC,
audio, I2C) y el `kill()` de un hilo que corre en el otro core. Ambos están
acotados en la Fase 3 y Fase 4.

---

## 2. Qué se entiende por "SMP real" aquí

Objetivo: **hebras Xinu planificadas en los dos HP harts**, no un segundo core
con una función tuya. Eso implica que `ready()`/`create()`/`kill()`/semáforos/
sleep/colas funcionan igual a como funcionan hoy, eligiendo el hart que corre.

No es objetivo (y está fuera de este documento): balanceo dinámico por robo,
migración con afinidad fina, hotplug de cores, RT/affinity por prioridad,
SMP en las apps ELF como formato aparte (las apps seguirán siendo hebras
Xinu normales; eliges en qué core nace la hebra al crearla).

---

## 3. Hallazgos de hardware que cambian la factibilidad

### 3.1 L1-D compartida (el punto que decide)

ROM del P4 (`include/esp32p4/rom/cache.h:228-235`):

```c
#define CACHE_MAP_L1_ICACHE_0 BIT(0)
#define CACHE_MAP_L1_ICACHE_1 BIT(1)   /* L1-I de cada core: existen DOS */
#define CACHE_MAP_L1_DCACHE  BIT(4)    /* L1-D: UNA, sin pareja por core */
#define CACHE_MAP_L1_CACHE_MASK (CACHE_MAP_L1_ICACHE_MASK | CACHE_MAP_L1_DCACHE)
```

`cache_ll.h` (espejo en `include/hal/cache_ll.h`) hace lo mismo: invalida/
escribe L1-D con una única máscara, y L1-I con `CACHE_MAP_L1_ICACHE_MASK` (las
dos). **No hay dos L1-D ni dos huestes de D-cache que sincronizar.**

Consecuencias operativas:

- Los datos compartidos entre cores escritos por el CPU (estado del kernel,
  tablas, pilas, buffers) son visibles en ambos sin `CACHE.sync_*` ni
  `fence` de mantenimiento; basta `fence rw,rw` (o los `amo*.aq/.rl`) para el
  orden de memoria, que es lo que dan los candados atómicos.
- El problema real medido en este proyecto (`elf_cache_sync()`,
  `system/elfload.c:149-182`) es **D ↔ L2 ↔ escritura DMA/código**, no D↔D.
  La receta `CACHE.sync_map = L1D|L2` sigue valiendo igual con SMP.
- Los alias `0x88…`/`0x48…` siguen comportándose como hoy para el DMA. El
  core1 debe obedecer la misma regla "un buffer, una ventana".
- `L2MEM` sigue sin alias sin caché (`0x8F…` no existe), pero ya no importa:
  el D-cache es uno.

**Por verificar (Fase 0-1):** que este análisis por máscara se corresponda con
el silicio: escribir por core0, leer por core1, con `fence rw,rw`, sobre
L2MEM y sobre PSRAM. Es la única duda de hardware que puede tumbar el diseño.

### 3.2 L1-I por core → código debe invalidarse por hart

Con hilos **con afinidad** (§5.3) un programa solo ejecuta en el core que lo
cargó o que lo heredó, y su L1-I no tiene líneas viejas de otro core. Si se
hace migración libre, hace falta `CACHE.sync_map = L1I0|L1I1` (o
`CACHE_MAP_L1_ICACHE_MASK` vía ROM) antes de que el otro core ejecute código
recién cargado. En la práctica: con afinidad, no hace falta nada.

### 3.3 CLIC y matriz: por hart, con alias ya conocidos

- `P4_CLIC_BASE = 0x20800000` es la ventana **local** del hart que ejecuta;
  `0x20810000` (+0x10000) es la del otro hart (`DUALCORE_CLIC_CTRL_OFF`).
  `clic.c` solo programa la local.
- La matriz ETS→CLIC es por hart:
  `INTERRUPT_CORE0_BASE = 0x500D6000`, `INTERRUPT_CORE1_BASE = 0x500D6800`
  (ya en `include/esp32p4.h:73-74`); `clic.c:91` escribe solo en la de core0.
- `interruptVector[]` puede seguir siendo global (una sola copia en flash),
  porque el índice es la línea (mcause&0xff), no el core.
- `mtvec`/`mtvt`/`mstatus`/`mepc`/`mstatus.MIE` son por hart. `park` ya pone
  `mtvec` para core1; **no pone `mtvt`** (hay que añadirlo, inerte o real).
- Cada core necesita su propia pila para traps si no usa la del hilo actual.
  Hoy `p4_irq_entry` usa la pila del hilo actual; con afinidad eso sigue
  valiendo (cada hart ejecuta el suyo).

### 3.4 Timer: `mtimecmp` por hart o SYSTIMER

CLINT estándar: `msip` (0x20000000+4·hart) y `mtimecmp`
(0x20004000+8·hart). Nuestra cabecera solo define el hart0
(`include/esp32p4.h:150-154`, `P4_CLINT_BASE 0x20000000`).

- **Por verificar (Fase 0-2):** escribir `mtimecmp` en `0x20004008` (hart1),
  dejar MIE=0 en core1, leer `mtime`, comprobar que no tumba nada; luego
  habilitar en core1 y contar ticks en un bucle. Si el P4 implementa el
  CLINT completo, cada core tiene su propio timer de 1 kHz con el mismo
  `IRQ_TIMER 7` (mcause 7 = machine timer) y el núcleo no necesita nada más.
- Si no existe mtimecmp por hart, alternativa con registros YA documentados:
  **SYSTIMER** `SYSTIMER_BASE 0x500E2000` (`include/esp32p4.h:77`) con 3
  comparadores (`TARGET0 0x34`, `TARGET1 0x38`, `TARGET2 0x3C`) y fuentes ETS
  `SYSTIMER_TARGET0/1/2_IRQn = 53/54/55`. Como el CLIC solo tiene 48 líneas
  (`CLIC_INT_INFO_NUM_INT = 48` en `clic_reg.h`, rango 0-47), hay que
  **reasignar** la fuente (p.ej. 54) a una línea libre (p.ej. 18) con
  `INTERRUPT_CORE1_BASE + 4*18 = 54`, y registrar el handler en
  `interruptVector[18]`. Es más código y una tabla nueva, pero no depende de
  si el CLINT es completo.
- El `sleepq` global puede seguir siendo uno único (lo decrementa core0);
  core1 sólo hace resched de sus hilos en su propio tick.

### 3.5 IPI (interrupción entre cores) para despertar/interrumpir al otro

Necesario para: (a) despertar a un core en `wfi` con trabajo pendiente,
(b) reclamar a un hilo que corre en el otro core (`suspend`/`kill`).

Canditatos, por verificar (Fase 0-3):

1. `msip` CLINT: `*(uint32_t *)(0x20000000 + 4*hart) = 1` → mcause 3 en el
   otro core; se limpia escribiendo 0. Camino estándar si el CLINT es completo.
2. La ventana CLIC del otro hart: escribir el `ip` de una línea libre (p.ej.
   19) en `0x20810000 + 0x1000 + 4*19 + 0 = 1`. No requiere CLINT completo.
3. Último recurso: polling de un flag con `wfi`+un timer; correcto pero con
   latencia y sin despertar inmediato.

### 3.6 L2MEM/PSRAM/pilas

El heap de L2MEM (~450 KB) es compartido y el allocator serializa con
`disable()` (va al BKL, ver §4). Las pilas de los hilos las asigna el core que
hace `create()`; cualquier core puede liberarlas en `kill()` con el BKL. No
hace falta heap por core (fase 1); si más adelante se detecta serialización en
`memget()`/`psram_*()`, se parte el allocator, no la semántica.

### 3.7 Lo que ya NO es problema con afinidad

- **I-cache**: un hilo no migra; nunca ejecuta código cuyo L1-I sea viejo.
- **FPU**: `ctxsw.S` guarda f0-f31/fcsr por hilo; los dos cores salvan/restauran
  su contexto igual que core0. Hace falta `mstatus.FS=11` también en core1.
- **`userret`→`kill(gettid())`**: `kill()` sobre el propio hilo es local; el
  caso remoto está acotado en §7.3.
- **apps ELF**: `system/elfload.c` ya hace `CACHE.sync_map` sobre el bloque;
  `elf_run()` ejecuta la app en la hebra que llama, así que basta crear esa
  hebra en el core deseado. Las apps no se recompilan ni se tocan.
- **LVGL**: `lvgl_lock()` es un monitor (`lock()`/`unlock()`); funciona entre
  cores porque espera en semáforo (con el BKL liberado durante el wait).
  `lvglui` se queda en core0; las apps en core1 dibujan bajo el monitor.

---

## 4. La pieza central: `disable()`/`restore()` como gran candado

Hoy `disable()` (`system/arch/riscv/intutils.S`) es `csrrc mstatus, MIE` y
`restore()` decide solo por el bit MIE. **Es local al hart.** 82 ficheros lo
usan como exclusión mutua global. La estrategia barata y verificable es:

```c
/* sistema/arch/riscv/intutils.S (reescrito) */
irqmask disable(void)          /* anidamiento POR CORE */
{
    irqmask old = local_irq_save();        /* csrrc mstatus, MIE  */
    struct percpu *c = thiscpu();          /* mscratch            */
    if (c->nest++ == 0)
        spin_acquire(&kernel_lock);        /* amoadd/amoor, aq    */
    return old;
}
void restore(irqmask im)
{
    struct percpu *c = thiscpu();
    if (--c->nest == 0)
        spin_release(&kernel_lock);        /* libera con rl       */
    local_irq_restore(im);
}
```

`kernel_lock` es un `amoadd.w` con anidamiento por core; **se libera** solo en
la transición nest 1→0. Es recursivo por core, así que un ISR en el mismo core
que ya tiene el lock no se cuelga.

### 4.1 El caso `ctxsw()`: el lock atraviesa el cambio de contexto

`resched()` hace `disable()`, elige hilo y llama `ctxsw()`. Si el BKL se
libera ahí, el nuevo hilo entra con el lock tomado por el viejo y no lo suelta;
si no se libera, el otro core se queda fuera para siempre. Solución estándar
("release across switch"):

```c
int resched(void)
{
    struct percpu *c = thiscpu();
    ...                                 /* elegir thrnew como hoy */
    throld->klock_depth = c->nest;      /* se guarda por hilo    */
    c->nest = 0;
    spin_release(&kernel_lock);         /* el otro core puede entrar */

    ctxsw(&throld->stkptr, &thrnew->stkptr, asid);

    /* al reanudarse ESTE hilo, aquí vuelve: */
    c->nest = throld->klock_depth;      /* su propia profundidad     */
    if (c->nest) spin_acquire(&kernel_lock);
    ...                                 /* restore(throld->intmask)  */
}
```

El hilo que reanuda restaura **su** profundidad (la guardada en `thrtab`), no
la del que salió. El primer contexto de un hilo nuevo tiene `klock_depth = 0`.
`struct thrent` gana un campo `uint16_t klock_depth;` (+2 B, cabe en el pad de
`stk_pad`/ELF si se quiere; THRENTSIZE 160 → 162 → alinear a 164/168).

### 4.2 Qué pasa con `wait()`/`sleep()`/`signal()`/`kill()` bajo el BKL

Todos hacen `im = disable(); … resched(); restore(im);`. Con el esquema:

- `disable()` anida, toma BKL (transición 0→1).
- `resched()` suelta BKL en el cambio (nest 0 en `struct percpu`).
- Al reanudarse, readquiere, `restore()` lleva nest a 0 y suelta.

Resultado: **los 82 ficheros siguen siendo correctos sin editarse**, porque sus
secciones críticas pasan a ser exclusión mutua entre cores. Lo único que se
auditar por si no usaran `disable()` son las rutas de espera bloqueantes que
corren por el ISR y las no bloqueantes sin lock (ver §7).

### 4.3 Reglas duras del BKL (para que no haya deadlock)

1. **Nunca esperar al otro core con el BKL tomado.** En particular: con nest>0
   no se hace `signal()` hacia un hilo en el otro core y se espera su ack, ni
   `kill()` remoto síncrono, ni `sdmmcBusLock()` cruzada.
2. Un ISR puede tomar el BKL (anida) pero nunca bloquear; si un ISR necesita
   wakar a otro core, escribe un flag + IPI y sale.
3. El BKL se suelta solo en `ctxsw()`. Ningún `restore()` lo suelta en otra
   parte.
4. `kprintf()`/`printf()` deben serializarse con su propio spinlock o con el
   BKL; nunca escribir UART desde los dos cores a la vez (hoy la consola no
   tiene candado: hay que ponerlo, fase 3).
5. Los candados de driver por dispositivo van **fuera** del BKL cuando hay
   espera (p.ej. el monitor `lvgl_lock()`), o se anidan dentro con la regla 1.

---

## 5. Diseño de planificador por core

### 5.1 Estado por core (`struct percpu`)

```c
struct percpu {
    tid_typ  current;          /* thrcurrent de ESTE core            */
    qid_typ  readylist;        /* cola de listos de ESTE core        */
    int      nest;             /* anidamiento disable() de ESTE core */
    int      resdefer;         /* resdefer de ESTE core              */
    int      idle;             /* 1 si ESTE core está en wfi          */
    ulong    core_hz;          /* reloj (MEZFC no, para diagnóstico)   */
};
static struct percpu percpu[2] __attribute__((aligned(64)));
```

`thiscpu()` = `csrr mscratch` (el P4 no usa `mscratch` en este kernel:
verificado, no hay ninguna referencia). Cada core lo pone en
`nulluser()`/`core1_main()` a su `percpu[hart]`.

### 5.2 Readylist por core (afinidad) o global

| | global + migración | por core + afinidad |
|---|---|---|
| código en `ready()`/`resched()` | + robo, + invalidación L1-I | ~30 líneas nuevas |
| I-cache | hay que `CACHE.sync_map` en el destino | no hace falta |
| `thrcurrent` | `thiscpu()->current` | igual |
| balanceo | automático | manual (elegir home en `create()`) |
| apps ELF | hay que elegir core al lanzar | se elige por flag en `create()` |
| depuración (`ps`) | hay que pintar el core actual | idem |

**Recomendado: por core + afinidad.** Es más pequeño, elimina el problema de
I-cache, y cubre el objetivo real ("tareas en ambos núcleos"). El campo
`struct thrent::cpu_home` (u8) decide a qué cola vuelve. `ready()` inserta en
`percpu[home].readylist` y, si `home` ≠ este core e ID está en wfi, le manda un
IPI. `chprio()` igual. `dequeue()` del propio core.

Con dos `nulluser`/`idle` (tid 0 en core0, tid 1 en core1, prio 0, siempre
ready) la readylist nunca se vacía: no hay que crear thread idle aparte ni
gestionar lista vacía. `NULLTHREAD` sigue siendo tid 0; tid 1 pasa a ser
`NULLTHREAD1` protegido igual que 0 (no se puede `kill()`, nunca sale de la
cola).

### 5.3 Quién corre en qué core

- core0: `nulluser` → `main` → shell, launcher/LVGL, `netboot`.
- core1: un `idle` + lo que se cree con `create_on(core1, …)` o los apps que
  se lancen con `elf run --cpu1`/`xsh smp lanzador`.
- El campo `home` se hereda del creador por defecto (hoy `create()` hereda ese
  comportamiento), o se fuerza con el flag.

Así la consola, el panel y LVGL siguen en core0 con el código probado, y core1
se usa para lo caro y aislado (decode MP3, red, telnet, compilación/armado de
minimax, backlight/audio).

### 5.4 `cpuid` y `thrcurrent`

`cpuid` global desaparece (o pasa a diagnóstico por core). `thrcurrent` se
convierte en macro `(percpu[hart].current)`; los 32 usos en el árbol compilan
sin cambiarse. `struct thrent` gana `cpu_home` y `cpu_last` (para `ps`).

---

## 6. Ficheros que cambian, estimados

### 6.1 Nuevos (código propio, estilo del árbol)

| fichero | qué | ~líneas |
|---|---|---|
| `include/percpu.h` | `struct percpu`, `thiscpu()`, `cpu_id()` | 60 |
| `include/spinlock.h` | `kernel_lock`, API `spin_acquire/release` (inline, `amoadd.w.aq`) | 80 |
| `system/platforms/esp32p4/smpcpu.c` | `percpu[]`, `thiscpu()`, IPI (`ipi_send(core)`), `core1_main()` | 180 |
| `system/platforms/esp32p4/smpit.c` | clkhandler por core, `enable_irq_on(core, line)`, reasignado de SYSTIMER si hace falta | 150 |
| `shell/xsh_smp.c` | comando de diagnóstico: cores, `wfi`, IPI, ticks, home de hilos | 120 |

### 6.2 Reescritos

| fichero | cambio | ~líneas |
|---|---|---|
| `system/arch/riscv/intutils.S` | `disable/restore` con nest por core + BKL; `thiscpu()` opcional | 60 |
| `system/resched.c` | cola por core, liberación/readquisición de BKL en `ctxsw()` | 150 |
| `system/clkinit.c` | `clkinit()` solo en core0; `clkupdate(cycles)` por core | 40 |
| `system/clkhandler.c` | `clkhandler()` despacha por hart: core0 → bookkeeping + sleepq + wakeup; ambos → cpu_ticks + resched | 80 |
| `system/platforms/esp32p4/riscvclk.c` | `P4_MTIMECMP(hart)` (0x20004000+8·hart) y `clkupdate_hart()` | 40 |
| `loader/platforms/esp32p4/start.S` | `park`: `mtvec`, `mtvt` inerte, `mstatus.FS=11`, `MIE=0`, `sp` de parking, **salto a `core1_main()`** (ya no wfi) | +40 |
| `compile/platforms/esp32p4/ld.script` | `.core1stk` grande (8 KB) + `.core1bss` NOLOAD, `_core1stk_end` | +15 |
| `system/platforms/esp32p4/clic.c` | `clic_init_local()` por hart (threshold, attr, ie), `clic_route(core, line, src)`; tabla inerte de 48 en flash | 80 |

### 6.3 Tocados

| fichero | cambio | ~líneas |
|---|---|---|
| `system/initialize.c` | `percpu[core1]` con `idle1`, `readylist`→`percpu[].readylist`, `thrcurrent` retirado, nulluser core0 listo | 80 |
| `system/ready.c` | inserta en la cola del `home`; notifica si está en wfi | 30 |
| `system/create.c` | campo `home` (flag en `create_on`); init `klock_depth/cpu_home/cpu_last` | 25 |
| `system/chprio.c` | reinserta en la cola del home | 10 |
| `system/kill.c` | `kill()` remoto: sólo `THRREADY/THRSLEEP/THRWAIT` (sacar de su cola), nunca `THRCURR` de otro core (SYSERR); la víctima en core1 se apila y la recoge el otro core | 40 |
| `system/sleep.c`, `wait/signal/recvtime/unsleep/send/receive/recvclr` | sin cambios de lógica; pasan a ser globales por BKL | 0 |
| `include/thread.h` | `struct thrent` + `klock_depth`, `cpu_home`, `cpu_last`; `THRENTSIZE`/padding | 15 |
| `shell/xsh_ps.c` | columna `cpu`/`home` con `cpu_last` y `cpu_home` | 15 |
| `system/exports.c` | `thiscpu()`, `cpu_id()`, `ipi_send()`, `cpu_home` en `create_on` | 10 |
| `lib/libxc/printf.c`, `lib` | `putc` serializado con spinlock de consola (o con BKL) | 20 |
| `system/devices`, `tty` | un spinlock de consola; nada más | 30 |

### 6.4 Qué NO se toca

`system/Semaphores/monitors/mutexes/mqueue/mailbox/queue/msg`, `memget`,
`psrammem`, `bfpalloc`, `stack`, `sem*`, `insert/dequeue/getitem/insertd`,
`sleep` (la cola de sleepers se decrementa en core0), `LVGL`, apps ELF,
`FAT`, `FatFS`, `net`, `espeth`, `sdmmc`, los 82 ficheros con `disable()` (los
absorbe el BKL), el SBL (`machine/sbl`), `main.c` (el arranque no se cambia),
ni el ULP.

**Total estimado: ~1.100 líneas nuevas/cambiadas en ~25 ficheros**, de las
cuales ~400 son el núcleo y el resto audición + drivers.

---

## 7. Drivers y puntos de verdad

### 7.1 Consola (`tty`/`uart-esp32p4`/`kprintf`)

Hoy `kprintf` escribe registro a registro sin lock. Con dos cores hay que:

- spinlock `console_lock` en `ttyPutc`/`uartPutc` (no dormir nunca: es un poll
  breve sobre registros; con el BKL bastaría, pero el ISR podría anidar); o
- reservar la consola al core0 y redirigir `printf` de core1 a un anillo SPSC
  que core0 drena. Más barato el spinlock.

### 7.2 SDMMC / microSD / FAT

`sdmmcBusLock()` ya es un candado compartido (lo usan FAT y el C6). Con SMP:
cualquier core puede esperarlo, pero **nunca se toma con el BKL**: es un
semáforo con espera (el hilo puede salir del CPU). Se usa como hoy.

### 7.3 `kill()` de un hilo en ejecución en el otro core → IPI + barrera

Política simple y segura (fase 4):

- `kill(tid)`: si el hilo está `THRREADY` en su cola → sacar de la cola (con
  BKL). `THRSLEEP/THRTMOUT/THRWAIT` → `unsleep`/++sem igual que hoy. `THRCURR`
  y es de este core → suicidio (como hoy). `THRCURR` y de otro core →
  **SYSERR + mensaje** ("mata el hilo desde su core o usa `suspend`").
- `suspend(tid)` remoto: setflag + IPI; el otro core lo ve en su `disable()`
  o en un punto seguro, marca el hilo `THRSUSP` y reschedulea. Sin IPI, xsh
  responde "no respondiendo en core1".
- `appmgr_panic`/`elf_exit()`: `kill(gettid())` siempre es local, sin cambios.

### 7.4 LVGL y LCD

`lvgl_lock()` = monitor (`lock()`/`unlock()`, `system/lock.c`): espera por
semáforo con BKL liberado → correcto entre cores. `lvglui` se queda en core0.
Las apps en core1 llaman a LVGL bajo el monitor. El flush (`lcdControl`
`LCD_CTRL_BLIT`) se ejecuta en core0 (el que hace `lvgl_lock`), así el SPI2
sigue teniendo un único dueño. Este es el camino más barato y ya concuerda
con "tareas en ambos núcleos".

### 7.5 Audio/I2S/DMA

El DMA ya no pasa por caché; los anillos viven en PSRAM por la ventana `0x88…`.
No cambia nada con SMP. Lo que conviene: **fijar el hilo consumidor PCM/MP3
en core1** (o en el que lo lance) para que el core0 mantenga la GUI.

### 7.6 I2C / SDA/SCL

Hay un solo bus y varios masters (audio, power, touch, EEPROM). Hoy se
serializa dentro de `i2cControl` con `disable()`. Con SMP hay que asegurar
que la transacción completa es atómica entre cores: un spinlock por bus
dentro de `i2cControl` (y que el UART/I2C ISR, si lo hubiera, anide).

---

## 8. Fase 0: lo único que puede tumbar el diseño (probamos ANTES de escribir nada)

Nada de esto va al kernel todavía; es un comando `xsh smp probe` que escribe y
lee registros/ventanas y devuelve hex. Cada prueba se hace con core1 en
`park` (sin `start.S` modificado) o con un segundo hilo mínimo.

| # | Prueba | Éxito | Si falla |
|---|---|---|---|
| 1 | L1-D compartida: escribir `0x5A5A0000\|cpu` en L2MEM y PSRAM en core0, leer en core1 (mismo puntero); repetir 1000 veces | siempre coincide | pasar el buzón/estado caliente a una ventana sin caché o añadir `CACHE.sync_map` D en la frontera; si ni así funciona, SMP-as-designed no vale y se queda en coprocesador |
| 2 | `mtimecmp` por hart: programar `0x20004008` con `mtime + 10^6`, con core1 con MIE=0, leer `mtime` en core0 antes/después; luego core1 con MIE=1 y core1 cuenta ticks a 1 kHz | ambos cores reciben mcause 7 en su tick | remapear SYSTIMER_TARGET1 (fuente ETS 54 → línea 18) solo para core1 |
| 3 | IPI: escribir `1` en `0x20000004` (msip hart1) con core1 en `park` (MIE=0): no debe pasar nada; con MIE=1 debe entrar en el handler de línea 3 | mcause 3, línea 3, `mtvec` válido, sin reset | usar el alias CLIC `0x20810000` línea 19 para un IPI propio |
| 4 | CLIC core1: programar `mintthresh=0`, `attr=SHV`, `ie=1` en la ventana `0x20810000` línea 3 y ver que la línea se dispara en core1 y no en core0 | core1 entra en handler, core0 no | si no, core1 solo con polling y el tick solo en core0 (SMP degradado pero usable) |
| 5 | I-cache: correr código en PSRAM en core1 recién escrito por core0 | ejecuta sin `illegal instruction` | hacer `CACHE.sync_map = L1I0\|L1I1` en cada frontera o pinning + invalidar una vez al cargar |

Después de la Fase 0 ya se sabrá si el diseño es como está escrito o si hay
que degradar (sin tick en core1 → sin preemptividad, solo cooperativo).

---

## 9. Fases posteriores (con criterios de aceptación)

### Fase 1 — Dos cores, un hilo cada uno, BKL, timer por core

- `.core1stk` 8 KB, `core1_main()`, `mtvt` inerte, `thiscpu()`, `kernel_lock`.
- `nulluser` core0; `idle1` core1; `readylist` por core; `create_on()`.
- Regresión: `ps` lista ambos hilos con su core; `while(1) for(;;);` en un
  hilo de prueba **no** congela la consola ni el panel; dos hilos a prio 20
  en cores distintos avanzan a la vez.
- Flasheo: `make` + `make flash` (autorizado ya por el dueño para esta fase).
- **Criterio de salida**: un hilo en core1 calcula 5 s mientras core0 responde
  `ps`/`audio` y repinta LVGL sin perder ticks.

### Fase 2 — Semántica completa de hebras

- `sleep()`, semáforos, monitores, mutex, colas, mensajes, mailboxes, mqueue:
  core1 puede bloquearse/despertarse igual que core0.
- `cpu_ticks` por core en `ps`.
- Regla 1 estricta: ningún sitio espera al otro core con nest>0.
- **Criterio de salida**: productor-consumidor cruzado (hilo core0 ↔ hilo
  core1) durante 60 s sin pérdida de mensajes ni caída; `sleep(500)` en core1
  despierta con <5 ms de error.

### Fase 3 — Drivers y consola

- `console_lock`, `i2c_bus_lock`, verificación `sdmmcBusLock`/FAT desde ambos
  cores; `lvgl_lock` verificado con una app en core1 repintando.
- **Criterio de salida**: `audio play <archivo>` corriendo en core1 (hebra
  `apprun` creada en core1) mientras se usa el launcher y la shell en core0;
  1 h sin bloqueo; `ps` estable.

### Fase 4 — `kill`/suspend remoto, diagnóstico

- IPI desde `ready()`, `kill()`, `suspend()`; `xsh smp` (cores, hilos, IPI,
  wfi, ticks, home); `xsh ps` con columna core.
- **Criterio de salida**: `kill <tid de hilo core1>` desde la shell elimina el
  hilo sin pánico ni corrupción; `sleep 10` en core1 cancelado desde core0.

### Fase 5 — Rendimiento (opcional)

- Partir primero `psram_getmem()`/`memget()` en dos listas libres que se
  reparten por core (si el perfil lo pide), no antes: hoy no hay contención
  medida.

---

## 10. Riesgos y no-objetivos

| Riesgo | Mitigación |
|---|---|
| D-cache no compartida en silicio | Fase 0, prueba 1. Si falla, SMP se degrada a coprocesador |
| Deadlock por esperar cruzado con BKL | Regla 1 + revisión de `signal`/`kill`/`suspend` |
| Consola corrupta | `console_lock`, fase 3 |
| `kprintf` desde ISR en core1 | nunca; los ISR solo ponen flags/IPI |
| Kernel roto por `restore()` mal emparejado | `klock_depth` por hilo + aserción en `resched` (nest==0 al salir) |
| Apps ELF con estáticas (ya prohibidas) | sin cambios; SMP no las rompe ni las arregla |
| FAT en dos hilos a la vez | `fslock` ya existe; con BKL queda correcto |
| `panic()` en core1 | `panic` pasa por `panicstk` global → añadir `panic_on(core)` o parar solo su core |

**No-objetivos explícitos**: SMP en SBL/ULP, SMP para el arranque en `main.c`,
migración automática de hilos, RT throttling, aislamiento de fallos
(`panic` en core1 parando core0), RAMAC para `uart` DMA, y cambiar la ABI de
apps.

---

## 11. Decisión que necesito del dueño

1. **¿Afinidad por núcleo (recomendado) o cola global con migración?**
   La afinidad es más simple, protege la L1-I y es suficiente para "tareas en
   ambos núcleos". Migración global queda para después.
2. **¿Qué tarea arranca en core1 por defecto?** Hoy lo natural:
   `netboot`/red + apps lanzadas con `xsh smp run`; la shell y LVGL se quedan
   en core0.
3. **¿Autorizas la Fase 0 (requieren placa y un comando temporal `xsh smp
   probe`) y después la Fase 1?** La Fase 0 es ~300 líneas y no toca el
   planificador; la Fase 1 es el cambio de kernel real (~800-1.100 líneas).
4. **¿`kill` remoto devuelve error o espera a que el otro core termine el
   hilo?** Recomiendo error inmediato (el hilo sigue vivo en su core hasta que
   haga `exit()`, `suspend()` o lo recoja un punto seguro con IPI); si
   prefieres esperar, es la variante con IPI + spin (más simple de usar, peor
   de parar y de razonar).

Sin el OK de 3 no toco `resched()` ni `start.S`. Con el OK, este documento pasa
a ser el checklist de implementación.
