# syscheck: a que velocidad esta el CPU y si `millis()` funciona

El codigo **ya no vive aqui**: desde el 2026-11-04 esta cableado en
`shell/xsh_syscheck.c` y compila dentro de `compile/xinu.elf` como el comando
`syscheck` de la shell.  Este directorio conserva el PC-side (`p4clk.py`) y esta
nota con las cifras medidas.

Contesta a las dos preguntas que no se podian contestar mirando el codigo:

1. **¿A que velocidad esta realmente el core?**  El port declara
   `platform.clkfreq = P4_TIMER_FREQ = 360 MHz` (`include/esp32p4.h:160`,
   asignado en `system/platforms/esp32p4/platforminit.c:39`), pero eso era una
   suposicion de arranque sobre el reloj de la CLINT, no una medicion del reloj
   del nucleo.
2. **¿Funciona `millis()`?**  Tiene historia: antes se derivaba de `clkcount()`
   y era un diente de sierra de 11,93 s (`system/timershim.c:29-40`).  Ahora
   viene de `clktime`/`clkticks`, pero eso solo es correcto si `clkhandler()` no
   pierde ticks.

Las dos preguntas estan **respondidas y medidas en placa** el 2026-11-04, y
ninguna de las dos respuestas favorece al 360 MHz.

## Que hay aqui

| fichero | que es |
|---|---|
| `p4clk.py` | el PC como unica referencia ABSOLUTA de tiempo: mide el latido que suelta `syscheck clk` y da ratio, ppm, deriva s/hora y los MHz reales del mtime. |
| `README.md` | esto: las cifras medidas y como leerlas. |

El fuente del comando esta en `shell/xsh_syscheck.c`; el cableado son las
cuatro lineas de `shell/Makerules` (`C_FILES += xsh_syscheck.c`),
`shell/shell.c:80` y `include/shell.h:154`.

## Los subcomandos

```
syscheck clk     millis() monotono, cuentas de mtime por ms, micros(), latido HB
syscheck cpu     mcycle vs mtime -> MHz REAL, y bucle de referencia comparable
syscheck mem     heap L2MEM, heap PSRAM, patron de celdas, alias 0x48/0x88, banda fantasma
syscheck net     netif, buffers LIBRES del pool, ARP, rutas, contadores espeth
syscheck dev     I2C probe, PMIC, codec, touch, LBA0 de la SD, FAT montada
syscheck int     que hace falta para auditar el CLIC (remite a psram-blob/xsh_irqmap.c)
syscheck flash   ventana XIP 1:1, magic del SBL y el fin del kernel vs 0x100000
syscheck stk     pico de pila por hebra (con el aviso de lo que NO ve)
syscheck all     todos menos `cpu`
```

Cada test imprime `[ OK ]`, `[AVISO]` o `[FALLO]` y el resumen final devuelve
`0` si no hay fallos, `1` en cualquier otro caso, para que sirva en un script.

## Las cifras medidas (2026-11-04, red del trabajo, placa en 172.16.0.117)

| medida | valor | de donde sale |
|---|---|---|
| razon `mcycle / mtime` | **0,9999** | `syscheck cpu` |
| velocidad REAL del core | **319,977 MHz** | la razon anterior por los MHz del mtime |
| frecuencia REAL del mtime | **320,010 MHz** | `p4clk.py`, contra el reloj del PC |
| `clkfreq` declarado | 360 000 000 Hz | `include/esp32p4.h:160` |
| error de lo declarado | **-11,1 %** (360/320,010) | |
| cuentas de mtime por ms de kernel | **402 818** (no 360 000) | `syscheck clk` |
| duracion real de un segundo del kernel | **1,26 s** | 29 000 ms de kernel en 37 148 ms de PC |
| ratio kernel/PC | **0,78066** | `p4clk.py` |
| bucle de referencia | 5,34 ciclos/iter (200 000 iters en 3 338 us) | `syscheck cpu` |
| `syscheck all` | **23 tests, 19 OK, 4 avisos, 0 fallos** | |

### Los tres hallazgos

**1. `P4_TIMER_FREQ = 360000000` es falso.**  El mtime va a 320,010 MHz
medidos contra el PC, y el core va 1:1 con el mtime (`mcycle/mtime = 0,9999`).
O sea: el cristal es de 40 MHz y el divisor de la CLINT esta en 8, no en 9.
Consecuencia practica: todo lo que dependa de `platform.clkfreq` esta hinchado
un 12,5 % - el calculo del tick, y cualquier tiempo derivado de el.  **No se ha
corregido**: tocar `clkfreq` mueve el tick del kernel y no esta autorizado.

