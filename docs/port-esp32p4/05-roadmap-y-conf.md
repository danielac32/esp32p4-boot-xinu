# 05 · Hoja de ruta, puertos pendientes y revisión de recursos

Todo lo de este fichero es **propuesta**. Nada se ha ejecutado: el enunciado de esta fase era
"no modifiques el código". Cuando quieras una etapa, dime cuál y la cableo con su validación
en placa.

---

## 1. Revisión de recursos (`conf.c` / `xinu.conf` / `conf.h`) (punto 16)

Recordatorio de cómo funciona: `system/conf.c` e `include/conf.h` son **generados** por
`compile/config/config` desde `compile/platforms/esp32p4/xinu.conf`. Editar los generados no
sirve: cualquier `make` los borra. **Solo se toca `xinu.conf`** (y `include/network.h`,
`include/arp.h`, etc. para las constantes que no viven en el conf).

### 1.0 El reloj declarado está un 11 % arriba, y el tick pierde 0,13 ms {#clkfreq}

**Ya medido** (`syscheck cpu` + `syscheck clk` + `p4clk.py`, 2026-11-04; documento
[06-syscheck.md](06-syscheck.md)).  Son dos defectos distintos y hay que corregirlos
en dos pasos distintos:

| | declarado / programado | medido | desvío |
|---|---|---|---|
| frecuencia del `mtime` (`P4_TIMER_FREQ`, `include/esp32p4.h:160`) | 360 000 000 Hz | **320 010 000 Hz** | **−11,1 %** |
| velocidad del core (`mcycle/mtime`) | se suponía 360 MHz | **319,977 MHz** (1:1 con el `mtime`) | −11,1 % |
| cuentas de `mtime` por ms de kernel | 360 000 | **402 818** | +11,9 % |
| duración real de 1 s del kernel | 1,000 s | **1,26 s** | +26 % |

**Corrección 1 — el valor.**  `P4_TIMER_FREQ` (y con él `platform.clkfreq` en
`system/platforms/esp32p4/platforminit.c:39`, `CLKTICKS_PER_SEC`, `udelay()` y
`micros()`) debe pasar de `360000000` a `320010000`.  Es una constante, pero **no
es una corrección cosmética**: el periodo del tick se calcula como `clkfreq/1000`,
así que hoy se programan 360 000 cuentas cuando 1 ms real son 320 010.  Al
arreglarlo, `sleep(1000)` pasará de durar 1,26 s a durar 1 s **de verdad**, y todo
temporizador de la pila (`TCP_FREQ`, `TIME` en `tcpTick`, el timeout de ARP, el
retransmiter) se acelerará un factor ~1,26 con el mismo valor de macro.  Eso
toca el comportamiento de red validado en la fase 1 (200/200 de ping, HTTP a 16 ms
de mediana), así que **no se aplica sin autorización** y se valida con la
regresión de AGENTS §4 y un `ping -c 200` después.

Verificación de que el número es ese y no otro: el cristal del P4 es de 40 MHz, y
40 × 8 = 320.  El 360 del port sale de suponer un divisor de 9.  Con 320 010 000,
el `+10` por millón es la tolerancia del cristal, coherente con el −0,47 % que ya
se medía en el sincronismo del I2S.

**Corrección 2 — el overhead del handler.**  Una vez arreglado el valor, quedan
**~42 800 cuentas por ms que no son del reloj sino del coste de la interrupción**
(entrada, `clkintr()`, rearme absoluto desde el valor anterior en vez de
`mtime + periodo`).  Con `clkfreq` correcto, eso son **0,134 ms por tick → 134 ms
de deriva por segundo → ~8 minutos por hora**.  La corrección es rearmar el
comparador con `mtime_comparer += CLKTICKS_PER_SEC` (rearme relativo) en lugar de
posarlo desde `clkcount()` leído en el handler, que es lo que hereda el retraso de
la máscara de interrupciones.  **No se ha tocado**: es código de kernel
(`system/clkhandler.c`), no una constante de conf, y necesita su propia etapa con
medición antes/después con `p4clk.py`.

**Riesgo de no hacerlo**: cualquier cosa expressada en "ms" en este port es un
11 % optimista hoy, y un 26 % a nivel de `sleep()`.  El bit-banging, los
`udelay()` de arranque de dispositivos y los timeouts de red están todos
afectados; y el presupuesto de CPU del LVGL, si algún día se porta, está calculado
sobre 360 MHz que no existen.

### 1.1 El desajuste gordo: el pool de paquetes {#pool}

