# 7 · Organización de la flash (diseño, no aplicado)

> **Estado: SOLO DISEÑO.** Este documento no cambia nada: ni `ld.script`, ni `ulp.ld`,
> ni las reglas `flash`/`flashxinu`, ni el SBL. Describe el mapa propuesto, las
> restricciones que lo fijan, el bootloader de segunda etapa (`bl2`) que se añade **después
> del SBL**, y el encogimiento de la ULP a **64 KB de flash y 20 KB de RAM** que se pidió.
> Cada afirmación de "hoy" va con su `fichero:línea` y su cifra medida el 2026-11-04 en el
> árbol de `compile/` y `machine/build/`.

---

## 1. Punto de partida medido (lo que hay ahora)

Flash **16 MB** (`--flash-size 16MB`, [compile/platforms/esp32p4/platformVars:170](../../compile/platforms/esp32p4/platformVars))
y PSRAM **32 MB** ([device/psram/psramInit.c:26](../../device/psram/psramInit.c)). La ventana
XIP arranca en `0x40000000` y es 1:1 con el offset físico (medido con `syscheck flash`,
[06-syscheck.md](06-syscheck.md) §4), así que *offset en flash + `0x40000000` = dirección que
ve el core*.

| offset flash | XIP | imagen | medida hoy |
|---|---|---|---|
| `0x002000` | `0x40002000` | SBL (cabecera esptool, magic `0xE9`) | **4.304 B** (`machine/build/sbl.bin`); `.text` 4.186 + `.rodata` 45 + `.bss` 6 |
| `0x005000` | `0x40005000` | kernel Xinu, `xinu.boot` **plano, sin cabecera** | **298.732 B**, `_etext = 0x4004DD00` |
| `0x100000` | `0x40100000` | ULP, binario plano | **724 B** (`.text` 672 + arranque 2 + ctors/dtors 8 + `clear_sec` 16 + `copy_sec` 24) |

Tres huecos y cuatro verdades incómodas:

- **No hay tabla de particiones en ningún sitio** ([01-memoria-nucleos.md:63](01-memoria-nucleos.md),
  `:90`); se escribe a tres direcciones fijas ([platformVars:167-171](../../compile/platforms/esp32p4/platformVars)).
- Entre el final del kernel (`0x4DD00`) y la ULP (`0x100000`) quedan **729.364 B (~712 KB) sin
  proteger**: `FLASH LENGTH = 31M` ([ld.script:26](../../compile/platforms/esp32p4/ld.script)) no corta
  contra los 16 MB reales, así que `ld` deja crecer al kernel hasta pisar la ULP en silencio.
- **El port no sabe escribir su flash.** No hay driver de programa/borrado en `device/` ni en
  `system/` (solo definiciones de registro en `machine/sbl/esp32p4.h` y la API ROM
  `SPI_FLASH_MMAP_INVALIDATE_CACHE_ADDR`, [include/esp32p4/rom/cache.h:1595](../../include/esp32p4/rom/cache.h)).
  Quien escribe la flash hoy es `esptool` por USB. Esto es el trabajo gordo del OTA, no el mapa.
- La RAM de la ULP **no está en LP_SRAM**: está en el último megabyte de PSRAM
  (`RAM ORIGIN = 0x49F00000, LENGTH = 1M`, [machine/ulp/ulp.ld:42](../../machine/ulp/ulp.ld)), y el
  kernel cierra justo ahí con `RAM LENGTH = 31M` ([ld.script:27](../../compile/platforms/esp32p4/ld.script),
  `_psram_free_end` en `:245`). `LP_SRAM (0x50108000, 32K)` está declarada y **sin usar**
  ([ld.script:29](../../compile/platforms/esp32p4/ld.script)).

---

## 2. Restricciones duras (lo que el diseño no puede discutir)