**2. `clkhandler()` pierde overhead en cada tick.**  El tick se programa a
`clkfreq/1000 = 360 000` cuentas de mtime, pero entre latido y latido real
pasan 402 818.  Esos 42 818 de diferencia (0,13 ms) es el tiempo que se pierde
entre que salta la interrupcion y se rearma el comparator.  Por eso un segundo
del kernel dura 1,26 s reales: `millis()` avanza 1000 ms por cada 1258 ms de
reloj.  `millis()` sigue siendo **monotona y sin sierra** (eso comprueba el
test y esta OK); lo que no es es exacta.

**3. `micros()` esta hinchado en la misma proporcion.**  El intervalo de 100 ms
de `sleep` midio 111 533 us, no 100 000 - exactamente el factor 1,11 de la
brecha entre 360 000 y 320 010 cuentas por ms.  `micros()` es la palabra baja
del mtime dividida por un `clkfreq` que no es el suyo.

## Lo que mide cada uno, y contra que

**clk.** `millis()` se compara contra `clkcount()` (`system/clkhandler.c`), que
es mtime puro.  Son dos caminos distintos al mismo cristal: `millis()` va por
`clkhandler()` -> `clktime`/`clkticks` (una interrupcion por ms), `clkcount()`
lee el contador directo.  Si la interrupcion se pierde, la razon media de
cuentas de mtime por ms deja de valer lo declarado.  Es el test que dio el
402 818.

**cpu.** `mcycle`/`mcycleh` son los CSRs `0xB00`/`0xB80`; el SDK bare-metal los
usa como contador de ciclos del P4
(`/home/daniel/esp32-riscv-bare-metal-sdk-main/core/riscv/include/riscv/rv_utils.h:95`).
La lectura va en `do/while` con relectura de la palabra alta, porque en RV32 el
bajo envuelve.  El numero que salio es la **razon 0,9999**: core y CLINT van al
mismo reloj, asi que la unica incognita era *cual* reloj, y esa la resuelve
`p4clk.py` (320,010 MHz).

**mem.** El test del alias escribe por `0x88......` (acceso directo,
`PSRAM_CPU_UNCACHED`, `device/psram/psramInit.c:36`) y lee por `0x48......`
(con cache).  No juzga: **documenta**.  Medido: se escribe `0xC0FFEE11` por el
alias y por `0x48` se lee `0xaaafeeab` - la linea sigue sucia en L2MEM, que es
exactamente lo que rompio el blob de PSRAM y lo que obligo a `.l2reserve`.  El
test de patron usa `memget(256)`/`memfree`, **no** `memheap` directamente: ahi
vive la cabecera del primer bloque libre y escribirla corrompe el heap (error
que se cometo en la primera version y se corrigio).  La prueba de la banda
fantasma comprueba que el enlistado sigue respetando el corte `0x4FF20000`.

**net.** El numero que interesa es `semcount(bfptab[netpool].freebuf)`.  Medido
con la pila en reposo: **23 de 24 libres**; durante un `ping` sostenido baja a
**16 de 24**.  Con `NET_POOLSIZE 24` y un buffer de 1632 B eso es un margen de
8 buffers, y explica por que la propuesta del documento 05 es subirlo a 96.
Con la red del trabajo arriba, los 4 tests de `net` salieron OK y `drops 0`.

**dev.** Solo lecturas.  El probe del I2C va a 0x18, 0x34 y 0x38 y **no toca
0x36** (aparcado por decision, AGENTS.md seccion 4).  Del PMIC se lee
`POWER_CTRL_PERCENT` y nada mas: escribir 0x10 (RWAC) o 0x80 tumba la placa.
La SD se lee con `control(sd, SD_CTRL_SEEK, 0, 0)` y `read()` sin `open()` (no
existe VFS en este port), y la firma MBR se compara en orden little-endian:
`blk[510]==0x55`, `blk[511]==0xAA`.  Medido: LBA0 leido y firma correcta.

**flash.** Primera version buscaba `0xE9` en `0x5000` y `0x100000` y fallaba, y
el por que es el hallazgo: `ld.script:26` pone `FLASH (rx) : ORIGIN =
0x40005000`, o sea que el kernel esta **enlazado para ejecutar en la ventana
XIP** (VMA == LMA) y la cabecera ESP que veria un `esptool` la consume el SBL al
cargar.  En `0x5000` el primer byte es `0x6f` (`jal`), que es codigo, no magic.
El test ahora comprueba lo que si es cierto: que la ventana cruda `0x40000000`
es **1:1 con el offset de fichero** (16 bytes contrastados) y que el SBL en
`0x2000` si empieza por `0xE9`.  Los dos salieron OK.

