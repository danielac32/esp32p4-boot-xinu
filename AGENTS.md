# Contexto del proyecto

Documento de trabajo para quien (o la sesión de agente que) meta mano en este
árbol. Refleja el estado real de la rama `main` y de lo que hay sin commitear,
verificado en placa. Última actualización: 2026-11-01, durante la fase de audio.

---

## 1. Qué es esto

Port de **Embedded Xinu a baremetal RISC-V sobre el ESP32-P4**, en solitario y a
nivel de registro. La placa es una **Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5**:
pantalla 3.5" con lcd/sd/fat32 ya funcionando, PMIC AXP2101, codec ES8311 con
altavoz (amplificador NS4150B) y micro, táctil FT6336U, y WiFi6 (no tocada).

No hay FreeRTOS ni ESP-IDF en runtime. Los drivers escriben offsets crudos
sobre la dirección base del periférico; los headers del SDK de Espressif se usan
solo como referencia de documentación, nunca como include.

**La interfaz gráfica Java está aparcada** (2026-11-05): el JVM, sus natives, el
SDK java, el launcher `workspace` y las cinco apps viven en `parked/java-gui/` y
no entran en el build.  El kernel arranca sin panel: red, consola y shell.
Antes de tocar nada de eso, leer `parked/java-gui/README.md` (tiene el paso a
paso para reactivar y el estado de lo que quedó roto).

**LVGL volvió al árbol** (2026-11-05) como biblioteca del kernel, sin la VM: es
la base de la tabla de exports para apps en C cargadas como ELF
(`system/exports.c`; la app de ejemplo `examples/p4-elfsdk/apps/ejemplo/`
es la referencia para quien escribe una app).  `main()` sigue sin lanzar interfaz alguna, así que el panel arranca en
negro y la hebra residente `lvglui` solo refresca lo que un ELF monte.

- Árbol Xinu: `/home/daniel/xinu-p4.2` (este directorio; es repo git, rama `main`).
- SDK de referencia: `/home/daniel/Descargas/esp32-riscv-bare-metal-sdk-main`
  (`core/hal/esp32p4/include/hal/*.ll.h`, `core/soc/esp32p4/register/hw_ver1/`,
  `drivers/es8311/`, `drivers/i2s_drv/`). Nota: `~/esp-idf` también está clonado.
- Esquema de la placa: `/home/daniel/Descargas/ESP32-P4-WIFI6-Touch-LCD-3.5-schematic.pdf`.
- Datasheets de referencia: guía del ES8311
  <https://files.waveshare.com/wiki/common/ES8311.user.Guide.pdf> (va bien con
  `pdftotext -layout`), y la del AXP2101, que en cambio **no** convierte con
  pdftotext: hay que leerla por rangos de páginas.

---

## 2. Cómo se construye, se flashea y se mira

```sh
cd /home/daniel/xinu-p4.2/compile
make ESPTOOL=/home/daniel/esp-idf-py/bin/esptool          # compila
make ESPTOOL=/home/daniel/esp-idf-py/bin/esptool flashxinu # flashea + hard reset
```

- `esptool` y un python con pyserial viven en `/home/daniel/esp-idf-py/bin` y no
  están en el PATH.
- La consola es `/dev/ttyACM0` a 115200 (USB-CDC nativo). **Solo un proceso puede
  tenerla abierta**; si cutecom u otro la sostiene, todo lo demás falla con
  "Dispositivo o recurso ocupado" — mirar `fuser -v /dev/ttyACM0` antes de culpar
  al kernel.
- **Abrir el puerto NO resetea la placa** (medido el 2026-11-06:
  `serial.Serial("/dev/ttyACM0", 115200, exclusive=True)` sobre una placa ya
  arrancada devuelve el `xsh$` a los 5 s y sin banner). Lo que sí resetea es el
  pulso RTS de `make flash` y el comando `reset` de la shell. Y **tras un
  `reset` el enlace CDC del anfitrión queda roto**: el nodo sigue existiendo pero
  no vuelve ni un byte hasta que `esptool --chip esp32p4 --port /dev/ttyACM0
  --baud 115200 --after hard_reset chip_id` re-sincroniza la USB y suelta la
  placa arrancando normal. Es la forma limpia de reiniciar sin re-flashear.
- El arranque **no son 9 s cuando la AP guardada no existe**: el gate de
  `netboot` hace 3×3 reintentos de ~50 s y el `xsh$` aparece a los ~190 s
  (medido). Con la AP correcta son ~20 s. El banner de arranque no es capturable
  si la placa ya estaba arriba; se valida el init de los devices ejecutando
  comandos.