| # | restricción | de dónde sale |
|---|---|---|
| R1 | La BootROM solo mira `0x2000` y exige cabecera ESP con magic `0xE9`; copia el SBL a HP_TCM `0x30100000` (**8 KB**) y lo ejecuta ahí. | [machine/sbl/SBL.ld:19](../../machine/sbl/SBL.ld), [machine/Makefile:78-84](../../machine/Makefile), [01-memoria-nucleos.md:11-26](01-memoria-nucleos.md) |
| R2 | El SBL salta a `__kernel_entry_point = 0x40005000`, dirección **literal**. Cualquier cosa que vaya entre el SBL y el kernel tiene que ocupar exactamente el offset `0x5000`. | [SBL.ld:28](../../machine/sbl/SBL.ld), `machine/sbl/boot.S:63-66` |
| R3 | El SBL carga `__ulp_entry_point = 0x40100000` en `ULP_ENTRY_POINT_REG (0x50110028)`. Mover la ULP de `0x100000` cuesta editar `SBL.ld` **y** `ulp.ld` y re-flashear el SBL. | [SBL.ld:33](../../machine/sbl/SBL.ld), [machine/sbl/ulp_boot.c:33-64](../../machine/sbl/ulp_boot.c) |
| R4 | El kernel es XIP con **VMA == LMA** ([01-memoria-nucleos.md:41-43](01-memoria-nucleos.md)). Una ranura de app **es** su dirección de enlace: A/B obliga a enlazar dos veces el mismo objeto. | [ld.script:26](../../compile/platforms/esp32p4/ld.script) |
| R5 | `.data` vive en flash (`AT>FLASH`) y `start.S` la copia a L2MEM; los primeros **128 KB de L2MEM no tienen celdas** (banda fantasma, corte real en `0x4FF20000`). Cualquier código que se ejecute durante una escritura de flash tiene que ir a `≥ 0x4FF20000`, no a la ventana vieja. | [ld.script:160-203](../../compile/platforms/esp32p4/ld.script), [06-syscheck.md:233](06-syscheck.md) |
| R6 | El MMU del SBL mapea la flash en **páginas de 64 KB** (512 entradas, [machine/sbl/mmu.c:25-51](../../machine/sbl/mmu.c)); el borrado mínimo es de sector **4 KB**. | ⇒ particiones alineadas a 64 KB salvo el bloque fijo `0x2000..0x7FFF`, que manda la R1/R2 |
| R7 | Topes del heap L2 ya fijados: `memheap = _end + PANICSTK + NULLSTK`, y `_l2mem_free_end = 0x4FFB0000` porque los últimos 64 KB son ROM. | [loader/platforms/esp32p4/start.S:143-152](../../loader/platforms/esp32p4/start.S), [ld.script:233-241](../../compile/platforms/esp32p4/ld.script) |

---

## 3. Mapa propuesto (16 MB)

```
 offset      tamaño    nombre    XIP           qué vive ahí
----------  ---------  --------  ------------  --------------------------------
 0x000000     8 KB     margen    0x40000000    no se escribe (la BootROM lee 0x2000)
 0x002000    12 KB     sbl       0x40002000    SBL con cabecera ESP  [4.304 B]
 0x005000    12 KB     bl2       0x40005000    NUEVO bootloader 2ª etapa
 0x008000    32 KB     nvram     0x40008000    selector A/B + intentos + mini-tabla
 0x010000   960 KB     factory   0x40010000    imagen buena de fábrica, NUNCA OTA
 0x100000    64 KB     ulp       0x40100000    coprocesador LP  [724 B usados]
 0x110000     1 MB     app-a     0x40110000    kernel ranura A    [299 KB, 29 %]
 0x210000     1 MB     app-b     0x40210000    kernel ranura B    (candidata)
 0x310000     1 MB     stage     0x40310000    imagen descargada, sin montar
 0x410000  ≈ 11,9 MB   datos     0x40410000    registro / FAT en flash o reserva
```

Suma exacta: `0x410000 + 0xBF0000 = 0x1000000`. Todo borde cae en página de 64 KB (R6) salvo
los cuatro primeros, que fija el hardware (R1/R2).