**stk.** `stack_peak()` se muestrea en el cambio de contexto y **no ve la cadena
profunda** (`tcpTimer` marcaba 100 B con 65 KB de pila).  Medido: 12 hebras.  El
pico es un indicio, no una medida; el dimensionado real se hizo con
`-fstack-usage`.

**int.** Sigue siendo un stub: lo que hay que auditar es el CLIC y para eso hace
falta portar `examples/psram-blob/xsh_irqmap.c`.  Es uno de los 4 avisos.

## Como se activo (ya hecho)

Cuatro puntos y un `make clean`:

1. `shell/xsh_syscheck.c` (el fuente, adaptado: el printf de este port **no
   soporta `%p`**, hay que usar `0x%08x` con `(unsigned)(uintptr_t)`).
2. `C_FILES += xsh_syscheck.c` en `shell/Makerules`.
3. Fila `{"syscheck", FALSE, xsh_syscheck},` en `commandtab[]` (`shell/shell.c`)
   y prototipo `shellcmd xsh_syscheck(int, char *[]);` en `include/shell.h`.
4. `make clean && make` (el build incremental no recompila a los dependientes
   cuando tocas un `.h`: `DEPFLAGS` esta vacio).

## Peligros que hay que saber

* `syscheck cpu` **puede panicar** si el core no implementa `mcycle`; en la
  placa si lo implementa y no trapo.  Aun asi `all` no lo llama.
* `syscheck clk` tarda 2 s + 30 s (los latidos).  Por serial eso inunda la
  shell; por telnet es lo comodo.
* `p4clk.py` **cierra el socket de golpe al terminar**, y eso reproduce la
  tarea #62: el `telnetserver` se queda sin aceptar conexiones hasta reset de la
  placa.  Si despues de medir la shell no responde, no es el script - es #62.

## p4clk.py

```
/home/daniel/esp-idf-py/bin/python examples/syscheck/p4clk.py 172.16.0.117 30
```

(El `python3` del sistema no tiene `pyserial`; solo el del entorno de idf.)
Conecta al telnet de la placa (puerto 23), manda `syscheck clk`, captura las
lineas `HB nnn xinums=.. uptime=.. mtime=..` y saca ratio, ppm, deriva en
s/hora y los **MHz absolutos del mtime contra el PC**.  **No abre
`/dev/ttyACM0`**, asi que no resetea la placa: es la unica forma de medir sin
perder la asociacion WiFi.

Metodo, para que el numero no se lea mal: el latido lo produce `sleep(1000)`, o
sea el propio reloj que se quiere medir, y lo que se compara es la **pendiente**
de `xinums` contra el reloj del PC a lo largo de 30 latidos.  El primer retardo
lleva la conexion y el arranque del comando, por eso se descarta; los saltos de
mas de 1500 ms se cuentan aparte porque son colas de la shell imprimiendo, no
reloj.  Con 1 ms de resolucion contra jitter de red WiFi, un desvio por debajo
de ~200 ppm no es interpretable: lo que se busca es una orden de magnitud, no un
calibrado de cristal.

El bug que hubo que arreglar: la primera version temporalizaba todas las lineas
de una misma lectura `recv()` en el mismo instante, lo que dio un ratio absurdo
de 105278.  Ahora cada linea completa lleva el `time.monotonic()` de la trama
en la que llego (`_drain` guarda `(ln, stamp)`), y con eso el ratio salio
0,78066 y los MHz del mtime 320,010.

Ojo con `mtime=%lu`: es la palabra **baja** del contador de 64 bits y envuelve
cada 2^32/320,010e6 = 13,4 s.  Como el latido va a ~1 s, cada paso cabe de
sobra en una vuelta y la resta modular por paso es correcta; lo que NO vale es
restar el primero del ultimo.

## Relacion con los documentos

Esto es la parte operativa del punto 1 de los 21, y esa pregunta esta cerrada
con cifras.  La explicacion de como leerlas esta en
`docs/port-esp32p4/06-syscheck.md`; el mapa de memoria que usan los tests `mem`
y `flash` en `docs/port-esp32p4/01-memoria-nucleos.md`; y la correccion
propuesta de `P4_TIMER_FREQ` (sin aplicar) en `docs/port-esp32p4/05-roadmap-y-conf.md`.
