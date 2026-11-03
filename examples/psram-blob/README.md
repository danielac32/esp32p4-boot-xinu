# PSRAM: ejecutar código copiado a runtime

Material de prueba **fuera de la compilación de Xinu** (`examples/` no está en
`COMPS`, ver `compile/Makefile:242`).  Nada de lo de este directorio entra en el
kernel salvo que el dueño lo copie a `shell/` a propósito.

## Qué hay aquí

| fichero | qué es |
|---|---|
| `sum.c` | blob hoja: `int sum(int a,int b)`. 4 bytes, 1 línea de caché, 0 relocaciones. |
| `sqsum.c` | blob complejo: `sqsum(v, n, add, pmax)`. 120 bytes, 2 líneas de caché, 0 relocaciones. |
| `link.ld` | enlaza en 0 y pone `.text.entry` primero, para que la entrada caiga en el offset 0 del volcado plano. |
| `Makefile` | `make` = compilar, enlazar, `objcopy -O binary --only-section=.text` y verificar; `make verify` = solo comprobaciones. |
| `sum_blob.h`, `sqsum_blob.h` | headers generados (array de bytes + `_blob_len`). No editar. |
| `xsh_runblob.c`, `xsh_runbig.c`, `xsh_irqmap.c` | **cargadores de referencia**, no compilados aquí. Explican el procedimiento y miden la caché. |

`sum.bin`, `sum.elf`, `sum.o` etc. son intermedios del `make` (`.PRECIOUS` para
que GNU make no los borre como prerequisitos de patrón).

## Las tres cosas que se midieron en placa (2026-11-02)

1. **No es un problema de permisos.** `0x48......` (con caché) y `0x88......`
   (directa, `+0x40000000`, `PSRAM_CPU_UNCACHED` en `device/psram/psramInit.c:36`)
   son las **mismas celdas**.  El MMU de memoria externa del P4 solo tiene bits de
   validez y de tipo flash/PSRAM, sin lectura/escritura/ejecución
   (`MMU_PSRAM_TLB_MARK_AS_VALID_ENTRY`, `machine/sbl/mmu.c:28`), y este kernel no
   configura PMP.  `compile/platforms/esp32p4/ld.script` ya pone `RAM(rwx)` en
   `0x48000000`.  **Un puntero de `psram_malloc()` es saltable.**

2. **El culpable es la jerarquía de caché, concretamente la L2.**  El camino de
   instrucciones es icache → L2 → PSRAM (la L1 de instrucciones es un array
   aparte; `machine/sbl/esp32p4.h:6233`).  Copiar por `0x48` deja la línea sucia en
   L1-D; copiar por `0x88` escribe en la celda pero **no invalida la línea que L2
   ya tenía** para ese bloque.  Los dos fallan igual, medido:
   `mepc=0x48027008 mcause=2 mtval=0xaaaaaaab` con la celda verificada como
   correcta por `0x88`; y en la variante por `0x48`, `mtval=0x20aa`, que es la
   mitad baja de la celda vieja `0xaaaa20aa`.  Con `0xaaaa...` porque el kernel no
   borra la PSRAM al liberar.
   Lo que sí funcionó: **copiar por `0x88` y saltar por `0x88`** → `sum(2,3)=5`,
   `sum(40,2)=42`.  Sin icache ni L2 en el camino, el fetch lee la celda tal cual
   (a cambio de ir sin caché de instrucciones).
   La ruta "correcta" con caché pide una operación explícita:
   `CACHE.sync_map` (bits `[0]` L1-ICache0, `[4]` L1-D, `[5]` L2),
   `CACHE.sync_addr` (dirección **virtual**), `CACHE.sync_size` y
   `CACHE.sync_ctrl` (`include/soc/cache_struct.h:5590-5593` y `:1611`), y luego
   `fence.i`.  Ojo: los envoltorios de `include/hal/cache_ll.h` **no enlazan** en
   este árbol porque delegan en los símbolos de ROM `Cache_Invalidate_Addr` /
   `Cache_WriteBack_Addr`, cuya tabla de funciones no está portada.  La línea de
   caché es de 64 bytes como mínimo (`MIN_L1_ICACHE_LINE_SIZE`,
   `include/esp32p4/rom/cache.h:31`; configurable hasta 128), así que un bloque ya
   cargado una vez queda en icache y otro blob en el mismo bloque exige repetir la
   invalidación.

3. **Mina para cualquier lazo largo, y de quién fue la excepción.**  El
   `mcause=0xb8000007` de la parada es id **7 = `IRQ_TIMER`** (`include/conf.h:75`):
   el machine timer de RISC-V que habilita `clkinit()` (`system/clkinit.c:56-57`) y
   genera el CLINT al comparar `mtime` contra `mtimecmp`
   (`system/platforms/esp32p4/riscvclk.c:39-46`).  **No** es `PMU1_IRQn = 7`
   (`machine/sbl/esp32p4.h:30`, copia en `include/esp32p4.h:108`): ese número es una
   fuente ETS del enum heredado de ESP-IDF, otro espacio de nombres.  Un barrido de
   4 MB de lecturas dio tiempo de sobra para el tick; el dispatch cayó en
   `irq_missing()` (`system/platforms/esp32p4/clic.c:61`), que termina en `for(;;)`
   y **para la placa hasta un reset**.  Que el vector saliera `0` con `ie=1` es lo
   que delata la corrupción: en este árbol `interruptVector[]` solo lo escriben
   `clic_init()` (→ `p4_irq_default`), `clkinit()` (→ `clkhandler`) y
   `uartHwInit()` (→ el del device); ninguno escribe nunca 0, así que el `.bss`
   estaba pisado — muy probablemente por las corridas anteriores, donde el núcleo
   ejecutó instrucciones de basura sacadas de una línea L2 rancia.  Consecuencia
   para el SDIO/WiFi: **ninguna línea puede quedar con `ie=1` sin su
   `interruptVector[]` puesto antes.**

Para no volver a adivinar de quién es un tick está `xsh_irqmap.c` (llama a una
versión local de `clic_report()`, ya no al kernel): vuelca `ip`/`ie`/`attr`/`ct` de
las 48 líneas del CLIC, la matriz `INTMTX`, `interruptVector[]` y
`mtime`/`mtimecmp`/`clkticks`.

## Reglas para que un blob siga siendo plano

1. Sin datos propios: enlazado en 0, cualquier referencia a su `.rodata` calcularía
   la dirección cerca de `0x0000xxxx` y petaría con `mcause=5`.  Los punteros los
   inyecta el llamante como argumentos.
2. Sin llamadas fuera del blob (`-nostdlib` → relocación sin resolver).  GCC puede
   convertir inicializaciones de arrays locales grandes en llamadas a `memset`.
3. La función de entrada en la sección `.text.entry`, para que caiga en el offset 0
   del volcado.  Las llamadas entre helpers internas salen como `call` PC-relativa,
   sin relocación; por eso van marcadas `noinline`, si no GCC los funde con la
   entrada y el ejemplo deja de enseñar lo que quiere enseñar.

## Si algún día se quiere en la consola

Copiar a `shell/` los tres `xsh_*.c` y los dos `*_blob.h`, añadirlos a
`shell/Makerules`, a la `commandtab` de `shell/shell.c` y a `include/shell.h`, y
**reconstruir limpio** (`make clean && make`): al tocar un `.h` el build incremental
no recompila a los dependientes porque `DEPFLAGS` está vacío en `compile/Makefile`.