Decisiones que lleva puestas:

- **`bl2` ocupa `0x5000`** porque es el único destino que conoce el SBL (R2). No hay que
  tocar el SBL para añadir el bootloader: el salto de `0x40005000` pasa a caer en `bl2` en vez
  de caer en el kernel, y `bl2` decide dónde va el kernel.
- **La ULP se queda en `0x100000`** con **64 KB** exactos (un bloque de borrado, 88 veces los
  724 B que usa hoy). Así se respeta R3: ningún cambio en el SBL. Su `ulp.ld:41`
  `FLASH LENGTH = 1M` pasa a `64K`, que además **corta de verdad**: hoy puede crecer 1 MB por
  encima de su partición y pisar `app-b` sin que `ld` avise.
- **App slots de 1 MB** (3,4× lo que ocupa el kernel hoy) en vez de los 512 KB del plan A
  antiguo, porque con `-O2` y LVGL/FAT dentro el kernel ya pesa 299 KB y 512 KB daban 1,7×.
- **`factory` de 960 KB** en el hueco que hoy está sin proteger: es la imagen que garantiza que
  un A/B mal cerrado no deja la placa muerta. No la toca nunca el mecanismo de actualización;
  solo `make flash` por USB.
- **`stage` en flash** (1 MB) en lugar de `/xinu.img` en la microSD del diseño anterior
  ([05-roadmap-y-conf.md §6.2](05-roadmap-y-conf.md)): copiar 300 KB desde flash a flash es
  ~2,6 s a ~115 KB/s de programación, y no depende de que la SD esté montada ni de
  `sdmmcBusLock()` compitiendo con el C6.

### 3.1 RAM de la ULP: 20 KB

Hoy la ULP usa **1 KB** (`__STACK_SIZE = 1K`, [ulp.ld:33](../../machine/ulp/ulp.ld); `.bss` = 0 y
`.data` = 0 medidos en `machine/build/ulp.elf`). 20 KB es margen 20× para el caso de uso que
se le dé (anillo de sensores, colas). Dos formas de dárselas:

| opción | dónde | qué cambia | riesgo |
|---|---|---|---|
| **R-1 conservadora** | cola de PSRAM: `0x49FFB000..0x4A000000` | `ulp.ld:42` → `ORIGIN = 0x49FFB000, LENGTH = 20K`; `ld.script:27` → `RAM LENGTH = 32M-20K` (`0x01FFB000`) | ninguno nuevo: mismo bus que ya usa hoy; **gana ~1.028 KB** de heap PSRAM el kernel |
| **R-2 recomendada** | `LP_SRAM`: `0x50108000..0x5010D000` (20 KB de los 32 KB, 12 KB libres como buzón HP↔LP) | `ulp.ld:42` → `ORIGIN = 0x50108000, LENGTH = 20K`; `ld.script:27` y `:245` → `RAM LENGTH = 32M` | hay que **medir** que el core LP accede a `0x50108000` (patrón escribir/leer desde `ulp/main.c`) y que la región no se apaga con el dominio de potencia LP. A cambio el kernel se queda los **32 MB enteros** de PSRAM |

Con R-2 desaparece el acoplamiento peligroso que ya advierte
[01-memoria-nucleos.md:99-114](01-memoria-nucleos.md): hoy `RAM LENGTH = 31M` del kernel y
`RAM ORIGIN = 0x49F00000` de la ULP son **la misma cota escrita dos veces en dos ficheros**; si
alguien la sube a 32M, el heap del kernel se come la RAM de la ULP en silencio y el fallo
aparece como corrupción en el C6. Mover la ULP a LP_SRAM corta ese contrato por la raíz.

---

## 4. `bl2`: el bootloader después del SBL

### 4.1 Qué es y qué no es

