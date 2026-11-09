# 01 · Memoria, arranque y los tres núcleos

Cifras sacadas del enlazado actual (`riscv64-unknown-elf-size -A compile/xinu.elf`,
`nm compile/xinu.elf`, `ls -l compile/xinu.boot`) el 2026-11-04. No redondeadas.

---

## 1. Cadena de arranque, en orden real

```
BootROM (rom)  ──lee cabecera──▶  0x2000 flash: SBL  ──copia a HP_TCM──▶  0x30100000
                                      │
                                      ├─ psram_init()  (libpsram.a)     → habilita 0x48000000
                                      ├─ mmu.c                          → marca flash y PSRAM
                                      ├─ arranca HP core0 y HP core1 ───▶ 0x40005000 (XIP kernel)
                                      └─ ulp_boot()  → ULP_ENTRY_POINT_REG (0x50110028)
                                                          │
                                                          ▼
                                                    LP core, XIP en 0x40100000
```

1. El BootROM lee la cabecera de imagen ESP en el offset **0x2000** y carga el SBL en
   **HP_TCM `0x30100000`** (RAM de 8 KB dentro del propio núcleo; se copia fuera de
   flash antes de ejecutar, por eso el SBL puede reprogramar flash sin pisarse).
   Un binario crudo sin cabecera se rechaza con `invalid magic number`
   ([machine/Makefile:76-83](../../machine/Makefile)).
2. El SBL inicializa PSRAM y MMU y suelta los dos núcleos HP. Los **dos harts caen en el
   mismo sitio**: `__kernel_entry_point = 0x40005000`, que es `_start` del kernel
   ([machine/sbl/boot.S:64-66](../../machine/sbl/boot.S)).
