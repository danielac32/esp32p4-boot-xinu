# 02 · Dispositivos: audio, power, LCD/touch y SD

La tabla de ranuras es `include/conf.h:34-66` (generada desde
`compile/platforms/esp32p4/xinu.conf`; ver
[05-roadmap-y-conf.md](05-roadmap-y-conf.md#conf) para qué se edita en cada sitio).

```
 0 SERIAL0 uart   1 DEVNULL   2 CONSOLE tty   3 PSRAM0 psram  4 LCD0 lcd
 5 SDMMC0 sdmmc   6 SD0 sdblock  7 FAT0 fat   8 GPIO0 gpio     9 I2C0 i2c
10 TOUCH0 touch  11 POWER0 power 12 AUDIO0 audio 13 ETH0 espeth
14 RAW0 15 RAW1  16 UDP0 17 UDP1  18..27 TCP0..TCP9  28..31 TELNET0..TELNET3
32 NAMESPACE
```

---

## 1. audio (AUDIO0 · ES8311 + I2S0 + AHB-DMA) {#audio}

**La fuente canónica del estado de la fase es [AGENTS.md §5](../../AGENTS.md)**. Aquí va
el resumen operativo y las cuatro cosas que hay que saber para no romperla.

### Formato del cable

- **48 kHz, 16 bits, 2 slots estéreos ⇒ 4 B/trama, 192.000 B/s**
  ([include/audio.h:11-16](../../include/audio.h#L11-L16)). No hay cabeceras ni
  decodificación dentro del driver: un WAV o PCM crudo se normaliza **antes** de escribir
  (`shell/xsh_audio.c`). MP3/AAC/MP2 necesitan decodificador — no existe (punto 17 de tu
  lista; análisis en [05-roadmap-y-conf.md](05-roadmap-y-conf.md#mp3)).
- `write(fd, buf, n)` encola tramas en el anillo de PSRAM y **duerme** si se llena (ese
  sueño es lo que sostiene los 192 kB/s). `read(fd, buf, n)` **no espera**: entrega lo que
  el motor ya cerró, `0` si va a media trama.
- **Quirk mono (arreglado y validado al oído)**: el ES8311 de esta placa solo reproduce
  bien si la **misma** trama va en los dos slots. Un audio mono hay que duplicarlo por
  frame antes de escribir, no por bloque.

### Qué escribe quién

| capa | fichero | qué hace |
|---|---|---|
| codec | `device/audio/es8311.c` | registro a registro por **I2C0 a 0x18**, compartido con touch/PMIC |
| relojes | `device/audio/i2s.c` | I2S0 maestro; **el bloque I2S no tiene registros de reloj**: fuente y divisor fraccionario están en `HP_SYS_CLKRST.peri_clk_ctrl13/14` y la salida MCLK se habilita en `LP_AON_CLKRST` |
| motor | `device/audio/ahbdma.c` | AHB-DMA `0x50085000`, canal TX 0 / IN, `peri_out_sel = 3` (I2S0) |
| salida | `device/audio/audioOut.c` | anillo cíclico de descriptores + amplificador |
| entrada | `device/audio/audioIn.c` | 32 trozos de 2.048 B, cierre por `in_suc_eof_des_addr` |
| device | `device/audio/audioOps.c` | `open/read/write/control/close` |

**I2S del P4 no expone FIFO ni registro de llenado a la CPU**: lo vacía el DMA. Por eso el
PCM exige descriptores, igual que el IDMAC del SD.

### Anillos en PSRAM (sección `.audioshare`, `NOLOAD`)

`_audio_desc_page 0x48000000` (4 KB) · `_audio_buf_page 0x48001000` (8 KB, 2.048 tramas) ·
`_audio_stream_desc_page 0x48003000` (4 KB) · `_audio_stream_buf_page 0x48004000` (64 KB) ·
`_audio_in_desc_page 0x48014000` (4 KB) · `_audio_in_buf_page 0x48015000` (64 KB).
Total `.audioshare` = **151.552 B** (medido con `size -A`). Está puesta **antes** de
`.sdshare` a propósito: así `_psram_free_start` queda por encima de todas las páginas y el
reparto del SD no se entera.

Regla de caché (la que salvó la E4): **la CPU toca estas páginas solo por `0x88……`**
(ventana sin caché) y dentro de los descriptores se anota la forma `0x48……`, que es la que
entiende el motor. Ver [01-memoria-nucleos.md §4](01-memoria-nucleos.md).

### Secuencia anti-pop (no la reordenes)

patrón → descriptores → relojes → engancha el motor → quita el silencio del codec →
**lo último** `AMP_POWER` = **GPIO53**, NS4150B activo por alto
(`AUDIO_OUT_ST_AMP`, `device/audio/audioOut.h:50`). Al parar, inversa. El pad se queda en
alta impedancia hasta que hay datos circulando.

### Opcodes de `control(AUDIO0, …)`

33 en total, `include/audio.h:32-96`. Los que se usan desde fuera del driver:

```
0x01 VOLUME(arg 0..100)   0x02 MUTE(0/1)   0x03 GET_VOLUME   0x05 GET_MUTE
0x04 MIC_GAIN(0..7 = 0..42 dB)  0x18 MIC_PGA(0..10)  0x19 MIC_GET  0x20 MIC_SCALE (reg 0x16)
0x06 CLOCKS(1 arranca / 0 para)  0x07 BCLK(us de medida) -> Hz
0x08 OUT(Hz pedidos) -> Hz aplicados   0x09 OUT_OFF   0x0A OUT_STAT   0x0C OUT_CNT
0x0D OUT_RING  0x0F STREAM(1/0)  0x11 OUT_QUEUE  0x12 OUT_CAP  0x21 STREAM_GAPS
0x13 IN(1/0)  0x14 IN_STAT  0x16 IN_DROP  0x17 IN_RING  0x1D IN_BASE  0x1E IN_EOF  0x1F IN_QUEUE
0x1A MIC_STAT 0x1B MIC_ADCSV 0x0B OUT_DESC 0x15 IN_DESC
```

Medido en placa (AGENTS §5): el contador de sincronización del I2S (registro `0x74`, cuenta
**muestras de 16 bits**) avanza a 95.551/s contra 96.000 teóricos; captura 193.547 B/s
contra 192.000. El peor desvío de tono en 50..8.000 Hz es 12 Hz (el anillo se fuerza a un
número entero de periodos: `base = 48000/mcd(hz,48000)`).

**Ganancia**: el registro `0x16` (escala digital del ADC) era el mando que nadie escribía,
así que la captura arrancaba con el valor de la sesión anterior (el codec **no** sigue el
reset del P4). Hoy `es8311_start_mic()` lo fija a `ES_MIC_SCALE = 6` (+36 dB) con 0x14 y 0x17.

### Shell

```
audio                    estado (codec, relojes, salida, dB, flujo)
audio vol N / mute / unmute / gain N / pga N
audio clocks [off]       arranca/para relojes y mide BCLK
audio tone HZ [ms] [vol]  audio play fich | rec SEG | loop
```

---

## 2. power (POWER0 · AXP2101 por I2C0 a 0x34) {#power}

**Device de solo lectura, deliberadamente.** `device/power/powerControl.c` admite dos
opcodes y nada más ([include/power.h:19-22](../../include/power.h#L19-L22)):

```c
POWER_CTRL_PERCENT  0x01  /* -> 0..100, SYSERR si no hay PMIC */
POWER_CTRL_INFO     0x02  /* arg1 = power_info_t* -> OK o SYSERR */
```

Motivo (medido y documentado, no superstición):
- registro **0x10** tiene el bit **RWAC** de apagado por software;
- registros **0x80 / 0x90 / 0x91** controlan rieles que incluyen los 3V3 **del propio P4**.
Un `write` equivocado apaga la placa y solo la vuelve a levantar el botón o un `power
cycle`. Por eso el árbol no exporta ninguna escritura al PMIC.

Lectura actual: **batería 100 %, ~4.140 mV** (`xsh$ power`). La batería es real, no un
valor fijo del driver.

Dos huecos que conviene saber (para el punto 21 del ULP y para el §"wake"):
- `PWR_KEY` = **GPIO49** está definido en `include/board.h` y **nunca se lee** en ningún
  sitio del árbol: el botón físico no hace nada en Xinu.
- No hay interrupción de wake. `TP_INT` (interrupción del touch) tampoco se usa como wake;
  el panel sondea. Todo el dash es por sondeo (`p4demoTask`), lo que explica parte de la
  carga del core0.

Regresión mínima antes de dar nada por cerrado (AGENTS §4):
`i2c scan` → `0x18` ES8311, `0x34` AXP2101, `0x36` (aparcado, no tocar), `0x38` FT6336U;
`power` → 100 % / ~4.140 mV.

---

## 3. lcd (LCD0 · ST7796 320×480 por SPI2) y framebuffer + LVGL (punto 8)

### Cómo está hoy

`device/lcd-esp32p4/st7796.c` **no tiene framebuffer**: cada operación de dibujo se
traduce a writes por SPI2 directos al panel (RAMWC del ST7796). No hay memoria de pantalla
en el kernel: no existe un buffer de 320×480×2 = 300 KB en ninguna parte. Texto con la
fuente `font8x16.h` (8×16 px ⇒ 40 columnas × 30 filas).

`control(LCD0, …)` ([include/lcd.h:45-52](../../include/lcd.h#L45-L52)):

```
0x01 BACKLIGHT(0/1)   0x02 FILL(rgb565)   0x03 RECT(&lcd_rect)   0x04 PIXEL(&lcd_pixel)
0x05 TEXT(&lcd_text)  0x06 BLIT(&lcd_blit) 0x07 DEMO             0x08 ORIENT(LCD_ROT_*)
```

### Mi recomendación para framebuffer + LVGL

La librería actual está bien y merece quedarse como **capa de bajo nivel**; lo que falta es
una capa intermedia, no reescribir el driver. Ruta en tres pasos, cada uno verificable en
placa y ninguno rompiendo el dash existente:

1. **Framebuffer en PSRAM** (coste: 300 KB; hay 31,8 MB). Una sección nueva
   `.lcdshare (NOLOAD)` junto a `.audioshare`, o un `psram_malloc()` en `lcdInit`.
   API mínima sobre lo que ya existe: `lcd_fb_clear(rgb)`, `lcd_fb_pixel(x,y,rgb)`,
   `lcd_fb_rect()`, `lcd_fb_text()` y **`lcd_fb_flush(x,y,w,h)`** que baje el rectángulo
   sucio al panel con el `LCD_CTRL_BLIT` actual. Cambiar de "dibuja en el panel" a
   "dibuja en RAM y vuelca lo sucio" es puramente aditivo: `xsh_lcd`, `p4demoTask` y
   `xsh_dash` siguen llamando a lo de siempre.
2. **`LCD_CTRL_FLUSH` nuevo** (o reutilizar `LCD_CTRL_BLIT` con un puntero a la RAM) para
   que el vuelco sea una operación del driver y no un bucle en la shell.
3. **LVGL encima del framebuffer**. `include/lvgl.h` ya existe en el árbol, pero **no hay
   ninguna librería `lvgl/`**: el header apunta a algo que no está. Hay que traer LVGL
   (9.x, `lv_conf.h` a mano: sin SO, sin malloc propio ⇒ `lv_mem_alloc` sobre
   `psram_malloc`) e injertar tres ganchos:
   - `disp_flush` → `lcd_fb_flush()` (o SPI directo si no se hace el paso 1),
   - `indev_read` → `read(TOUCH0, …)` (ya devuelve coordenadas),
   - **tick** → el opcode `LVGL_CTRL_TICK` (`include/lvgl.h:20`), alimentado desde
     `millis()`.

Riesgos que hay que decir en voz alta:
- **Sin coma flotante en el kernel** (AGENTS §3). LVGL usa float internamente; o se porta
  con `LV_USE_FLOAT=0`, o LVGL vive **fuera** del kernel, en un ELF cargado
  ([04-elf-java-apps.md](04-elf-java-apps.md)) donde el float sí es negociable.
- Rendimiento: SPI2 a la velocidad actual + vuelco de 300 KB por trama es lo que limita el
  FPS. Con frame buffer en RAM el cuello pasa a ser el bus SPI, no el dibujo. Para
  animaciones reales hace falta **LCD_CAM/DMA2D/PPA** (el P4 tiene aceleradores 2D; solo
  existen sus cabeceras HAL, `include/hal/…`, sin driver).
- El panel lo usa `p4demoTask` (creada desde `system/main.c:73`). Una GUI LVGL sería **una
  hebra nueva que se adueña del panel**, no un parche del demo.

---

## 4. touch (TOUCH0 · FT6336U a 0x38) y sd/fat (SDMMC0/SD0/FAT0)

**touch**: descriptor con `open/read/control/close`; `read()` da coordenadas ya
normalizadas. Migrado al mecanismo de descriptores y validado con el monitor del bus I2C0
activo. `touchpanel 2` = modo sin bloqueos. `TP_INT` (GPIO del panel) no se usa como
interrupción: sondeo.

**sd + fat**: microSD en **slot 0** del bloque SDHOST, el C6 en **slot 1**; ambos comparten
el controlador y se serializan con `sdmmcBusLock()`/`sdmmcBusUnlock()`, un **monitor
reentrante** (dueño + profundidad) de `system/sdmmcbus.c`. Consecuencia práctica que ya te
afectó: cualquier operación de red pesada y un `ls`/`df` grande compiten por el mismo
candado. El reparto del SD queda en `.sdshare` (8 KB, `_sd_block_page = 0x48026000`),
deliberadamente **después** de `.audioshare`.

Montaje y comprobación: `sd` (estado del bloque), `ls`, `df`, `cat`, `format`. La fase R4
blindó montaje/FAT (`df` lento, `ls`/`cat` de subdirectorios) — está documentado en el
historial de tareas y verificado en placa.

**Dos defectos de la librería FAT (`fatfs/fat_filelib.c`) que hay que conocer antes de
escribir en la tarjeta**, ambos encontrados el 2026-11-04 al portar los servidores:

1. **`fl_fopen(path, "w")` NO trunca.**  El modo `w` pone la bandera `FILE_ERASE`
   (`fatfs/fat_filelib.c:735`, y `:750` para `w+`) y **nadie en el árbol la lee nunca**: ni
   `_open_file` ni `_create_file` la consultan.  Si el nombre existe, `fl_fopen` cae en la rama
   "Write Existing" (`fat_filelib.c:791-795`) y devuelve el fichero existente con su `filelength`
   y su cadena de clusters viejos.  Escribir por
   encima menos bytes de los que había deja **la cola de los datos anteriores y la talla
   anterior en el directorio**.  Medido: un `PUT` de 4.096 B y encima uno de 100 B daba
   `ls -l` = 4.096 antes del arreglo y **100 B exactos** después.  El arreglo es siempre el
   mismo y está en el árbol: `fl_remove()` del destino **antes** de abrir (en
   `shell/httpapi.c`, ruta `PUT`, y en `shell/xsh_udpserver.c`, al finalizar la subida de
   firmware).  Cualquier camino nuevo que sobrescriba necesita la misma disciplina.
2. **No existe `fl_rename()`.**  El S3 la tenía y su comentario explica que lo que hace es
   mover la cadena de clusters a otro nombre.  Aquí no está, así que no hay forma de publicar
   un fichero de forma atómica (`.part` → definitivo): el sustituto es
   `fl_remove(dest)` → `fs_copy(src,dest)` → **verificar la talla reabriendo el nombre final**
   → `fl_remove(src)`, todo dentro de un único `fatLibLock()/fatLibUnlock()` (el monitor de la
   librería es reentrante para el dueño, `system/lock.c`, así que el candado envolvente es
   correcto aunque cada `fl_*` vuelva a entrar).  Si el corte cae en mitad de la copia, la
   verificación falla y se borra el destino: nunca queda un fichero parcial con el nombre
   definitivo, pero **la talla verificada no garantiza el contenido** -- de ahí que el CRC32
   siga pendiente ([05 §6](05-roadmap-y-conf.md) Pieza 1).

Y el candado que no se ve desde la shell: cada escritura FAT va por el **mismo monitor SDHOST**
que el WiFi (`sdmmcBusLock()`), así que una subida por UDP y un `ls` grande se serializan.  Es
lo que pone la punta de latencia que se midió en el ack de un bloque (0,04 s de media, >6 s en
la peor punta, [03 §7.5](03-red-y-wifi.md#udpserver)).
