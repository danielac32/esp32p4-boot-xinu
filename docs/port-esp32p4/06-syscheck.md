# Medir el CPU, validar `millis()` y chequear el kernel

Punto 1 del pedido (énfasis del usuario: *"quiero que hagas un comando, para medir
a qué velocidad está realmente el CPU, y si `millis()` funciona, quiero que hagas
una comprobación completa del kernel"*).

Entregable: el comando **está cableado y medido en placa** (autorizado y
flasheado el 2026-11-04).  El fuente vive en
[shell/xsh_syscheck.c](../../shell/xsh_syscheck.c), entra en el build por
`C_FILES += xsh_syscheck.c` ([shell/Makerules](../../shell/Makerules)), y se
llama `syscheck` desde la shell ([shell/shell.c:80](../../shell/shell.c#L80),
prototipo en [include/shell.h:154](../../include/shell.h#L154)).  En
[`examples/syscheck/`](../../examples/syscheck/README.md) queda el PC-side
(`p4clk.py`) y la tabla de las cifras medidas.

**Resultado en una línea:** el core va a ~320 MHz, no a 360; `mcycle/mtime` vale
0,9999; `millis()` es monótona y está sana de forma, pero **un segundo del
kernel dura 1,26 s reales**.  `syscheck all` cierra con 23 tests / 19 OK /
4 avisos / 0 fallos.

---

## 1. Por qué "360 MHz" no es una medida

El port arranca con `platform.clkfreq = P4_TIMER_FREQ = 360 MHz`
([system/platforms/esp32p4/platforminit.c:39](../../system/platforms/esp32p4/platforminit.c#L39),
con el define en [include/esp32p4.h:160](../../include/esp32p4.h#L160), que dice
literalmente `CPU root clock == mtime tick`: eso es justo la hipótesis bajo prueba).
Ese número es la frecuencia que se **supone** para el contador `mtime` de la CLINT, y de él derivan
`CLKTICKS_PER_SEC`, `udelay()` y `micros()`. No es una medición del reloj del
núcleo, y hay tres cosas que lo hacen sospechoso:

1. **Es el reloj del timer, no el del CPU.** En el P4 la CLINT y el core pueden
   comer de relojes distintos. Nada en el árbol obliga a que sean el mismo.
2. **En RV32 `clkcount()` solo devuelve la palabra baja.** 2³²/360e6 = 11,93 s
   de vuelta ([system/timershim.c:61-66](../../system/timershim.c#L61)). Todo lo
   que se mida con restas simples tiene que caber en ese margen, y el `micros()`
   heredado del SDK ya tropezó con esto: era un diente de sierra de 12 s
   ([system/timershim.c:29-40](../../system/timershim.c#L29)).  *(Medido: con el
   divisor real de 320,010 MHz la vuelta es de **13,4 s**, no 11,93 — el margen
   es más generoso de lo que decía el cálculo, pero la regla de "restas solo
   modulares" se queda igual.)*
3. **El cristal real tiene tolerancia.** Un ±0,5 % de cristal son 1,8 s al día.
   Eso no se ve contra sí mismo; solo contra un reloj de fuera.

La única referencia absoluta que existe es **el PC**. De ahí el par
`syscheck clk` → `p4clk.py`.

**Medido (2026-11-04): las tres sospechas se confirmaron, y el número era
peor que una tolerancia de cristal.**  El `mtime` corre a **320,010 MHz**
reales contra el PC, y el core va 1:1 con él.  O sea, el `clkfreq` declarado
está **un 11,1 % arriba** — no es deriva del oscilador, es un divisor
configurado en 9 cuando el reloj da 8 (40 MHz de cristal).  Toda cantidad que
derive de `platform.clkfreq` está hinchada en esa proporción, y ahí entran
`CLKTICKS_PER_SEC`, `udelay()` y `micros()`.  **No se ha corregido**: cambiar
`P4_TIMER_FREQ`/`platform.clkfreq` reescribe el periodo del tick del kernel, y
esa modificación no está autorizada.  La propuesta de corrección queda en
[05-roadmap-y-conf.md](05-roadmap-y-conf.md) §1.

---

## 2. Velocidad real del core: `mcycle`

El P4 es RV32IMAFDC y el SDK bare-metal usa los CSRs `mcycle`/`mcycleh`
(`0xB00`/`0xB80`) como contador de ciclos —
`/home/daniel/esp32-riscv-bare-metal-sdk-main/core/riscv/include/riscv/rv_utils.h:95`.
Ese es el mando que da la frecuencia del núcleo:

```
MHz_del_core = mcycle_delta / (mtime_delta / 1e6)
```

Tres detalles del `sc_cpu()` que no son ornamentales:

- **Lectura en dos palabras con relectura del alto.** En RV32 el contador bajo
  envuelve dentro del intervalo de medida; sin el `do { hi; lo; hi2 } while (hi
  != hi2)` el bit que se pierde es silencioso. `csr` con la dirección en registro
  **no** vale: `csrr` exige inmediato, así que hay una función por CSR
  ([shell/xsh_syscheck.c](../../shell/xsh_syscheck.c), `sc_cpu`).
- **Sin coma flotante** (regla del proyecto). La división se escala:
  `hz_x1000 = dcyc * 1e6 / dt` en `uint64_t`.
- **Aviso de pánico.** Si el core no implementa `mcycle`, el `csrr` levanta
  instrucción ilegal, el kernel panica y la placa para hasta reset. Por eso
  `syscheck all` **no** llama a `cpu`.

### Cómo leer el ratio

Lo que interesa no es la cifra de MHz, es la **razón** entre ciclos del core y
cuentas de `mtime`:

| ratio `mcycle/mtime` | qué significa | qué hacer |
|---|---|---|
| ≈ 1,00 | core y CLINT van al mismo reloj; **la frecuencia hay que medirla fuera** para saber cuál | cerrar la cifra en AGENTS.md §5 |
| ≈ 9 | la CLINT va a 40 MHz y el core a 360 | el `clkfreq` del kernel describe el **tick**, no la velocidad del CPU: los `udelay()` están bien, el "CPU a 360 MHz" es una interpretación |
| ≈ 0,11 | la CLINT va más rápido que el core | alguien está corriendo el núcleo por debajo de lo supuesto; revisar la config del APLL/XTAL en el SBL |
| trap / pánico | sin `mcycle` accesible | quedarse en el bucle de referencia |

**Medido (2026-11-04): salió 0,9999** — la primera fila.  El core y la CLINT
comen del mismo reloj, así que `mcycle` no aporta información *absoluta* por sí
solo: solo puede decir que ambos son el mismo, no a cuánto.  Lo que falta para
cerrar la cifra es la referencia externa, y `p4clk.py` la dio: **320,010 MHz**
de `mtime`, o sea **319,977 MHz de core**.  Consecuencia sobre la tabla: la
fila "≈ 1,00 → los 360 MHz declarados son válidos" era la lectura ingenua, y es
**falsa**; el ratio 1 no valida el número declarado, solo identifica el reloj.

El `csrr` de `mcycle` **no paniqueó** en esta placa, como se temía en el último
punto de arriba.  Se puede correr `syscheck cpu` sin red.

### Segundo número, comparable entre builds

`mcycle` da la frecuencia; no dice si el camino de instrucciones se ha vuelto
lento. Para eso está el bucle de referencia: 200 000 iteraciones de
`loop0 = loop0*6 + 7` (mul+add+reg). No son DMIPS ni pretenden serlo: es una
**cifra comparable**. Si un día baja, algo del camino ha cambiado — caché,
espera de flash XIP, *wait states* de PSRAM, `-march`. Es el test que avisa de
una regresión de rendimiento, que es lo que se pierde cuando se toca el mapa de
memoria.

**Medido (2026-11-04): 200 000 iteraciones en 3 338 µs → 5,34 ciclos/iter,
59 916 117 iter/s.**  Ese es el número de referencia para comparar builds
futuros.  Ojo con una trampa que había en la primera versión: el bucle estaba
cronometrado con `millis()` y daba **0 ms** (200 000 iteraciones caben dentro de
un tick de 1 ms del kernel, y encima ese tick dura 1,26 ms reales).  Se cambió a
`micros()`, y el producto `LOOP_ITERS * 1000 / dt` hay que hacerlo en `uint64_t`
o desborda.

---

## 3. `millis()`: qué es y qué comprueba el test

`millis()` ([system/timershim.c:41](../../system/timershim.c#L41)) **no** sale del
`mtime`: sale de `clktime` y `clkticks`, que mantiene `clkhandler()` a
`CLKTICKS_PER_SEC` = 1000 Hz, leídos con las interrupciones enmascaradas porque
`clkintr()` resetea `clkticks` y sube `clktime` en el mismo handler.
Monótono hasta 2³² ms ≈ 49 días.

Eso es correcto **si** la interrupción de reloj llega. El test hace tres cosas:

1. **Monotonía y resolución** — 200 lecturas seguidas. Un retroceso es un fallo
   de la lectura en pareja; un salto de >5 ms en un bucle vacío son ticks
   perdidos.
2. **Coherencia con el contador crudo** — durante 2 s, `cuentas de mtime / ms de
   millis()` debe valer 360 000 con un 2 % de tolerancia (el margen lo come la
   planificación del `sleep()`, no el reloj). Las dos cantidades derivan del
   mismo `mtime`, así que esto mide la **consistencia** del reloj del kernel: si
   `clkhandler()` pierde interrupciones, aquí se ve.
3. **`micros()`** — 100 ms de sueño deben devolver 100 000 µs ±30 % (envuelve a
   los 13,4 s, así que solo intervalos cortos y siempre con resta modular).

### Los umbrales: por qué fallan dos de tres, y por qué eso es el hallazgo

El test 2 comparaba contra las 360 000 cuentas/ms que anuncia `clkfreq` con un
2 % de tolerancia, y el test 3 contra 100 000 µs con un 30 %.  **Medido
(2026-11-04), ambos se disparan, y no porque el reloj sea inestable**:

- **Cuentas de `mtime` por ms de kernel: 402 818**, no 360 000.  Son **+11,9 %
  sobre lo declarado**, y se descompone en dos defectos distintos:
  1. **El divisor está mal** (−11,1 %): el `mtime` real da 320 010 cuentas/ms,
     no 360 000 (§1).  Ese es el error de `clkfreq`.
  2. **`clkhandler()` pierde tiempo en cada tick** (+13,3 % sobre el real): el
     comparator se rearma 360 000 cuentas después, pero entre latido y latido
     pasan 402 818.  Esos **42 808 de diferencia = 0,13 ms por interrupción** es
     el coste de la propia interrupción (entrada, `clkintr()`, rearme) más las
     cuentas que no se recuperan porque el rearme es absoluto desde el valor
     *anterior* y se planifica con un `clkfreq` que no es el del contador.
- **Por eso un segundo del kernel dura 1,26 s reales**: `millis()` avanza 1000
  ms por cada 1258 ms de reloj (ratio kernel/PC medido **0,78066**; 29 000 ms de
  kernel transcurridos en 37 148 ms del PC).
- **`micros()` está hinchado en la misma proporción**: un `sleep(100)` devolvió
  **111 533 µs** en lugar de 100 000 (+11,5 %).  `micros()` es la palabra baja
  del `mtime` dividida por un `clkfreq` que no es el suyo, así que hereda el
  defecto 1.  El test 3 con tolerancia del 30 % lo deja pasar; la cifra hay que
  leerla igual.
- **Monotonía y resolución: OK.**  200 lecturas seguidas sin retrocesos ni
  saltos >5 ms.  Forma correcta, valor equivocado — el reloj no pierde ticks,
  los **cuenta** todos; lo que no cuadra es cuántas cuentas hay en un ms.

Y suelta 30 líneas `HB %03u xinums=… uptime=… mtime=…`, una por segundo del
kernel, que es lo que consume `p4clk.py`.

### El PC como referencia

Desde la shell de la placa:

```
/home/daniel/esp-idf-py/bin/python examples/syscheck/p4clk.py 172.16.0.117 30
```

(El `python3` del sistema no tiene `pyserial`.)  Telnet al puerto 23 (el
servidor que levanta `netboot`/`telnetserver`), manda `syscheck clk` y ajusta la
pendiente de `xinums` contra `time.monotonic()`.  Da ratio, ppm, deriva en
s/hora, la mediana de cuentas de `mtime` por segundo del kernel y los **MHz
absolutos del `mtime`** (esta última con corrección de envoltorio módulo 2³² por
paso, porque lo que llega por telnet es la palabra baja; ahora envuelve cada
13,4 s, no cada 11,93, ya que el divisor real es 320 MHz).

**Medido (2026-11-04): ratio kernel/PC 0,78066 y `mtime` = 320,010 MHz.**  Con
eso se cierra la pregunta del punto 1: el número que había que conseguir era
este, porque `mcycle` solo demostró que el core va 1:1 con el `mtime`.

**No abre `/dev/ttyACM0`** — abrir el puerto resetea la placa y se va el WiFi. Es
la única forma de medir sin perder el estado del enlace.

Límites del método, para no leer mal el número: el latido lo produce
`sleep(1000)`, o sea el reloj bajo prueba; el primer retardo lleva la conexión TCP
y el arranque del comando, así que se descarta; los saltos >1500 ms se cuentan
aparte porque son colas de la shell imprimiendo. Con 1 ms de resolución contra
*jitter* de red WiFi, **por debajo de ~200 ppm no hay nada interpretable**.  El
desvío medido es del 22 %, así que esa restricción no aplica: aquí sí hay
lectura.

Comprobado de `p4clk.py`: analizador en limpio y **medición hecha**.  Tuvo un
bug de temporización que dio primero un ratio absurdo de 105 278 — todas las
líneas de una misma `recv()` se fechaban en el mismo instante; ahora cada línea
completa hereda el `time.monotonic()` de la trama en la que llegó.

---

## 4. La "comprobación completa del kernel"

Ocho dominios, cada uno con su umbral y su veredicto `[ OK ] / [AVISO] / [FALLO]`.
El resumen devuelve `0` si no hay fallos, para que sirva en un script.

**Medido el 2026-11-04: `syscheck all` → 23 tests, 19 OK, 4 avisos, 0 fallos.**
Los cuatro avisos son legítimos y cada uno está explicado abajo: el alias sucio
(documentado, no es un fallo), las cuentas/ms del reloj (el hallazgo del §3), la
auditoría del CLIC (sigue sin portar) y el pico de pilas (indicio deliberado).

| dominio | qué mira | umbral | medido 2026-11-04 | qué se rompe si falla |
|---|---|---|---|---|
| `clk` | monotonía de `millis()`, cuentas/ms, `micros()` | 360 000 ±2 % | monótona **OK**; **402 818 cuentas/ms** `[AVISO]`; `micros()` 111 533 µs en 100 ms **OK** (tolerancia 30 %) | todo retardo y todo temporizador de red; los `TCP_FREQ` ticks se van |
| `cpu` | `mcycle/mtime`, bucle de referencia | ratio y tiempo del bucle | ratio **0,9999** → core **319,977 MHz**; bucle **5,34 ciclos/iter** (3 338 µs / 200 000) | ni velocidad ni rendimiento del camino de instrucciones |
| `mem` | `memlist` (heap L2MEM), `psram_free_bytes()`, patrón de 64 palabras vía `memget(256)`, alias `0x48`/`0x88`, `_l2reserve` | >100 KB L2, >16 MB PSRAM, patrón íntegro, corte 0x4FF20000 | patrón 64/64 **OK**; alias `[AVISO]` (escribe `0xC0FFEE11` por `0x88`, lee `0xaaafeeab` por `0x48`); banda fantasma **OK** | la banda fantasma vuelve: líneas leídas a cero, `irq with null vector`, cuelgue mudo |
| `net` | `netLookup(getdev("ETH0"))`, `semcount(bfptab[netpool].freebuf)`, ARP, rutas, `ESPETH_CTRL_STATS` | libres > nbuf/4, `rx_drops` < 50 | 4/4 **OK**; libres **23 de 24** en reposo, **16 de 24** con `ping` sostenido; `drops 0` | la pila espera en `bufget()` y el espeth descarta: es la pérdida de tramas que ya se arregló |
| `dev` | `I2C_CTRL_PROBE` 0x18/0x34/0x38, `POWER_CTRL_PERCENT`, `AUDIO_CTRL_GET_VOLUME`, `TOUCH_CTRL_TYPE`, LBA0 con `0x55AA`, `fs_mounted()` | presentes y en rango | todos **OK**; LBA0 leído con `SD_CTRL_SEEK` y firma `blk[510]=0x55`/`blk[511]=0xAA` correcta | cualquier dispositivo del AGENTS.md §4 deja de dar por buena la regresión |
| `int` | qué hace falta para auditar el CLIC | — | `[AVISO]`: sigue siendo un `printf` con la receta; hace falta portar `examples/psram-blob/xsh_irqmap.c` | una línea con `ie=1` y `interruptVector[]` nulo cae en `irq_missing()` y para la placa (`system/platforms/esp32p4/clic.c:61`) |
| `flash` | ventana XIP 1:1 con el offset de fichero, magic `0xE9` del SBL en 0x2000 y `_etext` vs 0x100000 | sin solape | 1:1 **OK** (16 bytes contrastados), SBL **OK**; el kernel en 0x5000 empieza en `0x6f` (`jal`), no en `0xE9` — ver más abajo | el bootloader OTA del punto 18 se pisa el ULP al crecer el kernel |
| `stk` | pico por hebra con `stack_peak()` | AVISO deliberado | 12 hebras; `[AVISO]` por diseño | ver "lo que no comprueba" abajo |

**Lo que se aprendió cableando `flash`, y que afecta al punto 18 (OTA):** la
expectativa original — encontrar `0xE9` en `0x5000` y en `0x100000` — era falsa.
`ld.script:26` declara `FLASH (rx) : ORIGIN = 0x40005000`, así que el kernel está
**enlazado para ejecutar en la ventana XIP** (VMA == LMA) y lo que hay en ese
offset es código, no la cabecera ESP; esa cabecera la consume el SBL al cargar y
no aparece tal cual en la ventana.  El ULP en `0x100000` tampoco empieza por
`0xE9` (empieza por `0x97`, un `lui`).  Lo que **sí** es cierto, y ahora es lo
que comprueba el test: la ventana cruda `0x40000000` es 1:1 con el offset de
fichero de la imagen, y el SBL en `0x2000` mantiene su magic.  Para el OTA eso
significa que la comprobación de "el kernel no pisa el ULP" hay que hacerla con
`_etext` vs `0x100000`, no buscando magos.

### Las referencias absolutas medidas

El kernel tiene cifras validadas en placa, y son las que hay que seguir
reproduciendo después de cualquier cambio (AGENTS.md §5).  A las de audio se le
suman las del reloj, que son las que cerraban el punto 1:

| cosa | teórico | medido | desvío |
|---|---|---|---|
| velocidad del core | 360 MHz (`P4_TIMER_FREQ`) | **319,977 MHz** | **−11,1 %** |
| frecuencia del `mtime` | 360 MHz | **320,010 MHz** (contra el PC) | **−11,1 %** |
| razón `mcycle/mtime` | — | **0,9999** | core y CLINT, mismo reloj |
| cuentas de `mtime` por ms de kernel | 360 000 | **402 818** | **+11,9 %** (0,13 ms de overhead por tick) |
| duración real de un segundo del kernel | 1,000 s | **1,26 s** por cuentas/ms / **1,28 s** por el PC | +26 % / +28 % |
| bucle de referencia | — | **5,34 ciclos/iter** | cifra de comparación entre builds |
| sincronismo del I2S (reg. 0x74, muestras de 16 bits) | 96 000 /s | **95 551 /s** | −0,47 % |
| captura de micrófono por `read()` | 192 000 B/s | **193 547 B/s** | +0,80 % |
| PMIC | — | 100 %, ≈4140 mV | lectura, no reloj |

Los dos de audio son ratios de reloj medidos con `clkcount()` contra trabajo
real: como ambos lados de la razón los da el mismo cristal, el desvío de −0,47 %
es el del propio I2S, **no** del `clkfreq` del kernel — lo que confirma que el
error del 11 % está en el número declarado y en el rearme del tick, y no en que
`clkcount()` mienta.  Con `mcycle` ya disponible, la comprobación cruzada deja
de necesitar el audio como sustituto.

Con esas cifras, **el punto 1 de los 21 queda cerrado**: qué MHz reales tiene el
core, y si `millis()` funciona (responder: *de forma sí —monótona, sin sierra,
sin ticks perdidos—; en valor no, avanza un 22 % lento contra el mundo*).

### Lo que `syscheck` no comprueba (para que nadie se confíe)

- **`stk` es un indicio, no una medida.** `stack_peak()`
  ([include/stack.h:19](../../include/stack.h#L19)) se muestrea en el cambio de
  contexto y **no ve la cadena profunda**: `tcpTimer` marcaba ~100 B con una pila
  de 65 KB. El dimensionado real se hizo con `-fstack-usage`
  (`esp_hosted_dispatch_rx` = 1664 B). El test lo imprime como `[AVISO]` a posta.
- **LCD y touch no se validan de verdad.** Se comprueba que el panel responde
  (`TOUCH_CTRL_TYPE`) y nada más: pintar una prueba en la pantalla dejaría el
  panel en un estado que no es el del `dash`.
- **No hay prueba de estrés.** El test del pool lee cuántos buffers quedan; no
  los agota. La prueba que dimensionó `NET_POOLSIZE` fue `ping` de 200 paquetes
  bajo carga, y eso sigue siendo un comando de shell aparte.
- **No hay FAT profundo.** `fs_mounted()` + abrir la raíz; no hay `df` (tarda y
  recorre la cadena FAT).
- **Escribe en el heap, pero por su allocator.** `sc_mem` escribe 64 palabras con
  `memget(256)`/`memfree` y 64 bytes en un bloque de PSRAM que luego libera.  No
  es una lectura pura: si el centinela de `.bss` existe, este test puede
  dispararlo.  **Esto fue un bug real y ya corregido**: la primera versión
  escribía 64 palabras **directamente en `memheap`**, y ahí vive la cabecera
  `struct memblock {next, length}` del primer bloque libre — corrompia el heap
  de la shell nada más lanzar `syscheck mem`.  Si alguien vuelve a tocar el
  test, que no escriba en `memheap` ni en `memlist`; por eso no existe `malloc`
  en este port y se usa `memget()`.

---

## 5. Está activado (2026-11-04) y esto es lo que hay que saber para usarlo

Los cuatro pasos del cableado, ya aplicados y en el build:

1. El fuente en `shell/xsh_syscheck.c` (adaptado desde `examples/`).
2. `C_FILES += xsh_syscheck.c` en `shell/Makerules`.
3. `{"syscheck", FALSE, xsh_syscheck},` en `commandtab[]`
   ([shell/shell.c:80](../../shell/shell.c#L80)) y el prototipo en
   [include/shell.h:154](../../include/shell.h#L154).
4. `make clean && make` — el build incremental no recompila a los dependientes al
   tocar un `.h` (`DEPFLAGS` está vacío).

**Qué cazó el paso a placa** (cosas que compilar no ve):

- El `printf` de este port **no soporta `%p`**: imprime el literal `"%p"`.  Hay
  que usar `0x%08x` con `(unsigned)(uintptr_t)`.
- Escribir en `memheap` corrompe el heap (§4, último punto).
- La SD no se abre: no hay VFS.  Es `control(sd, SD_CTRL_SEEK, 0, 0)` y luego
  `read(sd, blk, SD_BLOCK_SIZE)`, como en `shell/xsh_sd.c:194`, y la firma MBR
  está en `blk[510]=0x55`, `blk[511]=0xAA` (little-endian: al revés de como se
  lee el número en el texto).
- La expectativa del magic de `flash` era falsa (§4).
- El bucle de referencia con `millis()` daba 0 ms (§2).
- Las unidades del `sc_cpu`: la primera versión reportaba "999,990 MHz" por
  mezclar un ratio ×10⁴ con una conversión a MHz.

Orden recomendado para correrlo, de menor a mayor riesgo — el mismo que se
siguió al medir:

```
1  syscheck dev        solo lecturas; la regresión del AGENTS.md §4
2  syscheck mem        escribe en heaps propios (memget/psram_malloc)
3  syscheck clk        32 s; correlo por telnet y guarda la salida
4  syscheck net        lectura de tablas
5  syscheck flash      lectura de la ventana XIP
6  syscheck stk        lectura de pilas
7  p4clk.py            en paralelo con el paso 3
8  syscheck cpu        al último por si acaso: en esta placa NO paniqueó
```

Y dos efectos secundarios que se reprodujeron midiendo:

* `syscheck clk` por **serial** inunda la shell (32 s de salidas); por telnet es
  cómodo.
* `p4clk.py` **cierra el socket de golpe** al terminar, y eso deja al
  `telnetserver` sin aceptar conexiones hasta reset de la placa — es la tarea
  **#62**, reproducida en vivo durante esta medición.  Si tras medir la shell no
  responde, no es el script: es #62.  Se recupera con reset (abrir
  `/dev/ttyACM0` vale, porque resetea).

Las tres cifras del cierre, ya anotadas: **core 319,977 MHz**, **ratio
`mcycle/mtime` 0,9999**, **ratio contra el PC 0,78066 (−22 %)**.  Con eso
`P4_TIMER_FREQ` queda determinado como descripción del **tick mal configurada**,
no del núcleo, y afecta a cualquier cosa que dependa de ciclos: bit-banging,
umbrales de `busy-wait` y el presupuesto de CPU de LVGL si algún día se porta
(doc 02, §LCD).  La corrección propuesta de `clkfreq` está en
[05-roadmap-y-conf.md](05-roadmap-y-conf.md) §1 y **no se ha aplicado**.

---

## 6. Dónde encaja con los otros documentos

- Mapa de memoria y la regla de caché que usan `mem` y `flash`:
  [01-memoria-nucleos.md](01-memoria-nucleos.md).
- Cifras de audio y la ganancia del ADC que `dev` reproduce:
  [02-dispositivos.md](02-dispositivos.md).
- El pool de paquetes, su coste por buffer y la propuesta de `NET_POOLSIZE 96`:
  [05-roadmap-y-conf.md](05-roadmap-y-conf.md) §1.1.
- Las tareas abiertas que un chequeo así puede reabrir (#61 latencia en reposo,
  #62 telnet tras corte): [00-indice.md](00-indice.md).