3. `reset_handler` lee `mhartid` y aparca al que no es 0
   ([loader/platforms/esp32p4/start.S:92](../../loader/platforms/esp32p4/start.S#L92)).
4. El core1 se queda en `wfi` con pila, `mtvec` y testigo propios
   (`.core1stk`, 2 KB en L2MEM). Verificado en placa con
   `memdump -C 0x4ff3f5d0 16` → `34 12 de c0 01 00 00 00 …` (magic `0xC0DE1234`, hart 1).
   **La dirección sale de `nm compile/xinu.elf | grep _core1stk`**; con el build de hoy
   es `0x4FF3F5D0`.
5. `ulp_boot()` ([machine/sbl/ulp_boot.c:29-45](../../machine/sbl/ulp_boot.c)) escribe la
   entrada del LP core en `ULP_ENTRY_POINT_REG = 0x50110028`, habilita
   `LP_CPU_SLP_STALL_EN`/`LP_CPU_SLP_RESET_EN`, pide sleep y levanta por software.

El kernel se ejecuta **XIP** (execute-in-place) desde flash: no hay copia de `.text` a
RAM. Eso es lo que limita qué se puede reprogramar en caliente (ver §5 y
[05-roadmap-y-conf.md](05-roadmap-y-conf.md)).

---

## 2. Mapa de memoria actual

```
0x30100000 ┌────────────────────────┐ 8 KB   HP_TCM   ── SBL (copiado aquí por el BootROM)
0x30102000 └────────────────────────┘
   …
0x3FF10000       CACHE bloques                L1I/L1D + `CACHE_L2_CACHE_CACHESIZE_CONF`
   …
0x40000000 ╔════════════════════════╗ ─┐ FLASH XIP (16 MB físicos, `--flash-size 16MB`)
0x40002000 ║  SBL     (4.304 B)     ║  │ termina 0x400030D0
0x400030D0 ║  · · ·  hueco 8 KB · · ·║  │
0x40005000 ║  KERNEL  (291.372 B)   ║  │ .init 186 · .text 214.576 · .rodata 76.092 · .ctors 4
0x4004C22C ║  fin imagen            ║  │  ← aquí termina hoy lo que hay en flash
           ║                        ║  │
0x4004C22C ║      LIBRE  ~720 KB    ║  │  ← hueco utilizable (stage OTA, apps .class, html)
0x40100000 ║  ULP     (724 B)       ║  │  ← 0x40100000 == offset de flash 0x100000
0x401002D4 ║      LIBRE  ~14,99 MB  ║ ─┘  (no hay tabla de particiones: nada protege este hueco)
0x41000000 ╚════════════════════════╝
   …
0x48000000 ┌────────────────────────┐ PSRAM 32 MB, base CON caché (window cached)
0x48000000 │ .audioshare   151.552 B│ anillos PCM TX/RX (NOLOAD)
0x48025000 │ .sdshare        8.192 B│ página de bloqueo del FAT
0x48026000 │ _sd_block_page         │
0x48027000 │ heap PSRAM  33.363.968 │  ← `psram_malloc()` reparte aquí (33.361.920 libres medidos)
0x49FF8000 │ RAM del núcleo LP (32K)│  ← `ulp.ld:49` (`ORIGIN = 0x49FF8000, LENGTH = 32K`)
0x4FFFFFFF └────────────────────────┘
0x50108000 ┌────────────────────────┐ 32 KB LP_SRAM — declarada en ld.script:35, SIN usar
0x50110000   LP_SYS  (0x50110028 = ULP_ENTRY_POINT_REG)
0x4FF00000 ┌────────────────────────┐ L2MEM 768 KB, RAM de verdad del HP
0x4FF00000 │ .l2reserve   131.072 B │ ── BANDERA FANTASMA: no hay celdas aquí (caché L2)
0x4FF20000 │ .vectors / .data   492 │
0x4FF20200 │ .bss          127.952 B│
0x4FF3F5D0 │ .core1stk       2.048 B│
0x4FF3FDD0 │ heap L2MEM    459.312 B│ ← `getmem()`/pilas de hebra
0x4FFB0000 │ ROM de arranque  64 KB │ tabla de despacho de caché en 0x4FFBFFD8, pila ROM en 0x4FFBCFC0
0x4FFC0000 └────────────────────────┘
```

Orígenes declarados: [compile/platforms/esp32p4/ld.script:28-35](../../compile/platforms/esp32p4/ld.script#L28-L35).
Offset de flasheo: [compile/platforms/esp32p4/platformVars:163-171](../../compile/platforms/esp32p4/platformVars#L163-L171).

### Dónde se solapan las cosas (lo que pregunta el punto 20)

- **No hay tabla de particiones.** Escribes con `write-flash` a tres direcciones
  concretas y nada impide que una imagen crezca sobre la vecina. Los topes reales hoy:
  - el kernel puede crecer hasta `0x100000 - 0x5000 = 1.023.984 B` antes de pisar el ULP;
  - el ULP tiene 1 MB de hueco hasta el final del primer megabyte de flash;
  - el hueco libre contiguo más grande es **~14,99 MB** detrás del ULP.
- El ULP se enlaza en `0x40100000` (XIP) porque **el offset de flash es 0x100000**: la
  ventana XIP empieza en `0x40000000`, así que `0x40000000 + 0x100000 = 0x40100000`. En
  el `ulp.ld` original de Chalandi era 0x8000, y con este kernel de ~291 KB quedaría
  pisado — por eso se movió ([machine/ulp/ulp.ld:16-20](../../machine/ulp/ulp.ld#L17-L23)).
- Los `ORIGIN/LENGTH` de `ld.script` **no son físicos**: `FLASH LENGTH = 31M` y
  `RAM LENGTH = 31M` mientras el chip son 16 MB de flash y 32 MB de PSRAM. El enlace no
  avisará si el kernel pasa de 16 MB. Propuesta en
  [05-roadmap-y-conf.md](05-roadmap-y-conf.md#recursos): bajar `FLASH` a 16M para que el
  `ld` corte; `RAM` ya cuadra porque el `LENGTH=31M` reserva exactamente el MB del ULP.

### ¿De dónde saca 1 MB de RAM el ULP? (pregunta literal del punto 20)

De la **PSRAM**: `0x49F00000 = 0x48000000 + 0x1F00000`, el último megabyte de los 32. Lo
inicializa el SBL (`psram_init()` + MMU) **antes** de soltar el LP core, y lo deja fuera
del heap del kernel por un detalle del enlazado: `_psram_free_end = ORIGIN(RAM)+LENGTH(RAM)`
con `LENGTH(RAM)=31M` → el heap de PSRAM termina **justo** en `0x49F00000`.
Medido entonces: `_psram_free_end = 0x49F00000` (símbolo `B` en `nm`; ver la actualización
más abajo). No es una RAM aparte del
chip; es PSRAM compartida con una frontera acordada entre dos scripts de enlace. Si
algún día alguien sube `LENGTH(RAM)` a 32M, el heap del kernel se come la RAM del ULP en
silencio.

**Actualizado el 2026-11-04 (dos pasos):** el ULP ya no usa el megabyte. Primero
`ulp.ld:48-49` se recortó a `FLASH LENGTH = 64K` / `RAM LENGTH = 32K`; después el `ORIGIN`
se movió al final exacto del PSRAM, `0x49FF8000`, y `RAM LENGTH` del kernel subió a
`32M-32K` ([ld.script:33](../../compile/platforms/esp32p4/ld.script)), así que
`_psram_free_end = ORIGIN(RAM)+LENGTH(RAM)` vale ahora **`0x49FF8000`** (medido en
`nm -n xinu.elf`) y las dos reservas teselan los 32 MB sin solape. Ganancia medida en
placa: heap PSRAM de 32.346.112 B a **33.361.920 B** = **+992 KB** (`syscheck mem`, span
`0x48027000..0x49ff8000`; el autotest de arranque pasó de `31588 KiB free` a `32580 KiB
free`). El contrato entre los dos ficheros sigue vigente, solo cambió la cota: si alguien
pone `LENGTH(RAM)` en `32M`, el heap del kernel se come la pila del LP. La alternativa que
rompe el contrato —mover el LP a `LP_SRAM`— está en
[07-flash-map-diseño.md §3.1](07-flash-map-diseño.md).

---

## 3. ¿Cuánta RAM disponible "debería" contar? (respuesta corta)

| zona | cuentas | uso |
|---|---|---|
| **L2MEM útil** | **576 KB** de celdas reales | 768 KB declarados − 128 KB fantasma − 64 KB ROM |
| · ya ocupado | ~130,5 KB | `.data` 492 + `.bss` 127.952 + `.core1stk` 2.048 (`.vectors` son 0 B de tamaño, la tabla real está en `.l2reserve`-después) |
| · **heap L2MEM** | **459.312 B ≈ 448 KB** | de `0x4FF3FDD0` a `0x4FFB0000`; aquí van las pilas de hebra |
| **PSRAM** | **31,8 MB** | 32 MB − 1 MB ULP − ~156 KB de anillos compartidos |
| · **heap PSRAM** | **33.329.152 B de span** (`0x48027000`…`0x49F00000`) | lo que devuelve `psram_freemem()` es algo menos, por las cabeceras `memblock` |
| **HP_TCM** | 8 KB | ocupada por el SBL; nada del kernel debería ir ahí |
| **LP_SRAM** | 32 KB | **libre y sin usar**; es la única RAM que sigue viva con el núcleo HP dormido |

Reglas prácticas que se desprenden:

1. **Presupuesta ~400 KB de RAM rápida** (L2MEM) y **~31 MB de RAM lenta con caché**
   (PSRAM). Todo lo que sea pila de hebra, tablas del kernel y buffers de 4-8 KB → L2MEM.
   Todo lo que sea PCM, imágenes, WAV, catálogo de apps → PSRAM con `psram_malloc()`.
2. La banda `0x4FF00000…0x4FF1FFFF` **no existe**. `nm` la muestra y el enlazado la
   reserva, pero escribir ahí y leer después da basura en cuanto la línea L2 se desaloja.
   Ese fue el root-cause del "phantom 128 KB": ahora `.data`/`.bss`/heap empiezan en
   `0x4FF20000` y el reservado son 128 KB enteros
   ([ld.script:171-184](../../compile/platforms/esp32p4/ld.script#L171-L184)).
3. La caché L2 **no es write-back transparente para el DMA**: si un buffer lo consume el
   AHB-DMA (audio) o el SDHOST (SDIO), o lo pones en una sección `.NOLOAD` de PSRAM y lo
   escribes por el alias sin caché, o limpias la línea antes de arrancar el DMA. Ver §4.

---

## 4. PSRAM y la caché, de una vez por todas (punto 9)

**La respuesta a "se le suma 0x400… y falla con memcpy".**

- `0x48000000` = PSRAM **con** caché (L1 + L2). Es la base que usa el MMU y el enlazado.
- `0x88000000` = **el mismo array** visto sin caché. El truco es sumar `0x40000000`, que
  es el offset del alias (`PSRAM_CPU_UNCACHED`, `device/psram/psramInit.c:36`).
- El número `0x40000000` **no es una dirección**: también es el principio de la ventana
  XIP de flash (`0x40000000`), y por eso la frase "sumarle 0x400…" suena a que te sales
  del mapa. No te sales: `0x48… + 0x40… = 0x88…` sigue dentro del espacio del CPU y es
  la ruta desacachada de la misma celda.
- **No es un problema de permisos.** El MMU externo del P4 solo tiene bits de validez y
  de tipo flash/PSRAM, sin R/X (`machine/sbl/mmu.c:28`), y este kernel no configura PMP.
  Un puntero de `psram_malloc()` es saltable y ejecutable.

Medido en placa (2026-11-02, `examples/psram-blob/README.md` §1-2), y es lo que explica
los fallos con `memcpy`:

| camino | escribir | ejecutar | resultado |
|---|---|---|---|
| caché → caché | `0x48…` | `0x48…` | **falla**: `mepc=0x48027008 mcause=2 mtval=0x20aa` (mitad baja de la celda vieja) |
| sin caché → caché | `0x88…` | `0x48…` | **falla**: la línea sucia de L2 tapa lo escrito |
| sin caché → sin caché | `0x88…` | `0x88…` | **funciona**: `sum(2,3)=5`, `sum(40,2)=42` |

Regla operativa para este port:

1. **Un buffer, una ventana.** Si el DMA o un blob recién copiado entran por `0x88…`,
   todo el mundo entra por `0x88…`. Mezclar ventanas dentro del mismo bloque es lo que
   corrompe.
2. `memcpy` **no es el culpable**; `memcpy` por `0x48…` deja la línea sucia en L1-D y el
   fetch de instrucción la lee rancia. La ruta "correcta" con caché exige la operación
   explícita: `CACHE.sync_map` (bit 0 = L1-ICache0, bit 4 = L1-D, bit 5 = L2),
   `CACHE.sync_addr` (dirección **virtual**), `CACHE.sync_size`, `CACHE.sync_ctrl` y luego
   `fence.i`. **Ojo**: los envoltorios de `include/hal/cache_ll.h` no enlazan en este
   árbol porque delegan en símbolos de ROM (`Cache_Invalidate_Addr`,
   `Cache_WriteBack_Addr`) cuya tabla no está portada. Línea de caché ≥ 64 B
   (`MIN_L1_ICACHE_LINE_SIZE`, `include/esp32p4/rom/cache.h:31`).
3. Ventana de dirección **válida para DMA AHB**: `0x40000000 … 0x4FFC0000`. Un puntero
   `0x88…` **no** lo accepts el DMA: usa la base `0x48…` para el descriptor y la ventana
   sin caché solo para que la CPU limpie/pise. Es exactamente lo que hace audio: la CPU
   escribe los anillos por `0x88…`, el descriptor del DMA apunta a `0x48…`
   ([02-dispositivos.md](02-dispositivos.md#audio)).
4. El alias `0x8F…` de L2MEM **no** existe; L2MEM no tiene ventana desacachada. No
   intentes "hacer lo mismo" con la RAM rápida.
5. `psram_malloc()` devolvía `NULL` porque la lista libre se construyó sobre la banda
   fantasma de L2MEM en una versión anterior; hoy la lista vive en PSRAM real
   (`system/psrammem.c:57`) y `psram_freemem()` responde.

---

## 5. Los tres núcleos (punto 21)

### 5.1 Core1 (HP secundario)

Estado: **aparcado con orden**, no ejecuta nada. El diseño completo está en
[README_CORE1.md](../../README_CORE1.md) (secciones 4-7); aquí va el resumen operativo y
las tres trampas que hay que conocer antes de intentarlo.

Lo que **sí** está verificado: hart 1 despierta, se pone su pila (`.core1stk`, 2 KB),
apaga `mstatus.MIE`, pone su `mtvec` y entra en `wfi`, dejando el testigo
`{0xC0DE1234, 1, sp}`.

Lo que decide si merece la pena:

| recurso | alcance | consecuencia |
|---|---|---|
| `mtvec`, `mtvt`, `mstatus`, `mepc`, `mie` | **por hart** | core0 no puede configurar las trampas de core1; cada hart programa las suyas al entrar en C |
| CLIC | por hart **con alias** | `P4_CLIC_BASE = 0x20800000` es el CLIC del hart que ejecuta; `+0x10000` (`0x20810000`) es el del otro (`DUALCORE_CLIC_CTRL_OFF`). `clic.c` solo usa la ventana local |
| matriz ETS→línea | por hart | `INTERRUPT_CORE0_BASE 0x500D6000`, `INTERRUPT_CORE1_BASE 0x500D6800` (`include/esp32p4.h:73-74`); `enable_irq()` escribe solo en la de core0 |
| `mtime`/`mtimecmp` | compartido | un solo consumidor: Xinu. Core1 no puede usar `clkcount()` para sus ticks |
| periféricos | compartidos | dos harts sobre el mismo `uarttab`/buffer = corrupción. **Un periférico por hart** sí es seguro |
| L2MEM/PSRAM | compartidas, **caché por core** | hay que `fence` en ambas direcciones; los datos que se pasen deben vivir en la ventana sin caché o respaldarse |

Modelo realista y acotado (el que propone README_CORE1 §4): core1 ejecuta **una única
función sin reentrar en el runtime Xinu** — solo MMIO y memoria compartida — encargada
por core0 con un `struct core1_block {magic, hartid, stack_top, entry, arg, req_seq,
ack_seq, done_seq}` en LP_SRAM o PSRAM, con `core1_fence()`. Nada de `printf`, nada de
`sleep`, nada de `getbuf`.

Qué tarea darle, en orden de utilidad para este proyecto:
1. **`esp_hosted_poll()` + SDIO del C6**: es un bucle de sondeo puro, sin llamadas al
   runtime, y es exactamente lo que hoy roba ciclos del core0 y de la shell. El candado
   del bus (`sdmmcBusLock()`) tendría que pasar a un candado compartido entre harts, o el
   bloque SDHOST asignado en exclusiva a un hart.
2. **PCM → I2S** (mezcla/reproductor): el anillo ya vive en PSRAM; un hart dedicado que
   consuma muestras deja libre al core0 para la GUI.
3. **Un segundo `netRecv`** — descartado: `netRecv` llama a la pila Xinu entera
   (`netGetbuf`, `ipInput`, `tcpNext`, `resched`), no es código aislable.

Lo que **no** puedes hacer hoy sin tocar `start.S`, `clic.c` y `ctxsw.S`: ejecutar hebras
de Xinu en core1. El planificador es de un solo procesador (`prqhead[]` único, sin lock)
y `create()`/`ready()` asumen que quien los llama es el hart que ejecuta hebras.

### 5.2 ULP (core LP-RISC-V)

RV32IMC, **sin FPU**, XIP en flash, `.data/.bss/.stack` en el último MB de PSRAM, y
**printf resuelto a la ROM en `0x4FC00024`** por el propio `ulp.ld`
([machine/ulp/ulp.ld:14 de main.c](../../machine/ulp/main.c)).

Hoy: `ulp.bin` son **724 B**, se flashea en `0x100000`, arranca, y su `main()` está en
`while(1)` con el `printf` comentado. Es un andamio funcionando, no una tarea.

Para qué sirve de verdad en este proyecto:
- **botón/encendido en reposo**: `PWR_KEY` (GPIO49) está definido en `include/board.h`
  pero nunca lo lee nadie; el PMIC se sondea desde el panel. El LP core + LP_SRAM son los
  únicos que siguen vivos con el HP dormido — es el sitio natural para un vigía de tecla
  y para el wake por carga del AXP2101.
- **sondeo lento de batería/temperatura** por I2C LP, escribiendo el resultado en LP_SRAM
  para que core0 lo lea sin despertarse.
- **no** para audio/red/ELF: sin FPU, sin runtime Xinu, 1 MB de RAM y XIP por flash
  (cada salto de instrucción es un acceso a SPI).

Comunicación con HP: hoy no hay canal. Lo mínimo es un área fija de LP_SRAM (los 32 KB
declarados y sin usar en `ld.script:35`) + un bit de `LP_SYS` como interrupción. Hay que
añadir `LP_SRAM` al enlazado del kernel (`ld.script` no le pone ninguna sección) para
que core0 tenga dónde mirar.

### 5.3 SBL y HP_TCM

HP_TCM (`0x30100000`, 8 KB) es la RAM interna del propio núcleo; el SBL vive ahí porque
**necesita poder reescribir flash sin ejecutarse desde flash**. Ese es el mismo truco que
habría que reutilizar para un bootloader de actualización OTA: la etapa que copia
`/xinu.img` de la SD a la flash **no puede correr desde el kernel XIP que está
sobrescribiendo**. Ver el diseño en [05-roadmap-y-conf.md](05-roadmap-y-conf.md#ota).