- Automatización usada: un proceso python que abre el puerto, `dtr=True`, espera
  9,5 s, limpia el búfer y por cada comando escribe `cmd\n` y lee en una ventana
  de tiempo fija. Scratch en `/tmp/p4cmds.py` (varios comandos, una ventana cada
  uno) y `/tmp/p4cmd.py` (uno solo).
- El enlace deja un aviso inocuo y conocido: `xinu.elf tiene un segmento LOAD con
  permisos RWX`.

No hay nada que necesite sudo. Los ficheros del árbol son propiedad del usuario.

---

## 3. Reglas de trabajo del dueño del árbol

Las repite y son vinculantes:

- **No romper lo que funciona. No dañar el kernel.** Un fallo de hardware se
 recovere; uno de consola no.
- **Las etapas solo se empiezan con autorización explícita** suya. Lo que no está
  autorizado se deja apuntado, no se hace.
- Autorizado de antemano: compilar con `make`, `make clean`, y flashear con
  `make flash`. Nada de push a remotas ni acciones irreversibles sin preguntar.
- Los valores que se muestran por consola tienen que estar **leídos del propio
  periférico**, no de un caché del driver: eso es lo que convierte un comando en
  una prueba del bus.
- Quiere **ver las direcciones** (descriptores, páginas compartidas) en la
  consola, no solo "OK".
- El dispositivo `i2c` hace todo por `control()`: no añadir `read()` al bus.
- Comentarios y docs en español, registro a registro, explicando el *por qué*.
  Sin coma flotante en kernel (se trabaja en décimas de dB, microsegundos, etc).

---

## 4. Estado del kernel (lo que ya está y vale como regresión)

Dispositivos montados y validados en placa: consola serial, `gpio`, `i2c0` con su
monitor de bus, `touch0` (FT6336U, con demo de coordenadas), `power` (AXP2101,
**solo lectura**: escribir 0x10/0x80 puede tumbar la placa), `lcd`, `sd`+fat32 con
la microSD de 60 GB, y `audio0` (ver §5). El segundo núcleo HP (core1) está
**aparcado formalmente** y no ejecuta nada: su diseño está en `README_CORE1.md`,
sección 4 dibujada pero no implementada.

Set de regresión mínimo antes de dar nada por cerrado:

```
i2c scan        -> 0x18 (ES8311) 0x34 (AXP2101) 0x36 (no molestar) 0x38 (FT6336U)
power           -> bateria 100 %, tension ~4140 mV
touchpanel 2    -> sin bloqueos
audio           -> estado del codec, relojes y etapa de salida
ps              -> el kernel vivo
```

El 0x36 está **aparcado por decisión**: apuntado y sin dedicarle tiempo.

---

## 5. Fase de audio: qué está hecho

Cuatro etapas. Las cuatro validadas en placa; las dos últimas ya moviendo PCM de
verdad por el `read()` y el `write()` del device (E1 y E2 estaban pensadas para no
hacer ruido: son muteadas y sin PCM).

**E1 — codec ES8311 por I2C** (`device/audio/es8311.c`, `audioInit.c`).
Trayecto completo por el bus compartido: reset, coeficientes de reloj para
MCLK 12.288 MHz / fs 48 kHz, formato 16 bits, volumen, mute y ganancia del micro.
Las secuencias vienen del driver de referencia del SDK, nada inventado.

**E2 — relojes I2S0 maestro** (`device/audio/i2s.c`).
El bloque I2S del P4 **no tiene registros de reloj**: fuente y divisor
fraccionario están en `HP_SYS_CLKRST.peri_clk_ctrl13/14` y la salida MCLK por el
pad se habilita en `LP_AON_CLKRST`. La división fraccionaria 40 MHz → 12,288 MHz
es exacta (N=3, z=49, x=2, y=45), así que **el APLL no hace falta**. BCLK 1,536 MHz
y LRCK 48 kHz; el contador interno `tx_bck_cnt` los mide.

**E3 — salida PCM: AHB-DMA + anillo en PSRAM + tono** (`ahbdma.c`, `audioOut.c`).
- El I2S del P4 **no tiene FIFO visible para la CPU ni registro de llenado**: lo
  vacía el DMA. Por eso el flujo exige descriptores, como el IDMAC del SD.