```c
/* include/network.h:144-147 */
#define NET_MAX_PKTLEN  1598
/* "Poolsize should be >= ARP_NQUEUE + RT_NQUEUE + UDP_IBLEN + RAW_IBLEN" */
#define NET_POOLSIZE    24
```

Y los consumidores reales, medidos en el árbol:

| macro | valor | dónde |
|---|---|---|
| `ARP_NQUEUE` | **32** | `include/arp.h:69` |
| `RT_NQUEUE` | **32** | `include/route.h:37` |
| `RAW_IBLEN` | **100** | `include/raw.h:34` |
| **suma exigida** | **≥ 164** | comentario de `network.h:145` |
| `NET_POOLSIZE` | **24** | `network/net/netInit.c:41-42` (`bfpalloc(NET_MAX_PKTLEN + sizeof(struct packet), 24)`) |

Consecuencia observable: en cuanto hay tráfico ICMP (que es lo que hace `ping`, y también lo
que contesta el kernel cuando te pingeana ti) más colas ARP/ruta llenas, `netGetbuf()` se
queda sin buffers y la pila **descarta en silencio**. Es exactamente el tipo de síntoma que
se confunde con "el WiFi va mal". Con `ping -i 2` y dos `netboot` seguidos es fácil llegar.

Opciones, con su coste real (cada buffer = `roundword(1598) + sizeof(struct poolbuf)` ≈ **1.616 B**,
`system/bfpalloc.c:34`):

| opción | resultado | RAM de L2MEM | comentario |
|---|---|---|---|
| A. `NET_POOLSIZE` 24 → **96** | 96 buffers | **155 KB** (antes 39 KB) | deja 293 KB de heap L2MEM de los 448 KB actuales. Cubre ARP+RT+una carga razonable de RAW pero sigue sin llegar a 164 |
| B. `NET_POOLSIZE` → **164** | cumple el comentario | **265 KB** | heap L2MEM baja a ~183 KB: a 100 hebras de 8 KB de pila ya no llegas |
| C. `NET_POOLSIZE` 96 **y** `RAW_IBLEN` 100 → 32 | coherente y barato | **155 KB** | 32 paquetes crudos en cola por PCB es más que suficiente para `ping`; recomendada |
| D. Buffers en PSRAM | barata en RAM rápida | 0 | hay que respetar la regla de caché (`0x88` vs `0x48`) y el DMA no ve PSRAM por encima de `0x4FFC0000`… que sí la ve; pero la pila entera asume buffers en RAM rápida. **No la recomiendo ahora** |

**Propuesta concreta**: opción **C** (`NET_POOLSIZE` = 96 + `RAW_IBLEN` = 32), y validar con
`netstat` (impresiona el estado del pool) antes y después de un `ping -i 2 -c 200` mientras
`httpd` da servicio.

