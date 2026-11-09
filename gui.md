# GUI sobre apps ELF cargadas

Estado del camino *kernel con cargador ELF + LVGL + apps en C*, a 2026-11-05.
Es un documento de trabajo: qué se hizo, qué hay ahora, cómo se compila y qué
no funciona.  El SDK mínimo (sin cabecera de exports: solo includes del
kernel) está en `examples/p4-elfsdk/apps/ejemplo/`; la decisión de
hacer el launcher en C y no en Java, en
[docs/port-esp32p4/04-elf-java-apps.md](docs/port-esp32p4/04-elf-java-apps.md).

## 1. El cargador ELF (`system/elfload.c`)

El kernel carga y ejecuta ET_REL (lo que produce `ld -r`) desde la SD.  Pasos,
medidos en placa y documentados en la cabecera del fichero:

1. comprueba magic / ELFCLASS32 / EM_RISCV / **ET_REL**;
2. coloca las secciones `SHF_ALLOC` en un bloque de PSRAM respetando
   `sh_addralign`, y pone a cero la `.bss`;
3. resuelve los símbolos UND por nombre contra
   [system/exports.c](system/exports.c) — 1133 entradas: `printf`, `memset`,
   `psram_malloc`, `millis`, `control`, unas mil de LVGL;
4. aplica los cinco tipos de relocación que salen del SDK
   (`R_RISCV_32`, `HI20`, `LO12_I`, `CALL`, `CALL_PLT`) y **aborta con
   cualquiera otra** antes de saltar;