- Motor: AHB-DMA en `0x50085000`, canal TX 0, periferico seleccionado con
  `peri_out_sel = 3` (I2S0). Ventana de direcciones del motor
  `0x40000000..0x4FFC0000`, que sí cubre la PSRAM cacheada `0x48......`.
- Páginas compartidas declaradas en el linker (`.audioshare`, colocada
  deliberadamente **antes** de `.sdshare`: el reparto del SD queda intacto y
  `_psram_free_start` sigue cayendo por encima de todas las páginas):

  | símbolo | dirección | uso |
  |---|---|---|
  | `_audio_desc_page` | 0x48000000 | 4 KB de descriptores del tono |
  | `_audio_buf_page` | 0x48001000 | 8 KB de PCM (2048 frames) |
  | `_audio_stream_desc_page` | 0x48003000 | 4 KB de descriptores del anillo de reproducción |
  | `_audio_stream_buf_page` | 0x48004000 | 64 KB de PCM (32 trozos de 2048 bytes) |
  | `_audio_in_desc_page` | 0x48014000 | 4 KB de descriptores de captura |
  | `_audio_in_buf_page` | 0x48015000 | 64 KB de PCM capturado |

  El CPU las escribe por el acceso directo `0x88......` (no pasa por cache) y
  dentro de los descriptores se anota la forma `0x48......`, que es la que
  entiende el motor. Es el mismo reparto que ya demuestra el driver del SD, y
  evita tener que invalidar cachés en el camino de datos.
- Descriptor de 3 palabras (dw0 con size/length/eof/owner, buffer, next), lista
  **cíclica** (`suc_eof=0`, el último apunta al primero, dueño cedido al motor):
  no hay que rearmar nada y no se usan interrupciones. La duración la manda quien
  llama, bloqueándose y mandando parar.
- Secuencia anti-pop: patrón → descriptores → relojes → enlace del motor → quitar
  el silencio del codec → **por último** AMP_POWER (GPIO53, NS4150B activo por
  alto). Al parar se hace la inversa. El pad permanece en alta impedancia hasta
  que hay datos circulando.
- El tono cierra el bucle sin costura: el anillo mide un número entero de periodos
  (`base = 48000/mcd(hz,48000)` y se toma el múltiplo más largo que cabe), así que
  no hay chasquido en cada vuelta. Las frecuencias que no cierran se cuantifican a
  la más próxima y **se imprime la que suena de verdad** (1000 Hz suena a 1000; el
  peor desvío en toda la gama 50..8000 Hz es de 12 Hz).

Medido en placa: el contador de sincronización del I2S (registro 0x74, que cuenta
**muestras de 16 bits**) avanza a 95.551/s contra 96.000 teóricos; el descriptor
actual recorre la lista cíclica; sin `dscr_err`; reinicios encadenados y las rutas
de error (30 Hz, 9000 Hz) sin dejar nada armado.

**E4 — el device por `read()` y `write()`** (`audioOps.c`, `audioIn.c`, ruta del
micro en `es8311.c`). Reproducir fichero, grabar del micro y volver a escucharlo.
- La fila del devtab la **genera** `compile/config/config` desde
  `compile/platforms/esp32p4/xinu.conf` (`-r audioRead -w audioWrite`); editar
  `system/conf.c` a mano no sirve, lo borra cualquier `make`.
- `write()` apila tramas en el anillo de PSRAM y duerme cuando se llena: ese
  sueño es lo que sostiene los 192.000 bytes/s. `read()` vacía el anillo de
  captura y **no espera**: entrega lo que el motor ya cerró, 0 si va por mitad
  de un trozo.
- RX: canal IN del AHB-DMA (lista cíclica de 32 trozos de 2048 bytes, lo mismo
  que cierra un fin de recepción del I2S) y sin interrupciones; la única
  posición que da el periférico es `in_suc_eof_des_addr`, de ahí la cuenta de
  trozos producidos y consumidos.
- **Lo que costó la etapa, y es una trampa del chip, no del driver**: mover PCM
  por la vía *con cache* (`0x48......`) deja líneas de L2MEM leídas a cero. Se
  manifestaba como `irq with null vector: irq=31` con `interruptVector[9..47]`
  a cero a partir del borde de una línea de 64 bytes, y como un cuelgue mudo.
  Es el mismo mecanismo que ya obligó a `.l2reserve` (una barrida grande de
  PSRAM limpia los primeros 128 bytes de L2MEM) y a poner la tabla de vectores
  del CLIC en flash. El arreglo es el de siempre: el CPU toca la PSRAM **solo**
  por el acceso directo `0x88......`, también el búfer de grabación de la shell.
  A 192 kB/s sobra de margen.
