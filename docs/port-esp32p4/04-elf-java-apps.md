# 04 · ELF en runtime, librerías, busybox, JVM y el catálogo de apps

---

## 1. El cargador ELF que ya funciona (puntos 6 y 7)

Estado: **probado en placa** — 235 relocaciones resueltas, selftest 26/27 (`elfapp`).

Qué es de verdad: un **mini-enlazador en tiempo de ejecución** (estilo `elfrun`), no un
`execve`. Vive en [shell/xsh_elfapp.c](../../shell/xsh_elfapp.c).

### 1.1 Contrato estricto de lo que acepta

| aspecto | lo que admite | qué pasa si no |
|---|---|---|
| tipo de ELF | `ET_REL` (relocatable, `ld -r`) | `ET_EXEC`/`ET_DYN` → aborta |
| clase | `ELFCLASS32` (RV32) | 64 bits → aborta |
| máquina | `EM_RISCV` | otra → aborta |
| relocaciones | `R_RISCV_HI20`(26), `R_RISCV_LO12_I`(27), `R_RISCV_32`(1), `R_RISCV_CALL`(18), `R_RISCV_CALL_PLT`(19) | **cualquier otra aborta** con mensaje |
| secciones | `MAX_SECC = 48` (`xsh_elfapp.c:99`) | más → "sube MAX_SECC" |
| símbolos | `MAX_SIMB = 192` (`:103`) | más → "sube MAX_SIMB" |
| exports | `MAX_EXPORTS = 16`, 14 preenchidas (`:123`) | símbolo UND sin export → aborta |

Detalle que ya costó un fallo real: el índice 0 de `.symtab` es el símbolo nulo por convención
(`st_name=0`, `shndx=SHN_UNDEF`); si entra en el bucle, `resolve("")` devuelve `NULL` y el
cargador aborta con "símbolo SIN EXPORTAR:" y nombre vacío. Se empieza en `i=1`
(`xsh_elfapp.c:351`).