**Medido con `syscheck net` (2026-11-04)**: el pool **no está hoy en el punto de
rotura que este análisis sugería**, y hay que decirlo para no sobredimensionar.
En reposo quedan **23 buffers libres de 24**; con un `ping` sostenido baja a
**16 de 24**, y `rx_drops` se quedó en **0**.  Es decir: para el uso actual
(`ping`, `telnet`, `httpd` de una página) los 24 buffers aguantan con margen, y
el escenario de 164 exigidos por el comentario no se ha dado porque las colas
ARP/RAW no se llenan con ese tráfico.  La propuesta C sigue en pie por otra
razón: en cuanto se porta `udpserver` (#56) o `httpd` con ficheros (#57) y
concurren varios clientes, 8 buffers de colchón son pocos.  El coste real por
buffer medido es **1632 B**, y el cálculo lo reproduce: `bfpalloc` hace
`roundword(NET_MAX_PKTLEN + sizeof(struct packet)) + sizeof(struct poolbuf)` =
`1624 + 8` ([include/bufpool.h:49](../../include/bufpool.h#L49),
`system/bfpalloc.c:34`).  Los 1616 B de la estimación de arriba se olvidaron del
`struct packet`; la opción C pasa de 155 KB a **~157 KB**.

### 1.2 Macros duplicadas que mienten

`include/conf.h` tiene **dos juegos** de las mismas constantes, y solo uno es el que usa el
generador:

| | `conf.h:85-88` | `conf.h:147-150` |
|---|---|---|
| | `NRAW 2` `NUDP 2` `NTCP 10` `NTELNET 4` | `NUDP 2` `NTCP 10` `NRAW 2` `NTELNET 4` |

( más `NUART 1` en `:71` y `:132`, y `NNETIF 1` en `:146`). Vienen de la copia literal de
`xinu.conf:276-279`. Hoy **coinciden**, así que no hay bug — pero si alguien sube `NTCP` en
un sitio y no en el otro, el `devtab` y las tablas de control de bloques quedan en tamaños
distintos, que es el tipo de fallo que se manifiesta como corrupción lejana. **Propuesta**:
que `xinu.conf` declare cada una **una sola vez** (con `#ifndef` en la copia) y regenerar.

### 1.3 `FLASH LENGTH = 31M` frente a 16 MB físicos

`compile/platforms/esp32p4/ld.script:26` declara 31 MB de flash; la placa tiene 16 MB
(`--flash-size 16MB`, `platformVars:170`). El enlazado no avisará si el kernel pasa de 16 MB:
petaría en el flasheo o, peor, en tiempo de arranque. **Propuesta**: `LENGTH = 15M` (deja el
MB que ocupa el ULP y su hueco) para que el `ld` corte. El `RAM(rwx) LENGTH = 31M` sí es
correcto: reserva el último MB para la RAM del ULP (ver
[01 §2](01-memoria-nucleos.md#dónde-se-solapan-las-cosas-lo-que-pregunta-el-punto-20)).

### 1.4 DHCP no renueva nunca

```c
/* network/dhcpc/dhcp.h:28-29 */
#define DHCPC_STATE_RENEW   4
#define DHCPC_STATE_REBIND  5
```
Esos dos estados están **definidos y no se usan en ningún sitio** del árbol (barrido de
`RENEW|REBIND` sobre `network/`, `device/`, `include/` → solo las dos líneas de arriba). El
cliente hace `INIT → SELECTING → BOUND` y se queda ahí. Con una lease corta (algunos routers
dan 1-2 h) la IP del P4 expira en el servidor; en la práctica funciona hasta que el router
reasigna esa IP a otro aparato. **Propuesta**: hebra/`sleep` que, al 50 % de la lease, mande
`DHCPREQUEST` al servidor original (estado RENEW) y, si no hay respuesta al 87,5 %, haga
`DHCPDISCOVER` (REBIND). Necesita guardar `lease`, `renewal` y `rebinding` de las opciones 51/58/59,
que hoy se descartan.

### 1.5 DNS sin configuración

`shell/xsh_dns.c:43-46` fija **8.8.8.8** como servidor y no hay `resolvtab` en el kernel. Dos
consecuencias: no funciona en redes que solo dejan salir DNS a su propio resolver, y DHCP no
puede decirte qué resolver usar (la opción 6 se ignora). **Propuesta**: tabla de resolutores
pequeña (`NRESOLV 2`) + `dns -s <ip>` persistente en la sesión + leer la opción 6 del DHCP.

### 1.6 Comandos existentes sin registrar

`xsh_curl.c`, `xsh_wget.c`, `xsh_ip.c` están compilados en el árbol pero **no** están en la
`commandtab` (`shell/shell.c:36-79`). Tres líneas en `shell.c`, tres prototipos en
`include/shell.h`. Es la victoria más barata de toda la lista (te da `curl` y `ip` en la
consola sin escribir funcionalidad).

### 1.7 Otros números, revisados y **sin necesidad de cambio**

| macro | valor | veredicto |
|---|---|---|
| `NTHREAD` | 100 | Techado real por memoria, no por tabla: 100 hebras × `MINSTK 4 KB` = 400 KB de heap L2MEM sobre 448 KB. Si subes el pool (§1.1), baja `NTHREAD` a 64 o sube las pilas a demanda. La tabla en sí es barata |
| `NSEM 100`, `NMON 20`, `NMUTEX 20`, `NMQUEUE 20` | | suficiente; `sdmmcBusLock()` es un monitor y usa una ranura de `NMON` |
| `NMAILBOX 10` | | **obligatorio > 0** con la pila: `arpInit`/`icmpInit`/`rtInit` llaman a `mailboxAlloc()` y `mailboxInit()` solo se invoca bajo `#if NMAILBOX` (comentario en `conf.h:120-123`) |
| `NTCP 10` | | justo si haces un `httpd` con varios clientes concurrentes (cada conexión de un navegador típico abre 2-6 sockets) + telnet + `curl`. **Propuesta**: 16-24 |
| `NTELNET 4` | | correcto (4 sesiones) |
| `NET_NTHR 1` | | una hebra de recepción; con el candado SDHOST compartido, más hebras no dan más throughput. Dejar 1 |
| `NPOOL 8`, `POOL_MAX_BUFSIZE 2048` | | compatible con `NET_MAX_PKTLEN 1598` |
| `ESPETH_RX_SLOTS 8` | | subido esta fase; cubre la cola de 8 del transporte |
| `TCP_TIMER_STK` / `NET_RECV_STK` = 8192 | | añadidos con `-fstack-usage` (no con el peak de `ps`) |

### 1.8 Resumen de la propuesta de recursos (en orden de riesgo/beneficio)

0. **El reloj** (§1.0): `P4_TIMER_FREQ`/`platform.clkfreq` 360e6 → **320,010e6**,
   y después el rearme relativo del tick en `system/clkhandler.c`.  Es la
   corrección con más alcance de toda la lista - afecta a `sleep()`, a todos los
   timeouts de la pila y a cualquier `udelay()` - y la que más validación pide
   después (regresión AGENTS §4 + `ping -c 200` + `p4clk.py`).
1. `RAW_IBLEN` 100 → 32 **y** `NET_POOLSIZE` 24 → 96 (§1.1).  Baja de prioridad
   tras medir 23/24 libres en reposo y `drops 0`; se hace cuando se porten
   `udpserver`/`httpd` con concurrencia.
2. Registrar `curl`, `wget`, `ip` (§1.6).  (`syscheck` **sí** se registró y se
   midió; ver documento 06.)
3. Duplicados de `conf.h` → fuente única en `xinu.conf` (§1.2).
4. `FLASH LENGTH` 31M → 15M en `ld.script` (§1.3).
5. `NTCP` 10 → 16 si se va a usar httpd con varios clientes (§1.7).
6. DHCP renew + `resolvtab` (§1.4, §1.5) — son etapas de verdad, no cambios de macro.

Cada una se valida con `netstat`, `ps`, `free`/`memstat` y la regresión de AGENTS §4 antes de
pasar a la siguiente.

---

## 2. TinySTT / TinyTTS (punto 13)

Referencias dadas: `github.com/pschatzmann/TinySTT`, `.../TinyTTS`. Lo que son en realidad:

- **TinyTTS**: no es un sintetizador neuronal; es un envoltorio de voces pregrabadas/tables de
  forma de onda (el repositorio depende de datos y de un motor de reproducción PCM). Para este
  port es **buenas noticias**: lo que aporta es el *pipeline* de reproducción, y ese pipeline
  aquí ya existe y está validado (`write(AUDIO0, pcm)` a 48 kHz/16 bits/estéreo, 192 kB/s
  sostenidos medidos).
- **TinySTT**: reconocimiento de palabras aisladas con modelos cuantizados. Depende de un
  backend de inferencia (en los ejemplos, a menudo **TFLite Micro**).

Qué hace falta, en orden:

1. **Almacenamiento de modelos**: la SD ya está montada; un modelo de palabras clave cuantizado
   son 20-200 KB, cabe sin problema en el hueco de flash de 720 KB
   ([01 §2](01-memoria-nucleos.md)). Hay que añadir un `read()` de fichero a un buffer de
   PSRAM y **cargarlo por `0x88…`** si el modelo se copia a RAM ejecutable.
2. **Inferencia**: portar **TFLM** (TensorFlow Lite for Micro) es la parte grande — es C++ con
   kernels de `int8`/`int16` y una arena de tensors. Alternativa mucho más realista para este
   hardware: **KS841/KWS por redes propias en C entero**, o un detector de palabras por plantillas
   (DTW sobre MFCC) que cabe en C puro, entero, y sin dependencias. Recomendación: **MFCC + DTW
   para 8-12 palabras clave** primero, y TFLM solo si eso se queda corto.
3. **Front-end de audio**: ya está (`read(AUDIO0, …)`, captura medida en 193.547 B/s, ganancia
   calibrada con el registro 0x16). Esto es lo que hace el puerto viable: no hay que resolver
   micrófono, DMA ni clock.
4. **Restricción del árbol**: sin coma flotante en el kernel. Los MFCC piden multiplicaciones
   acumulativas — se hacen en `q31`/entero de 64 bits. Si se prefiere float, el STT **vive en un
   ELF cargado** ([04 §3](04-elf-java-apps.md)), donde el float es negociable porque el coste es
   una subrutina de soft-float, no una violación de la regla del kernel residente.
5. **TTS por la misma vía**: si lo que se quiere es "que hable", lo más barato es un repositorio
   de WAV/PCM en la SD (locuciones pregrabadas) reproducidas por `write(AUDIO0,…)`. Eso es un
   "TTS" funcional a escala de este proyecto, sin portar ningún motor.

Coste estimado: el pipeline completo de palabras clave (punto 1-3) es 3-4 etapas de tamaño E3
de audio. El TTS por ficheros, 1 etapa.

---

## 3. TinyUSB (punto 14) — estado real y el peligro que lo bloquea

Referencia externa en la máquina: `/home/daniel/kernel_tinyusb-main/usb/` (`tinyusb.h`,
`tusb.c`, `class/`, `device/`, `osal/`, `portable/`). **En el árbol Xinu no hay `device/usb`:
la etapa está plantada, no portada.** Lo que sí existe y validado son las tareas #28-#30 y #32:
configuración y bucle polled, bring-up de DCD/clock/PHY/IRQ en bare-metal, el glue MSC↔SD0
(bloque → LBA) y el diagnóstico del **latch de pads USB** (`LP_SYS`/`USJ`/`otg_conf`).

Quedan abiertas #31 (comando `usb msc` y validar enumerado en el PC) y #33 (bring-up del
transceptor FS-PHY: clock/PLL + GPIO `CAP_3`).

**El problema de fondo, dicho claro**: en esta placa los pads USB están **compartidos con el
USB-Serial-JTAG**, y el USB-Serial-JTAG **es tu consola** (`/dev/ttyACM0` del PC). Tomar los
pads para un dispositivo TinyUSB (MSC, CDC, HID) **corta la consola serie y el flasheo por
USB** hasta un reinicio con el strap correcto. Consecuencias prácticas:

- cualquier prueba de USB device se hace sabiendo que **se pierde el canal de depuración**;
- la consola de respaldo es el **segundo UART** (`SERIAL0`, si sacas los pads a un TTL externo)
  o telnet por WiFi (que sí funciona y no usa los pads);
- la alternativa limpia es **USB host** (para ratón/teclado/memoria) en lugar de device, si el
  conector de la placa lo permite; el P4 tiene OTG, pero el ruteado de esta placa manda los
  pads al puente USJ.

Propuesta sensata: dejar #31/#33 para cuando tengas un UART externo cableado, o cerrar la
etapa con `usb` en modo "solo enumerar y leer descriptores, sin claim del MSC" para no dejar la
placa sin consola en mitad del test.

---

## 4. Cámara OV5647, foto y vídeo a SD (punto 15)

**Aquí no hay nada portado** — y conviene saber por qué no es una etapa, son cuatro:

- En el árbol Xinu no existe ninguna cabecera de cámara: `find include -iname "*csi*" -o
  -iname "*isp*" -o -iname "*jpeg*"` devuelve **vacío**. Lo único relacionado es
  `LCD_CAM_BASE 0x500DC000` (`include/esp32p4.h:50`), que es el bloque de pantalla/cámara
  paralelo, **no** MIPI.
- En el SDK bare-metal sí están las HAL de referencia:
  `mipi_csi_ll.h`, `mipi_csi_phy_ll.h`, `mipi_csi_brg_ll.h`, `isp_ll.h`, `jpeg_ll.h`,
  `cam_ll.h`, `dma2d_ll.h`, `ppa_ll.h`
  (`/home/daniel/esp32-riscv-bare-metal-sdk-main/core/hal/esp32p4/include/hal/`).
- El **OV5647 es una cámara DVP/paralela con sensor CMOS y ISP propio**, no MIPI nativo: en un
  P4 se conecta por el interfaz **cam-parallel (LCD_CAM)** o mediante un puente. Además, en
  `include/board.h` **no hay ningún conector de cámara referenciado** (el único pin de cámara
  que aparece es `C6_IO2_STRAP`, que es del C6). O sea: primero hay que confirmar que **la placa
  que tienes** trae línea de cámara fuera; si no la trae, esto es hardware, no software.
- Grabar vídeo sin encoder es inviable: raw RGB a 640×480@15 fps son **5,9 MB/s** y la SD + FAT
  por el bus compartido con el WiFi no lo sostienen con margen. Con el **JPEG del P4**
  (`jpeg_ll.h`, acelerador `0x50086000`) sí: 640×480 JPEG ~40-80 KB por fotograma.

Ruta recomendada, si después de confirmar el conector se quiere seguir:
1. **Un fotograma** (VGA, raw, a PSRAM) por DVP/`cam_ll` + `LCD_CAM` — demuestra clock,
   sincronías y formato del sensor.
2. **Guarda ese fotograma como BMP en la SD** — demuestra el pipeline de almacenamiento y que
   el `sdmmcBusLock()` no se satura.
3. **JPEG por el acelerador** — convierte la etapa en fotos útiles (galería de punto 11
   desbloqueada aquí, no antes).
4. **Vídeo** = secuencia de JPEG + contenedor mínimo (AVI/MJPG escrito en FAT). Sin códec de
   frame-to-frame: MJPEG, no H.264 (el P4 no tiene encoder H.264).

Escalón honesto: esto es la parte más cara de toda tu lista, por delante de la JVM.

---

## 5. Librería MP3 (punto 17) {#mp3}

Candidatos y por qué:

| opción | licencia | coma flotante | RAM | veredicto |
|---|---|---|---|---|
| **Helix MP3 (RealNetworks)** | licencia propia (no GPL) | **no, fijo** | ~10-16 KB de stack + 1,7 KB de tables | **recomendada**: enteros, sin FPU, cabe en L2MEM, demostrada en micros |
| **minimp3** | CC0 | **sí, float** | 4 KB | fácil de portar pero viola la regla de no-float del kernel; vive bien en un ELF |
| **libhelix-aac** | licencia propia | fijo | más grande | si algún día quieres AAC |
| **MAD (libmad)** | GPL | **no, fijo** | ~70 KB | buena opción si no molesta GPL; más lenta que Helix |
| **shoutcast/ffmpeg** | | | | descartado: ni de lejos |

Integración propuesta, sin tocar el driver de audio:
- El decoder **no va en el kernel**; va en un ELF cargado ([04 §2](04-elf-java-apps.md)),
  llamando a `write(AUDIO0, pcm)` por el `exports[]`/tabla de API. Motivos: licencia aislada,
  tamaño, y que un fallo del decoder no tumbe la placa.
- Fuente: `read(fd, …)` del WAV/MP3 desde FAT en bloques de 4 KB; salida: tramas de 1.152
  muestras/frame, convertidas a **48 kHz, 16 bits, 2 slots** y con el **quirk mono** (duplicar
  la trama en los dos slots) ya conocido.
- Bitratado: 128 kbit/s = 16 kB/s de entrada + 192 kB/s de salida; holgado.
- Si el ELF no puede existir todavía (primero hay que ampliar `exports[]` y cargar desde SD), el
  paso intermedio razonable es **dejar el decoder fuera y reproducir WAV**, que ya funciona.

---

## 6. Actualización remota + bootloader OTA (punto 18) {#ota}

Tu descripción: "Xinu recibe la imagen, reinicia, el bootloader ve la imagen en SD, la copia a
flash y salta". Es la arquitectura correcta, y el árbol ya tiene casi todos los ingredientes.
Falta el aplicador.

### 6.1 Por qué el aplicador no puede ser el kernel

El kernel se ejecuta **XIP desde flash** (`0x40005000`, ver [01 §1](01-memoria-nucleos.md)).
Sobrescribir el flash donde estás leyendo las instrucciones es exactamente el caso que el SBL
ya resuelve: él se copia a **HP_TCM `0x30100000`** antes de tocar nada. Un aplicador en el
kernel necesitaría copiarse a TCM/L2MEM y ejecutar desde ahí, con interrupciones enmascaradas y
el código de borrado/programación en RAM. Se puede hacer, pero es frágil y no es el camino que
tú mismo propusiste. **Usar el SBL (o una etapa pegada al SBL) como aplicador es lo correcto.**

### 6.2 Diseño propuesto, tres piezas

**Pieza 1 — staging (la que ya sabes hacer).**
`udpserver` portado desde el S3 (`/home/daniel/xinu-s3-amp/shell/xsh_udpserver.c`, tarea #56)
recibe el fichero y lo escribe en `/xinu.img` en la microSD. Hay que añadirle:
- tamaño esperado + **CRC32** al final (el `crc32` de `libxc` ya está en el árbol),
- un marcador `/xinu.img.ok` escrito **después** del CRC correcto (escritura atómica por renombre:
  `/xinu.img.part` → `/xinu.img`).
Sin ese marcador, una transferencia cortada deja una imagen basura que el bootloader puede
programar y **matar el arranque**.

**Pieza 2 — petición de aplicar.**
Una orden en la shell (`applyupdate`) que escriba un flag en un sitio que el SBL lea. Opciones
por coste/riesgo:
1. **Marcador en la SD** (un fichero `/APPLY`): el SBL ya sabe… **no**, el SBL no tiene driver
   SD/FAT (hoy el SBL solo hace clock, MMU, PSRAM y arranque; el SD lo maneja el kernel). Coste:
   portar lectura SDMMC básica al SBL. Es la vía más "limpia" pero la más cara.
2. **Sector crudo en la SD** (bloque fijo, p. ej. LBA 0 del volumen, sin FAT): el SBL solo
   necesita un `read_blocks` mínimo. **Recomendada**: ~150 líneas de SBL.
3. **Una marca en flash** (byte en un offset pactado, p. ej. `0xF0000`) que programa el kernel por
   flash-mapeado… no se puede escribir flash por XIP sin driver QSPI en el kernel. Descartada.

**Pieza 3 — aplicador en el SBL.**
En `machine/sbl/`, antes de salta al kernel: si el flag está, lee `/xinu.img` de la SD por el
SDHOST (modo SPI/1-bit crudo), **valida la cabecera ESP** (magic `0xE9`, nº de segmentos,
checksum — el mismo formato que genera `esptool elf2image`, `machine/Makefile:76-83`), calcula
el destino **0x5000**, borra, programa y, si algo falla, **no toca la imagen vieja**. Luego
borra el flag y arranca la imagen nueva.

### 6.3 Dónde poner la imagen: reutiliza el hueco que ya existe

Hoy: SBL@`0x2000` (4.304 B), kernel@`0x5000` (291.372 B, termina `0x4C22C`), ULP@`0x100000`
(724 B). El hueco entre el fin del kernel y el ULP son **~720 KB libres**.

| plan | layout | comentario |
|---|---|---|
| **A. A/B en el hueco actual** | kernel activo en `0x5000`, candidato en `0x80000` (≤512 KB), flag en `0xF8000` | **el que recomiendo**: cabe en los 720 KB, no mueve nada que hoy funcione, y el "salto" sigue siendo 0x5000 (el BootROM/SBL no cambian de sitio) |
| B. Dos particiones kernel + una tabla | particiones en `0x8000` | obliga a flashear el kernel en dos sitios y a mover el ULP; más limpio pero toca la cadena de arranque que ya validaste |
| C. `otadata` al estilo ESP-IDF | — | **no**: eso es un mecanismo de IDF; la regla del proyecto es bare-metal y aquí no hay `esp_image` ni bootloader IDF |

Con A, el flujo es: kernel recibe → escribe SD → `applyupdate` → reset → SBL copia
`0x80000`→`0x5000` (previa validación y copia de seguridad del activo a `0xC0000` si cupiese:
291 KB + 291 KB sí caben en los 720 KB) → arranca → si el kernel nuevo no llega al banner, el
SBL puede hacer *rollback* con un contador de intentos en el flag.

**Sin tabla de particiones no hay nada que proteja los offsets**: la disciplina es del script de
flasheo. Añadir una mini-tabla firmada en `0x10000` es opcional pero hace que el SBL pueda
comprobar tamaños en vez de fiarse.

### 6.4 Coste y orden

1. `udpserver` portado + CRC + renombre atómico (etapa de tamaño medio, todo en el kernel).
2. Lectura SD de bloque crudo en el SBL + aplicador con validación de cabecera (etapa grande y
   **la única que puede dejar la placa imposible de arrancar**; hay que probarla con `make flash` por
   cable como red de seguridad).
3. Flag + rollback.

---

## 7. El dashboard en la SD servido por httpd (punto 19) {#dashboard}

Lo que pediste: "todo lo que documentarás lo pasas a un dashboard estilo Material Design, lo
guardas en la SD y httpd lo muestre".

Parte 1 (el dashboard) está hecha:
[`docs/xinu-p4-dashboard.html`](../xinu-p4-dashboard.html), Material Design (App Bar con chips de
estado, Cards con elevación por sombra, surface tonal, chips de veredicto, tipografía de sistema
**sin descargas** para que funcione offline). Nueve pestañas: Resumen · Memoria · Núcleos ·
Dispositivos · Red y WiFi · ELF/Java/apps · syscheck · conf.c · Roadmap, con el mapa de memoria
dibujado a escala en CSS (no en ASCII) y el checklist del orden de flasheo de `syscheck`.
Escrito como **un único fichero, sin red y sin dependencias**: un documento HTML completo, que se
abre en el navegador del PC, se copia a la SD tal cual y se serviría igual cuando el httpd lea FAT.
Se muestra también aquí mismo en la conversación.

Parte 2 (guardarlo en la SD y servirlo) **no está hecha y tiene un motivo concreto**: el
`httpd` actual solo responde el `httpd_index[]` embebido (`shell/xsh_httpd.c:45`) y **no lee
ficheros de la SD**. Para que `http://192.168.1.146/` sea el dashboard hacen falta dos cosas del
§5.2 de [03-red-y-wifi.md](03-red-y-wifi.md):

1. httpd con ficheros desde FAT y `Content-Type` por extensión (`.html`, `.css`, `.js`, `.png`),
   tarea #57.
2. Servir con `Connection: close` y caché del HTML en PSRAM, porque cada `open()` de la SD toma
   el `sdmmcBusLock()` que también necesita el C6.

Instrucciones para poner el fichero en la SD hoy (que sí funciona, para verlo desde el PC):

```
$ cp docs/xinu-p4-dashboard.html /media/daniel/<sd>/index.html     # tarjeta montada en el PC
# y en la placa: xsh$ sd   /   xsh$ ls /index.html   /   xsh$ head /index.html
```

Si te vale con verlo en el navegador del PC sin esperar al httpd con ficheros, con copiar el
HTML a cualquier sitio del PC y abrirlo basta: es estático.

### 7.1 Un dashboard no-necesita-httpd también es posible

Para pintar el resumen en el **LCD** de la placa hace falta solo lo que ya está
(`LCD_CTRL_RECT`/`TEXT`/`BLIT`). Un "Material lite" en 320×480 con las mismas tarjetas es una
hebra nueva sobre el framebuffer propuesto en [02 §3](02-dispositivos.md), y no depende de la
red. Es la vía más corta si lo que quieres es que la placa se enseñe a sí misma.

---

## 8. Tareas abiertas, con lo que falta en cada una

| tarea | qué falta exactamente | tamaño |
|---|---|---|
| #38 W5 | una app de red que pruebe conectividad real (HTTP GET saliente o descarga pequeña). `xsh_curl` ya existe sin registrar → **1 línea de `shell.c` y probar** | pequeño |
| #56 udpserver | port `xsh_udpserver.c` del S3 + CRC + `.part`→ renombre | medio |
| #57 httpd completo | ficheros desde FAT, `POST`, 404, REST (`httpapi.c` del S3), webterminal con `memtty` | grande |
| #58 OTA por UDP + bootloader | §6 entero | grande (y el único que puede dejar la placa imposible de arrancar) |
| #61 power save del C6 | **no hay API en el SDK bare-metal**; solo queda una petición RPC propietaria o aceptar la latencia | incierto |
| #62 telnet tras corte | hebra releaser con `TCP_CTRL_RCVTIMEO` y `tcpFree()` en todos los caminos (§6.6 de [03](03-red-y-wifi.md)) | pequeño |
| #31/#33 USB | §3: el bloqueo real es que los pads son la consola | medio, con riesgo de consola |
| #63 esta documentación | ficheros en `docs/port-esp32p4/` + dashboard | hecho |
| #64 syscheck | [06-syscheck.md](06-syscheck.md) + `examples/syscheck/` | hecho (propuesta, sin cablear) |

## 9. Credenciales y privacidad (la app de "claves guardadas") {#credenciales}

Diseño que propongo en lugar de consultar el gestor de contraseñas del navegador, que no es
hacerlo por vía técnica ni por vía responsable:

- **Qué se guarda**: solo lo que **tú escribes** en la placa (SSID/pass de tus redes, tokens de
  tus endpoints propios), en `/etc/secrets.bin` en la SD.
- **Cómo se cifra**: clave derivada con PBKDF2-HMAC-SHA256 (entero, ~50.000 iteraciones a 360
  MHz es asumible) de una **frase que tecleas en la placa**; la sal va en el fichero. Nada del
  secreto sale por la red.
- **Qué NO se hace**: volcado de contraseñas guardadas de Chrome/Firefox (el navegador no las
  expone por API y descifrar su base local pide su clave maestra y sus DPAPI/OSCrypt), ni
  automatizar OAuth con tus credenciales de Google (eso requiere un cliente registrado con
  `client_id`/`secret` y scopes; además, Google no tiene un endpoint "devuélveme mis
  contraseñas").
- **UI**: una app Java (`p4.Vault`) sobre la tabla de API, con teclado en pantalla o teclado
  USB si se portara (#31/#33), y expiración del descifrado tras inactividad.