- Medido en placa: 193.547 B/s capturados contra 192.000 teóricos; `rec 15000`
  → 720.000 frames y `loop` de los 720.000 sin errores de descriptor ni
  `dscr_err`. Queda por oír la reproducción de un WAV real (`play`) y por
  confirmar con los oídos el `loop`.
- **Ganancia de captura, medida con el propio altavoz como patrón** (tono de
  1 kHz a `vol 75` = 0 dBFS y el pico de la captura como escala): el registro
  0x16 --escala digital del ADC-- es el mando que más nivel mueve y **no lo
  escribía nadie**, así que la captura arrancaba con el valor que dejó la sesión
  anterior, porque el codec no sigue el reset del P4. Ahora `es8311_start_mic()`
  lo programa a `ES_MIC_SCALE` (6 = +36 dB) junto a 0x14 y 0x17, y `audio gain N`
  lo cambia en caliente. Con 0 (+0 dB) el tono sale al 11 %, con 4 (+24 dB)
  recorta, y las tomas sin hablar --que antes daban 2..7 %-- dan ahora 7..20 %
  (+12 dB). La cadena es estable entre capturas repetidas (24/25/22/23 % en
  cuatro tomas del mismo tono): si una grabación suena baja, es el contenido, no
  el camino.

Mandos de shell disponibles:

```
audio                  estado (codec, relojes, salida, dB, flujo)
audio vol N            0..100 %   (ver el aviso de escala en §6)
audio mute | unmute
audio gain N           escala digital del ADC (0x16): 0..7 = 0..+42 dB
audio pga N            ganancia analógica del micro: 0..10 = 0..+30 dB
audio clocks [off]     arrancar/parar relojes y medir BCLK
audio out HZ [ms]      tono de HZ hercios durante ms (def. 1000, max 30000)
audio out off          parar la salida
audio play FICHERO     WAV PCM 16 bits / 48 kHz (o PCM crudo) por el altavoz
audio rec MS           grabar MS milisegundos del micro a PSRAM (max 30000)
audio loop             reproducir lo último grabado
audio in [on|off]      montar/parar la captura; solo `in` = estado del RX
```

---

## 6. Qué falta para cerrar esta fase

1. **Oídas pendientes.** El nivel del tono y el `audio loop` **hablando durante la
   grabación** ya están confirmados con los oídos (con `audio vol 100` suena alto;
   el techo limpio es `audio vol 75` = 0 dB con muestras a -0,3 dBFS). Falta un
   `audio play` con un WAV real de 48 kHz / 16 bits; en la microSD ahora mismo no
   hay ninguno. MP3/AAC/MP4 siguen fuera de alcance: hace falta un decodificador
   y aquí no entra ni un byte comprimido.