12 KB en `0x5000`, XIP, sin PSRAM y sin drivers de periférico salvo SPI0 para leer (que es
gratis en XIP). **Sí** es: leer el selector, validar integridad, elegir ranura, desarmar el
perímetro de arranque, saltar. **No** es: el aplicador que copia `stage`→ranura (eso lo hace el
kernel, que tiene el driver de flash y la red) ni un cargador que mueva código a RAM (R4: aquí
no se copia, se fetchea desde la ventana).

Se apoya en el mismo truco que ya demostró el SBL — ejecutarse fuera de la flash que se va a
tocar —, anotado como reutilizable en [01-memoria-nucleos.md:263-269](01-memoria-nucleos.md).

### 4.2 Formato de imagen (cabecera de 64 B al principio de cada ranura)

```c
struct xinu_img_hdr {           /* en el offset 0 de app-a / app-b / factory */
    uint32_t magic;             /* 'XKRN' — OJO: no es 0xE9, ver 06-syscheck.md §4 */
    uint16_t version;           /* 1 */
    uint16_t hdr_size;          /* 64 */
    uint32_t load_size;         /* bytes de imagen plana tras la cabecera */
    uint32_t entry_off;         /* desplazamiento del _start dentro de la ranura */
    uint32_t crc32;             /* de load_size bytes desde entry_off */
    uint32_t build_id;          /* fecha/serial del build, para el banner */
    uint8_t  sha256[32];        /* opcional, para firmar luego; 0 = sin firma */
};
```

`xinu.boot` hoy **no tiene cabecera** (es `objcopy -O binary` puro,
[compile/arch/riscv32/platformVars:24-25](../../compile/arch/riscv32/platformVars));
el diseño añade un paso de embalaje (`bl2_pack`) que antepone la cabecera y **desplaza** el
código 64 B, o, para no tocar el enlace, enlaza la ranura en `base + 64` y pone la cabecera en
`base`. Se propone lo segundo: el `ORIGIN` del `FLASH` de cada slot queda en
`0x40110040` / `0x40210040` y la cabecera en `0x40110000`.

### 4.3 `nvram` (32 KB, 8 páginas de 4 KB)

| página | contenido |
|---|---|
| 0..3 | **otadata**: 4 réplicas, cada una con 128 registros de 32 B `{seq, slot_activa, slot_pendiente, intentos, crc32}`. Se escribe el siguiente hueco libre; cuando la página se llena, se borra y se empieza al principio. Gana el `seq` más alto con CRC válido entre las cuatro. Cuatro réplicas porque una escritura interrumpida por corte de batería no debe dejar la placa sin selector. |
| 4..5 | mini-tabla firmada: copia de este mapa en binario, para que `bl2` y `syscheck` lean el layout en vez de llevarlo embebido (es la "mini-tabla en `0x10000`" de [05-roadmap-y-conf.md §6.3](05-roadmap-y-conf.md), aquí movida a `0x008000+16K`). |
| 6..7 | libres (registro de arranques, contadores de OTA). |

### 4.4 Secuencia de arranque y anti-bridado

```
bl2:
  leer otadata (4 réplicas) → slot = activa, pend = pendiente, n = intentos
  si pend == ninguna:                       # caso normal
      validar cabecera+CRC de slot; si OK → saltar
      si no → validar la otra; si OK → saltar y anotar "slot degradada"
      si ninguna → saltar a factory (0x010000) y anotar "rescate"
  si pend != ninguna:                        # hay OTA que confirmar
      copia ya hecha por el kernel; slot = pend; intentos = 3
      armar watchdog (RWDT) y saltar
  pasar a la imagen: a0 = base XIP de la ranura, a1 = &struct bl2_info{slot, intentos, motivo}
                     (bl2_info en L2MEM, 0x4FF20000, R5)
```