Dónde pone el código: **`psram_malloc()`** y copia **por la ventana directa `0x88……`**, que es
lo que garantiza que se ejecuta lo que hay en la celda (el porqué está medido en
[01-memoria-nucleos.md §4](01-memoria-nucleos.md#4-psram-y-la-caché-de-una-vez-por-todas-punto-9)).

### 1.2 La tabla `exports[]` de hoy (14 entradas)

```c
printf  kprintf  memset  memcpy  memmove  memcmp  strlen  strcmp  strncmp  strcpy
psram_malloc  psram_free  sleep  exit        /* exit -> eapp_exit(), guarda el código */
```

Son **punteros reales a funciones del kernel**, enlazados contra el árbol de `include/`. No
hay trampolines: en RV32 el `call` PC-relativo alcanza desde PSRAM a L2MEM (`xsh_elfapp.c:106-110`).

### 1.3 Cómo se construye un `.app` (el patrón de `examples/p4-elfapp`)

Obligatorio, y cada punto es una decisión que ya costó algo:

- **`-fno-pic`** → las direcciones absolutas quedan parcheadas en la instrucción (HI20 + LO12_I).
  Con `-fPIC` aparecen `R_RISCV_GOT20_I12` y `.got`; probado a mano en este proyecto: **petó**.
- **`-mno-relax`** → no aparecen `R_RISCV_RELAX`/`R_RISCV_ALIGN` y el par `auipc`+`jalr`
  conserva las dos palabras. Si GCC comprime una, parchear la vecina deja de ser correcto.
- **`ld -r`** (enlace parcial, sin libc) + `start.S` propio freestanding.
- Sin `.sdata`/`.sbss`: este GCC no los genera en la práctica, así que no hay que calcular
  `__global_pointer$` ni cargar `gp`. **Si algún día aparecen, es obligatorio** resolverlos
  antes de saltar.
- Con `-ffunction-sections`, cada función es su `.text.X` y su `.rela.text.X`: el selftest de
  10 funciones llega a 35 relocaciones.
- El ELF viaja **embebido como array `.rodata`** (`app_elf.h` generado por el `Makefile`), no
  se lee de la SD. Cambiar eso es el punto 2.1 abajo.

El `examples/p4-elfapp/README.md` tiene el detalle y la salida real de `make verify` (3 tipos
de relocación, 7 símbolos). El directorio **no está en `COMPS`** (`compile/Makefile:242`): no
entra en el kernel salvo que copies ficheros a `shell/` a propósito.

### 1.4 Lo que NO existe (para que no lo busques)

- `system/elfload.c` **no existe**; `shell/xsh_elf.c` es andamiaje muerto de otro intento.
- `system/sysent.c` tiene la `sys_call_table` **comentada** (`sysent.c:34-39`):
  `sys_call()` no despacha nada. Los ~90 números de `include/syscall.h` son **inalcanzables**
  hoy. Cualquier app ELF que hiciera `ecall` recibe el camino vacío y devuelve 0.
- No hay protección: un ELF puede escribir en cualquier MMIO, tumbar el kernel o liar el `wfi`.
- Un ELF no puede crear hebras ni llamar a la pila de red: no hay exports para eso, ni syscalls.

---

## 2. "Hacer librerías con `extern` las APIs" y "desacoplar la shell como un busybox" (punto 6)

### 2.1 Librerías compartidas para apps ELF — la ruta corta

Hoy cada ELF lleva sus dependencias dentro y las funciones del kernel las pide por nombre a
`exports[]`. Para "hacer librerías" hay dos modelos, y conviene elegir con cabeza:

**Modelo A — tabla de funciones (recomendado).** Un único vector de punteros expuesto por el
kernel, y las apps lo reciben como argumento o lo buscan una vez:

```
kernel:  struct api { int (*printf)(...); ssize_t (*read)(...); ... }  api_v1;   /* en .rodata */
app ELF: .globl p4api            /* NULL: la pone el cargador en args o en una reloc */
```

Ventajas: versión explícita (`api_v1`, `api_v2`), coste cero de enlace, y **no hay que tocar
`exports[]` cada vez** porque el `printf` de la librería es un campo de la tabla, no un nombre.
Es lo que hace un kernel real con su tabla de syscalls, pero sin `ecall`.

**Modelo B — `.so`-style con GOT.** Requiere `ET_DYN` + relocaciones `R_RISCV_GOT20_I12`
y `R_RISCV_PLT32`, es decir reescribir el cargador y pelear con el `-fPIC` que ya falló.
**Descartado para este hardware** hasta que haya una necesidad real de cargar código sin
recompilar el kernel por cada export nuevo.

En ambos modelos hace falta una **cabecera de SDK para apps**: `include/p4app.h` con los
prototipos y, en el cargador, un namespace de librería: `libgtk`-no, sino algo tipo
`libdisp` (framebuffer + touch), `libnet` (tcp/udp/dns wrappers), `libsd` (open/read/write ya
resueltos contra `FAT0`). Eso convierte las apps en "programa + `#include <p4app.h>`", que es
lo que preguntas cuando dices "para futuros programas".

Para que las apps sean algo más que un `printf`, hay que **rellenar `exports[]`** con lo
mínimo útil (quedan 2 huecos de 16; súbelo a 32-48). Candidatos, todos ya existentes como
funciones del kernel:

```
open close read write lseek control getdev mknod            (VFS: da acceso a SD/FAT/LCD/AUDIO)
create ready kill sleep recv send recvclr                   (hebras y mensajes)
tcpAlloc tcpAllocPassive tcpFree udpAlloc rawAlloc         (red cruda)
psram_malloc psram_free getmem freemem                      (memoria)
clkcount millis                                             (tiempo; ver 06-syscheck.md)
```

Y **un ABI de entrada**: hoy `app_elf.h`+`start.S` definen cómo entra. Documentarlo
(`int app_main(int argc, char **argv, const struct api *api)`) es lo que permite que existan
librerías reales entre apps.

### 2.2 La shell como busybox cargado

Qué costaría, de menos a más:

1. **Leer el ELF desde la SD en vez del array `.rodata`.** Un `psram_malloc` del tamaño del
   fichero + `read()` de `FAT0`. Es el paso que hace posible todo lo demás; 1-2 días.
2. **Un comando genérico**: `run /apps/ping.elf -c 5 8.8.8.8` que parsea argumentos, carga y
   salta. Sustituye la relación 1:1 `xsh_*` ⟷ comando.
3. **El `commandtab` poblado dinámicamente**: la shell actual recorre `commandtab[]`
   (`shell/shell.c:36-79`, `ncommand` en `:82`) con punteros compilados. Para busybox hace
   falta un nivel indirecto: una tabla de `{nombre, slot ELF o puntero}` y que `shell_main()`
   acepte entradas registradas en runtime (`register_command(nombre, elf_path)`).
4. **La shell entera como ELF** ("busybox"): aquí el problema no es cargarla, es **lo que la
   shell usa**. Hoy llama a `readline()` sobre la UART, al `tty` del console, a `printf`, y
   crea hebras (`create()`, `ready()`), y usa `control()` de devices. Si todo eso pasa por
   `exports[]`/la tabla de API, es factible; si no, la shell-ELF no puede ni leer una línea.
   **Recomendación**: no saques la shell del kernel. Saca los **programas**. El patrón
   útil aquí es: kernel + shell mínima residente (`help`, `ls`, `run`, `mount`, `ps`, `reboot`)
   y el resto como ELF en la SD. Es "busybox" en el sentido práctico, sin el riesgo de
   quedarte sin consola si un `load` falla.
5. **Firma/integridad antes de saltar**: CRC de la cabecera ELF + tamaño contra el hueco de
   PSRAM; hoy un ELF roto aborta con mensaje, lo cual es aceptable porque el único que carga
   ELF eres tú por consola.

Riesgos concretos que hay que nombrar: (a) un ELF con `R_RISCV_ALIGN` o `.sdata` hace abortar
el cargador **en la consola**, no hay aislamiento; (b) `MAX_SECC/MAX_SIMB` son techos duros;
(c) un app que no retorne deja el shell colgado hasta `kill`/reset; (d) sin PMP ni modo
`U`, no hay sandbox — un app con un puntero loco tumba el kernel.

---

## 3. JVM: portar la de `xinu-s3-amp` (punto 10)

### 3.1 Qué hay en el S3 (inventario real)

`/home/daniel/xinu-s3-amp/jvm/` — 12 ficheros `.c`, ~4.350 líneas en los principales:

| fichero | papel |
|---|---|
| `class_parser.c`, `constant_pool_parser.c`, `field_pool_parser.c`, `method_pool_parser.c`, `interface_pool_parser.c` | lectura de `.class` (formato de clase JVM) |
| `bytecodes.c` (71 KB) | el intérprete: el dispatch de opcodes |
| `heap.c`, `free_pool.c`, `stack.c` | montículo de objetos **por pools**, sin recolector |
| `jvm_run.c` | bucle de ejecución |
| `java_lib.c`, `java_std.c` | la "JCL": funciones nativas (`System`, `String`, `Math`) |
| `build-examples.sh`, `dist/` | cadena de compilación `.java → .class` |
| `shell/xsh_java.c` | `java <Class> [args]` → carga `/java/<Class>.class` desde FAT |
| `apps/java/` | **8 demos**: `ColorShow`, `DelayDemo`, `ImgDemo`, `LedBlink`, **`LvglDemo`**, **`Menu`**, `RgbDemo`, `WordQuest` |

Dos hechos que condicionan el port y tu plan de GUI:
- **Hay opcodes de `float`/`double`** implementados (23 referencias a `fadd/dadd/fload/…` en
  `bytecodes.c`), pero **el P4 tampoco tiene FPU** (RV32IMC en LP, RV32IMFC-like en HP sin FPU
  expuesta en este árbol): el float irá por software (subrutinas de compilación), con el coste
  que eso tiene en un intérprete.
- **No hay GC** (`heap.c` no tiene `collect`/`garbage`): `free_pool.c` gestiona pools de
  tamaño fijo. Eso es exactamente a lo que te refieres con "minimizar los errores por no tener GC".

### 3.2 Plan de port al P4, en etapas que se puedan validar

E1 — **copia y compile** (1-2 días): `jvm/` al árbol P4, `Makerules` nuevo, `xsh_java.c` y la
tabla de comandos. Sin peripherals: que `java Hello` imprima por `printf`. Riesgos: el S3 usa
`readline`/`open` de FAT y quizá tipos `long long` o inline-asm propios del S3; el paso a RV32
de P4 es casi transparente (misma anchura), pero la UART del S3 no es la del P4.

E2 — **JCL contra nuestros devices** (lo que llamas "rellenar javalib con nuestros
periféricos"). Sustituir/crear en `java_lib.c`:
```
java.lang.System      -> millis/clock, exit, arraycopy (ya casi todo)
p4.Lcd / p4.Touch     -> control(LCD0, LCD_CTRL_RECT|TEXT|BLIT) y read(TOUCH0)
p4.Audio              -> write(AUDIO0, pcm) + control(AUDIO_CTRL_*)
p4.Power              -> control(POWER0, POWER_CTRL_PERCENT|INFO)   (solo lectura)
p4.Net                -> tcp/udp por wrappers; ojo con NET_POOLSIZE (05-roadmap)
p4.Sd                 -> open/read/write sobre FAT0
```
Cada una es una función C estática con una firma `(objectref, args)`; es trabajo mecánico, no
de diseño.

E3 — **red de seguridad por la falta de GC** (esto es lo importante, y lo pides tú):
1. **Sin `malloc` sin tope**: el montículo del JVM se reserva con **un `psram_malloc` de tamaño
   fijo al arrancar** (propuesta: 4 MB de los 31,8 disponibles) y toda asignación de objeto sale
   de ahí con un puntero-bump por arena. Con bump-arena, "no haber GC" se convierte en
   "el `run` de una app tiene presupuesto de objetos".
2. **Límites duros**: nº máximo de objetos, profundidad máxima del `stack.c`, y tamaño máximo
   de array (un `newintarray[100000000]` sin comprobación es un panic).
3. **Fin de app = liberar la arena**: al volver de `jvm_run()`, `psram_free()` del bloque único.
   Con eso el no-GC pasa de "fuga lenta" a "no hay fragmentación ni fuga entre apps".
4. **`try/catch` mínimo**: el error más común sin GC no es la fuga, es el `NullPointerException`
   y el `ArrayIndexOutOfBounds` mal resueltos. Implementar que el intérprete devuelva error en
   vez de saltar a `0x0` cuesta 3 comprobaciones en `bytecodes.c` (referencias nulas, límites de
   array, división por cero) y es lo que más "minimiza los errores" de verdad.
5. **Apps aisladas**: cada app `.class` corre en su hebra con su propia pila Xinu de 8 KB+ y su
   arena; que el launcher la `kill` si excede un tiempo o un presupuesto.

E4 — **launcher de `.class`** (lo que describes): un directorio `/java` en la SD, un índice
(`/java/apps.idx` o leer el FAT), un `Menu` estilo el del S3 (que ya existe en `apps/java/Menu`)
pintado en el LCD con `ImgDemo`/`LvglDemo` como referencia, `touch` como entrada y
`java <Class>` como ejecución. Eso es un launcher, no un SO: el resto (memoria, red, audio) ya
está en E2.

### 3.3 Java o ELF en runtime: la indecisión (punto 12)

No son rivales; son capas distintas. Mi recomendación, con el criterio "qué quieres poder
actualizar sin recompilar el kernel":

| | ELF cargado | JVM + `.class` |
|---|---|---|
| velocidad | nativa, ~sin coste | intérprete: 1-2 órdenes de magnitud más lento |
| RAM por app | el propio ELF (KB) | ELF del JVM (fijo) + arena de objetos (MB) |
| seguridad | **ninguna**: un mal puntero tumba el kernel | mucho más difícil de liar (todo va por el intérprete; hay que comprobar índices y nulos) |
| que pueda escribirlo alguien | necesita toolchain RISC-V y las reglas de `-fno-pic`/`-mno-relax` | `javac` y un `.class` en la SD |
| GUI completa | tocarías C cada vez | es lo que el launcher necesita |
| acceso a hardware | `exports[]`, rápido de ampliar | JCL, más trabajo por API |
| dependencias/terceros | librerías C portadas a mano | cualquier `.class` que no use `java.*` raro |

**Decisión que propongo**: los **DOS, pero cada uno para lo suyo**.
- **ELF** = el "sistema" y lo que va apretado de rendimiento: servidores nuevos, el httpd con
  ficheros, el decoder MP3 (punto 17), un experimento de LVGL con float, y los drivers que no
  quieras dentro del kernel todavía.
- **JVM** = las **apps de usuario y la GUI**, que es justo lo que quieres ("la idea de usar Java
  es hacer todo el sistema GUI con Java"). El launcher es una app Java.
- Punto de enganche: `exports[]`/la tabla de API es la frontera única. La JVM la usa para llegar
  al LCD/audio/red; un ELF también. Si mantienes esa frontera pequeña y versionada, no pagas
  dos veces el trabajo.

Y un orden razonable: primero `exports[]` ampliado + carga desde SD (E1/E2 de §2), porque eso
abarata todo lo demás, incluida la JVM.

---

## 4. Catálogo de apps (punto 11)

Analizado uno por uno contra lo que **hay** en este árbol. "Fácil" = se hace con lo que ya
está; "medio" = hay que añadir una pieza al kernel; "no se puede" = no hay soporte y no es
cuestión de escribir app.

| app | viabilidad | qué hace falta |
|---|---|---|
| gestor de archivos (SD) | **fácil** | FAT + `ls`/`stat`/`cp`/`mv` ya existen; falta UI de scroll. JVM o LCD+touch en C |
| notas | **fácil** | ficheros en FAT; teclado en pantalla o **teclado USB** (ver el problema de USB, punto 14) |
| configuración del sistema | **fácil** | `power`/`ifconfig`/`wifi` ya responden; falta persistencia (escribir un `/etc/xinu.cfg` en FAT) |
| conversores de valores | **fácil** | aritmética entera; ojo con el float en el kernel (regla: sin coma flotante residente) |
| música / reproductor | **medio** | PCM por `write(AUDIO0)` funciona; hace falta decoder (MP3 → punto 17, WAV → hecho) |
| galería de imágenes | **medio-difícil** | no hay decoder JPEG/PNG en el árbol; el P4 tiene **acelerador JPEG** (`0x50086000`, solo cabeceras HAL). Sin decodificador, nada de galería salvo BMP crudo |
| juegos (flappy, tetris) | **fácil con framebuffer** | el paso previo es el framebuffer de [02 §3](02-dispositivos.md); hoy el SPI directo no da para 30 FPS, pero para tetris sí |
| herramientas de electrónica (frecencímetro, osciloscopio simple, calculadora de resistencias) | **fácil/medio** | el `audio BCLK` ya mide Hz por registro; una entrada analógica real no existe en esta placa (el P4 **sí** tiene ADC: `SOC_ADC_CHANNEL_NUM(0)=8` y `(1)=6`, más 2 comparadores analógicos (`SOC_ANA_CMPR_NUM=2`) en `core/soc/esp32p4/include/soc/soc_caps.h:131,300` del SDK bare-metal (`adc_periph.c`, `hal/adc_ll.h`, `regi2c_saradc.h`) — no hay nada portado en este árbol, ni en `include/esp32p4.h`) |
| herramientas de red: cliente/servidor telnet/UDP/TCP, ping, escáner de puertos | **fácil** | toda la pila está arriba; `xsh_nc` del S3 portado da el cliente interactivo |
| mapa de redes / "nmap" ligero | **medio** | `wifi scan` da las APs; lo demás es ARP sweep + SYN/UDP probes desde la pila — **legal y técnico**: solo contra redes tuyas |
| escáner QR | **difícil** | no hay cámara (ver punto 15) y no hay decodificador QR; se puede portar `zbar`-lite o `quirc` (C, entero) si hay imagen. `quirc` es la opción razonable |
| "app que consulte mi Google y traiga todas mis claves guardadas" | **no la hago así, y hay motivo técnico además del de seguridad** | el dispositivo no tiene pila TLS (no hay mbedTLS/portaria OpenSSL en el árbol; `xsh_curl` es HTTP plano), y **la API de Google no expone las contraseñas guardadas del navegador** ni con OAuth. Lo que sí se puede: un **almacén de credenciales propias** cifrado en la SD, con el secreto derivado de algo que tú tecleas en la placa, y sincronización por un endpoint propio. Diseñada así (tus claves, tu servidor) es una app perfectamente razonable y la documento en [05](05-roadmap-y-conf.md#credenciales) |
| "vulnerar redes para aprender" | **no se puede con este hardware en este port** | esp_hosted no expone monitor/promiscuo/sniffer/deauth/inyección: el transporte mueve **tramas Ethernet** y punto (barrido del SDK en §6.5 de [03](03-red-y-wifi.md)). Alternativa formativa real: escáner de red por ARP/TCP + análisis de tráfico **de tu propia** AP, con la pila que sí hay |

Para casi todo el catálogo: **JVM + launcher** es el contenedor correcto, y las tres APIs que
hay que construir primero son `p4.Lcd/Touch`, `p4.Sd` y `p4.Net` (§3.2 E2).