1b. **Causa del `loop` mudo, resuelta (quirk de mono).** El ES8311 es mono: el
   ADC vuelca la muestra **solo en el slot izquierdo** y deja el derecho a 0
   (medido con `memdump -f` sobre `recPcm`: L al 100 %, R todo ceros), y en esta
   placa el DAC mono reproduce **el slot contrario** al que saca el ADC. Un `loop`
   en copia literal mandaba la señal al slot que el DAC no usa: silencio con el
   contador de muestras del I2S subiendo y 0 huecos, que es lo que parecía un
   fallo de reproducción. El arreglo es duplicar slot0→slot1 en la captura
   ([shell/xsh_audio.c:621](shell/xsh_audio.c#L621)), igual que hace el patrón del
   tono con los dos slots ([device/audio/audioOut.c:272](device/audio/audioOut.c#L272));
   así `loop` y un WAV guardado se oyen, y `audio play` de un estéreo real queda
   intacto. Se barajó la vía de hardware (registro 0x09 `SDP_IN_SEL`, bit 7 = 0
   izquierdo / 1 derecho al DAC), pero el orden de slots en la línea no coincide
   entre ADC y DAC en esta placa y era una moneda sin medir: se eligió la
   duplicación garantizada.
2. **Decidir la escala de `vol`.** El registro 0x32 es **lineal en decibelios**
   (0,5 dB/paso, 191 = 0 dB, 255 = +32 dB), así que el porcentaje actual reparte
   linealmente sobre el registro: 1 punto ≈ 1,3 dB, 60 % son -19,5 dB y 100 % es
   realce digital que recorta. Se puede convertir en curva perceptual cambiando
   `es8311_set_volume()`, pero **redefine un mando ya validado en E1**: requiere
   autorización.
3. **Trampa conocida y no resuelta:** el registro 0x32 **sobrevive al flasheo**
   (el soft reset de `es8311_init` no lo borra). Tras reflashear, el nivel sigue
   siendo el último que se puso. Lo mismo le pasa al 0x16 de la captura, y ahí
   sí está resuelto: `es8311_start_mic()` lo reescribe en cada arranque de la
   ruta.
4. **Defectos aparcados a propósito**, que afectan a esta fase pero no se tocan:
   - `P4_TIMER_FREQ = 360000000` frente a ~320 MHz reales
     ([include/esp32p4.h:160](include/esp32p4.h#L160)): todos los retardos del
     kernel van 9/8 largos. `audio out 440 1000` dura ~1,125 s y la medida de BCLK
     sale a 1728 MHz en vez de 1536. La consola corrige ese factor al estimar
     el ritmo del flujo, no al dormir.
   - `include/i2s.h` es un stub rancio que no usa el driver real
     (`device/audio/i2s.h` es el verdadero).
   - `compile/Makefile` deja `DEPFLAGS :=` vacío: **ningún cambio de cabecera
     recompila a quien la incluye**. Tras tocar un `.h` o el `ld.script` hay que
     hacer `make clean`, o se linkean objetos viejos contra la cabecera nueva.
   - `irq_missing()` ([system/platforms/esp32p4/clic.c](system/platforms/esp32p4/clic.c))
     sigue siendo bring-up: pinta el estado y se queda en `for(;;)`, así que una
     línea sin handler = consola muerta hasta reflashear (el DTR del puerto no la
     recupera).
5. **Cuelgue de consola, abierto y sin reproducir.** El usuario reportó que la
   shell «a veces se queda bloqueada» durante el ciclo rec/loop. En dos pasadas
   automatizadas (`rec` con `out` de fondo = TX+RX concurrentes, luego `loop`) no
   apareció, y el `read` del `rec` ya topea con `AUDIO_REC_SILENT_LIMIT`, así que
   no cuelga para siempre por el RX. Queda por cazarlo: hace falta saber en qué
   comando exacto se queda y si sale sola o hay que resetear.
6. **Pendiente de commitear**: todo el device de audio está sin commitear en
   `main` (`device/audio/`, `shell/xsh_audio.c`, `include/audio.h`, `ld.script`,
   `xinu.conf`, `platformVars`, `shell/Makerules`, `include/version.h`). La
   limpieza de `/home/daniel/xinu-p4.2-touch` también sigue pendiente.

---

## 7. Dónde mirar antes de tocar

- Todo lo gráfico Java (JVM, natives, apps, launcher): `parked/java-gui/README.md`.
- Apps en C cargadas como ELF: la tabla de símbolos del kernel en
  `system/exports.c` (+ `include/exports.h`), el cargador en `system/elfload.c`
  (+ `include/elfload.h`) con su único cliente de shell: `elf /elf/x.elf [args...]`,
  que corre el app en la hebra del propio comando.  `exit()` dentro del app es
  `kill(gettid())` (elf_exit en `system/elfload.c`), y el `kill()` del kernel
  libera el bloque de PSRAM vía los campos `elf_task/elf_img/elf_size` de
  `struct thrent`.  Cómo se escribe una app: `examples/p4-elfsdk/apps/ejemplo/`
  (ejemplo.c + Makefile + lib/start.S; sin cabecera de exports: solo includes
  del kernel).  Regla de
  oro: con `--gc-sections` una función a la que nadie llama no existe en el ELF,
  así que la tabla de exports **define** la ABI; no añadir nombres a mano sin
  comprobarlos con `nm`.  Segunda regla, medida en placa: el código del app se
  copia y se ejecuta por la ventana directa `0x88......`, pero eso **no** evita
  la cache — hay que hacer write-back + invalidate de L1-D y L2 sobre el bloque
  antes de copiar (paso 5 de `system/elfload.c`).

- Diseño del segundo núcleo: `README_CORE1.md`.
- Notas del arranque de pantalla: `docs/lcd-boot-notas.md`.
- El device de audio: `device/audio/` (Makerules enumera los 6 .c) y su API
  pública en `include/audio.h`; los códigos de `control()` están ahí definidos.
- Patrón a imitar para cualquier cosa que necesite descriptores + memoria
  compartida con un motor: `device/sdmmc-esp32p4/sdHw.c` y la sección `.sdshare`
  de `compile/platforms/esp32p4/ld.script`.