- **Confirmación sin escribir flash**: el kernel **desarma el watchdog** al final de `netboot`
  (o en `main`, si se prefiere más atrás). Si la imagen nueva se cuelga antes, el watchdog
  resetea, `bl2` ve `pend` sin confirmar, `intentos--`, y a las 3 vueltas prueba la otra ranura
  y acaba en `factory`. Ventaja: el kernel nunca escribe el selector en el camino crítico, y el
  "éxito" lo define un hecho observable (sobrevivir N segundos con la red arriba), no un
  `return OK`.
- **Nunca borrar la ranura activa**: el kernel escribe siempre la **inactiva** (o `stage`) y el
  cambio de selector es el único byte crítico, protegido por las 4 réplicas.
- **`factory` inmutable por OTA**: solo se reescribe por USB con `make flash`.
- Queda por medir si el RWDT del P4 sobrevive al `wdt_disable_all_wdt` que hace el SBL al entrar
  ([machine/sbl/boot.S:40](../../machine/sbl/boot.S)): si no, la confirmación se hace con un
  contador de `intentos` en otadata y un reset suave, que es el plan B del mismo esquema.

---

## 5. Relación con lo ya escrito (esto **sustituye**, no convive)

[05-roadmap-y-conf.md §6.3](05-roadmap-y-conf.md) proponía el **plan A**: kernel activo en
`0x5000`, candidata en `0x80000`, flag en `0xF8000`, backup en `0xC0000`, aplicador dentro del
SBL. Este diseño lo **reemplaza** por un motivo mecánico: `0x5000` es el destino fijo del salto
del SBL (R2), así que no puede estar ocupando el kernel activo *y* siendo el sitio del
bootloader que decide. Consecuencias de la sustitución:

| | plan A (05 §6.3) | este diseño |
|---|---|---|
| quién decide ranura | el SBL (hay que re-flashear el SBL en cada cambio de lógica) | `bl2` (12 KB, se re-flashea solo: `make flashbl2`) |
| kernel activo | `0x5000` | `0x110000` o `0x210000` |
| candidata | `0x80000` (≤512 KB) | ranura inactiva (1 MB) + `stage` (1 MB) |
| flag/selector | `0xF8000` una página | `nvram` 32 KB con 4 réplicas |
| rescate final | backup en `0xC0000` | `factory` 960 KB |
| SBL | modificado | **sin tocar** (R1/R2/R3 respetadas) |

Y **[01-memoria-nucleos.md §2](01-memoria-nucleos.md)** sigue siendo el mapa *vigente*; este
documento es el mapa *propuesto*. Cuando se aplique, la §2 de ese doc pasa a ser historia con
una fecha.

---

## 6. Qué ficheros se tocarían (lista, **sin ejecutar**)

| fichero | línea | cambio |
|---|---|---|
| `compile/platforms/esp32p4/ld.script` | 26 | `FLASH ORIGIN` → `0x40110040` (slot A) / variante `0x40210040` (slot B) vía `-DSLOT` o `ld -T` por ranura |
| idem | 27, 245 | `RAM LENGTH` → `32M` (si R-2) o `0x01FFB000` (si R-1) |
| `machine/ulp/ulp.ld` | 41, 42 | `FLASH LENGTH = 64K`; `RAM ORIGIN/LENGTH = 20K` en `0x50108000` (R-2) o `0x49FFB000` (R-1) |
| `machine/sbl/SBL.ld` | 28 | sin cambio de valor (`0x40005000` ahora apunta a `bl2`); 33 sin cambio (ULP se queda en `0x100000`) |
| `compile/platforms/esp32p4/platformVars` | 162-179 | offsets nuevos en `flash`/`flashxinu` + reglas `flashbl2`, `flashfactory`, `mkimage` |
| `machine/bl2/` (nuevo) | — | `boot.S`, `main.c`, `bl2.ld` (XIP en `0x40005000`), `flash_ops.S` (rutina de programa/borrar que se copia a `0x4FF20000`) |
| `device/flash/` (nuevo) | — | driver de write/erase del kernel: es el trabajo grande y hoy **no existe** |
| `shell/xsh_syscheck.c` | subcomando `flash` | leer cabeceras de `factory`/`app-a`/`app-b`/`stage`/`nvram` y reportar `slot activa + intentos`; sin esto no hay forma de auditar el mapa en placa |