5. salta por la **ventana directa** (0x88……) y, antes de copiar, hace
   writeback+invalidate de L1-D y L2 sobre el bloque: sin eso, las líneas sucias
   que dejó el kernel se vacían cuando quiere y escriben encima del código ya
   parcheado (fue el `trap en la coda de crt0`, medido con la imagen #1639);
6. `elf_call()` arma `argc`/`argv` con su `NULL` final y llama a
   `int main(int argc, char *argv[])` — C estándar, sin trampolines (auipc+jalr
   cubre los 1,2 GB que hay de PSRAM directa al texto del kernel).

API: `elf_load()` [system/elfload.c:316](system/elfload.c#L316),
`elf_load_file()`:741, `elf_call()`:810, `elf_release()`:875,
`elf_run_file()`:886.  Lado shell: `elfrun`
([shell/xsh_elfrun.c](shell/xsh_elfrun.c)).

**Arranque**: `main()` crea `workspaceTask` (32 KB de pila) y esa hebra hace un
único `elf_run_file("/elf/workspace.elf", 0, NULL, &codigo)`
([system/main.c:85](system/main.c#L85)).  Desde tu cambio de hoy, eso va
**antes** del `netboot`.  El launcher no retorna: si retorna, la hebra muere y
sube la shell.

**Lo que NO está hecho (etapa E2)**: no hay `elf_spawn` / `elf_cancelar`.  Una
app se lanza **anidada** dentro de `elf_call()` del launcher, o sea en la misma
hebra, y el launcher espera.  Consecuencias directas: RECIENTES no tiene
miniaturas vivas que enseñar, no hay X que matar, y la app dueña de la pantalla
es la que está en primer plano por construcción.

## 2. LVGL en el kernel

LVGL **8.3.11** ([lvgl/lvgl.h:16](lvgl/lvgl.h#L16)), compilado como componente
residente, con el puerto en [device/lvgl/lvglport.c](device/lvgl/lvglport.c):

- hebra residente `lvglui` (creada por `lvglInit`, pila de 8 KB) que llama a
  `lv_timer_handler()` cada 5 ms; **esa hebra es la única que refresca**.
- candado `lvglmon` expuesto como `lvgl_lock()/lvgl_unlock()`: cualquier ráfaga
  de widgets desde otra hebra va entre esas dos llamadas.
- dos buffers de dibujo de 320×40 px (`DRAW_LINES 40`), en PSRAM por
  `psram_malloc`; modo parcial de LVGL, flush **síncrono**.
- táctil FT6336 por sondeo dentro de `lvgl_touch_cb` (TP_INT cableado, sin usar).
- `control(LVGL0, LVGL_CTRL_IDLE, ms)`: el kernel apaga el backlight tras el
  silencio y **se traga el primer dedo** del despertar (`wakeSwallow`), más el
  `lv_obj_invalidate(lv_scr_act())` al encender porque durante el apagado el
  flush tiraba los buffers.
- letra de filesystem **'S'** sobre fat_filelib ([device/lvgl/lvglfs.c](device/lvgl/lvglfs.c))
  y `LV_USE_SJPG 1` ([lvgl/lv_conf.h:91](lvgl/lv_conf.h#L91)): los JPEG entran
  con `lv_img_set_src(img, "S:/thm/t01.jpg")`, decodificados por franjas.
- espejo del fotograma completo (`lvgl_shadow`, 307.200 B en PSRAM) copiado fila
  a fila en el flush; lo lee `examples/p4shot.py` para sacar capturas por HTTP.

### Coste real de un refresco

| magnitud | valor | de dónde sale |
|---|---|---|
| SCLK del panel | **20 MHz, una línea** | [device/lcd-esp32p4/lcdhw.h:84](device/lcd-esp32p4/lcdhw.h#L84) |
| frame completo por el cable | **122,9 ms** | 320×480×2 B × 8 / 20 MHz |
| tamaño de ráfaga | **64 B** (4800 por frame) | [lcdhw.h:138](device/lcd-esp32p4/lcdhw.h#L138), [lcdSpi.c:112](device/lcd-esp32p4/lcdSpi.c#L112) |
| overhead por ráfaga | ~5-10 µs de CPU (16 palabras W0..W15 + polling del bit `USR` + byte-swap píxel a píxel desde PSRAM) | [st7796.c:412](device/lcd-esp32p4/st7796.c#L412) |
| **frame completo real** | **~150-180 ms → techo ~8 fps** | suma de lo anterior |
| una franja de 320×40 | ~10 ms de cable | 25.600 B |

De ahí que la fluidez dependa sobre todo del **área sucia**: repintar una
etiqueta es ~1 ms; remontar la pantalla entera son ~150 ms.  Las palancas, por
orden de rendimiento conseguido por trabajo, están en la tabla de la conversación
del 2026-11-05 (área sucia, fuera sombras/degradados, 40 MHz, GDMA + flush
asíncrono, buffers en SRAM, espejo conmutable).  Ninguna está hecha; la medida
que falta es la tarea #80.

## 3. Apps en C: `examples/p4-elfsdk`

```
examples/p4-elfsdk/
  sdk.mk        banderas (compartidas por librería y apps; NO negociables)
  app.mk        cómo se enlaza una app y las dos comprobaciones
  crt0.S        el _start que recibe a0=argc, a1=argv y llama a main
  mkexports.py  regenera system/exports.c desde el kernel
  README.md     cada bandera con el fallo medido que justifica
  apps/
    lib/        panel.c/h  (paleta, barra de estado, dock, tarjetas, iconos,
                            gestos)  +  sfx.c/h  (WAV: tocar y grabar)
                            -> libpanel.a
    workspace/  el launcher
    galeria/    rejilla de miniaturas + visor
    musica/     lista de /mus + reproductor
    voz/        grabador de /rec + reproducción
    hola/       la prueba mínima de exports
```

### Cómo se compila una app

```
cd examples/p4-elfsdk/apps/musica
make            # gcc -> crt0.o + musica.o, ld -r contra libpanel.a, y check
make check      # solo la comprobacion
make clean
```

`check` es el motivo de existir del SDK: saca los nombres de
`system/exports.c`, lista los `UND` del `.elf` con `nm -u` y **falla si falta
cualquiera**; después mira las relocaciones y **falla si aparece
`LO12_S`/`HI20_S`/`TLSDESC`/`ALIGN`/`SET`**.  Un símbolo que falta se descubre
aquí, no en placa con `mcause=5`.

Salida esperada (música, hoy):

```
musica: 71 externas, todas en la tabla del kernel
        476 R_RISCV_CALL_PLT   78 R_RISCV_HI20   78 R_RISCV_LO12_I
```

### Las dos reglas del ABI que condicionan todo el código

1. **Nada de estáticas escribibles de ámbito de fichero.**  Con
   `-msmall-data-limit=0` no hay `.sdata` relativa a `gp`, pero un
   `static int contador` sigue pudiendo salir por `R_RISCV_LO12_S`, que el
   cargador no aplica.  Todo el estado vive en la pila de `main` y se pasa por
   puntero (por eso `panel_*` y `sfx_*` no guardan nada).  Los `static` que hay,
   `const`.
2. **Nunca `lv_timer_handler()` desde una app**, y las ráfagas de widgets entre
   `lvgl_lock()/lvgl_unlock()`.  Matiz: `control(LVGL0, ...)` coge el monitor
   por su cuenta, así que va **fuera** del candado.

Y una tercera que es de PSRAM, no del ABI: el PCM se mueve por la ventana
directa (`SFX_DIRECT`/`REC_PCM_DIRECT` = +0x40000000 sobre el puntero de
`psram_malloc`), porque barridos grandes por la vía con cache dejan líneas de
L2MEM leídas a cero.  `psram_free` recibe siempre el puntero original.

### Cómo se sube a la placa (sin reflashear nada)

```
curl -T musica.elf "http://<ip-placa>/api/file?path=/elf/musica.elf"
```

El launcher escanea `/elf/*.elf` en el arranque y con el arrastre hacia abajo
desde la barra de estado; se excluye a sí mismo.  Cambiar la interfaz es volver
a `make` y mandar el `.elf`.

## 4. Estado de las cuatro imágenes

| imagen | estado |
|---|---|
| launcher | rejilla con scroll global, barra de estado (WiFi + batería leídas del kernel), dock HOME/TAREAS, apagado a los 2 min **verificado en placa** (`screenOff=1`, `idleOffMs=120000`).  Mal: el botón HOME sale resaltado cuando ya estás en home; las miniaturas de background no existen (E2); el dock está lleno de `hola2..hola9.elf` que son residuo de pruebas. |
| galería | monta rejilla 3×3 de `/thm` (96×96) y visor de `/img` (300×400).  **Prácticamente inservible**: la imagen grande no se ve bien y el `sjpg` por franjas es lento. |
| música | lista de `/mus` con duración, barra de progreso, anterior/pausa/siguiente.  **No funciona bien** y le falta lo pedido: la línea de espectro no existe.  El MP3 no suena (no hay decodificador con pérdida en el driver — tarea #117); solo WAV PCM 16 bits. |
| voz | lista de `/rec`, selector 5/10/20 s, nivel por pico y guardado en `/rec/gNN.wav`.  **No es lo pedido**: se pidió botón mantener=grabar / soltar=parar y reproducir. |

El dedo no se puede validar desde el PC: las gestas de sensibilidad (toque vs
scroll, despertar tragado) necesitan pasada de dedo humana.  La referencia que
sí funciona y conviene mirar antes de seguir es
[shell/p4demoTask.c](shell/p4demoTask.c) — 2038 líneas que pintan **a mano sobre
el framebuffer**, sin árbol de widgets, y por eso se ve fluida.

## 5. Pendientes reales

- **E2**: `elf_spawn`/`elf_cancelar`/`elf_cancelada` en `system/elfload.c`,
  exports del cargador y `elfspawn`/`elfapps` en la shell → hebra y pantalla por
  app, RECIENTES de verdad con X.  Toca kernel: hay que reflashear.
- **Rendimiento**: medir (tarea #80) y luego decidir entre 40 MHz, GDMA y
  repintado en situ.
- **MP3 real** (#117) y **espectro** (haría falta una FFT en el device o en la
  app).
- Documentación: el §7 de [AGENTS.md](AGENTS.md) y este mismo documento
  describen la etapa anterior; la referencia canónica del SDK es ahora
  `examples/p4-elfsdk/apps/ejemplo/`.