Orden de implantación propuesto (cada etapa con su prueba en placa, ninguna se aplica sin OK):

1. **Preparar el mapa sin cambiar el arranque**: `ulp.ld` a 64 KB/20 KB (R-1, cero riesgo de
   bus) y `FLASH LENGTH = 15M` en el kernel para que `ld` corte si crece demasiado. Prueba:
   `syscheck flash` sigue dando `0x6f` en `0x5000` y `0x97` en `0x100000`, y `syscheck net` intacto.
2. **Driver de escritura de flash** (`device/flash`) + `syscheck flash --crc`. Prueba: copiar
   `xinu.boot` a `stage` y volverlo a leer con CRC igual, sin reset, y con el C6 enviando
   tráfico mientras (comprobar que la ventana de interrupciones cortadas no rompe esp_hosted).
3. **`bl2` mínimo**: lee cabeceras, valida CRC y salta a `app-a` (single-slot todavía). Prueba:
   el banner imprime `bl2 v1 slot=A crc OK` y todo lo de antes funciona igual.
4. **Ranura B + selector** con otadata de 4 réplicas. Prueba: cambiar el selector a mano y ver
   que arranca B; cortar la alimentación a mitad de escritura y comprobar que elige la réplica sana.
5. **OTA real** = `stage` → ranura inactiva → `pend` → watchdog → confirmación. Prueba: imagen
   mala a propósito; la placa tiene que volver sola a la buena en ≤3 resets.
6. **`factory`** escrita una vez por USB y declarada inmutable.

---

## 7. Costes, riesgos y lo que esto NO resuelve

- **Dos enlaces por release** (R4): cada imagen se compila para su ranura. Cuesta ~40 s de
  build extra y un `make SLOT=A SLOT=B`; a cambio no hay que copiar 300 KB a RAM en cada
  arranque. La alternativa (BL2 copia la ranura a PSRAM y salta allí) unifica el enlace pero
  pierde el XIP, añade ~1 s de arranque y se pelea con `.audioshare`/`.sdshare` en
  `0x48000000`.
- **Escribir flash mientras se ejecuta XIP es la parte delicada**, no el mapa: hay que correr
  desde L2MEM (`≥0x4FF20000`, R5), invalidar la caché del rango
  ([include/esp32p4/rom/cache.h:1595](../../include/esp32p4/rom/cache.h)) y tener en cuenta que el
  SPI0 está arbitrado con el PSRAM (`SPI0_GRANT`, registros en `machine/sbl/esp32p4.h:56936`).
  Riesgo concreto: una escritura larga con interrupciones desactivadas come ticks del reloj ya
  de por sí derivados 0,134 ms/tick ([05-roadmap-y-conf.md §1.0](05-roadmap-y-conf.md)), y puede
  partir una trama del C6 por el medio.
- **El reloj y `sleep()`** no cambian aquí; si se corrige `P4_TIMER_FREQ`, los timeouts del
  watchdog de la etapa 5 se recalculan con la medida nueva, no con la declarada.
- **No resuelve**: que no haya NVS/credenciales fuera del fuente (la AP sigue siendo
  constante de `shell/xsh_netboot.c:55`), ni el FAT de la SD (que sigue siendo el volumen de
  datos; la partición `datos` es opcional y para registro/estado, no para reemplazar la SD),
  ni la latencia en reposo del C6 (#61), ni las carreras de telnet (#67).
- **Presupuesto real de la partición de app**: 299 KB hoy / 1 MB. Si el port llega a traer
  LVGL completo + FAT + pila Java, 1 MB por ranura se queda corto; la salida barata es mover
  `stage` al principio de `datos` y dejar ranuras de 2 MB, que es un cambio de 4 líneas de este
  mapa y de `platformVars`, no de arquitectura.
