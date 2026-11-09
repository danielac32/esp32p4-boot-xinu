# Pila gráfica Java aparcada (2026-11-05)

El kernel del P4 queda **sin interfaz gráfica**: arranca, sube la red y abre la
shell, y nadie dibuja en el panel.  Todo lo que hacía la GUI —JVM, natives,
LVGL, SDK java, launcher y apps— está aquí, movido sin tocar una línea de su
interior, para poder volver a montarlo cuando se decida el modelo de app
(JVM/Java vs. ELF con `elfload`).

Motivo del aparcamiento: cinco apps que "no abren" (en realidad arrancan y
pintan, pero la pintura acaba mezclada con la del launcher), traps en el cierre
de hebras java, y un presupuesto de widgets global que se agota.  Se para de
iterar sobre eso y se conserva el trabajo.

---

## 1. Qué está aquí y de dónde venía

Las rutas de abajo son **relativas a la raíz del árbol** (`xinu-p4.2/`) en el
momento de aparcar; dentro de `parked/java-gui/` se conserva la misma forma,
así que reactivar es mover de vuelta carpeta por carpeta.

| Ruta aparcada | Ruta original | Qué es |
|---|---|---|
| `jvm/` | `jvm/` | núcleo portable del JVM (bytecodes, class parser, heap, GC, `java_lib.c` con la tabla de natives) |
| `device/jvm/` | `device/jvm/` | costura de plataforma: `jvmglue.c` (arranque de `jvmTask`), `jvm_io_target.c`, y las natives `native_lvgl/hw/io/fat/net/sys.c` |
| `device/lvgl/` | `device/lvgl/` | puerto LVGL al panel: `lvglport.c` (flush por `LCD_CTRL_BLIT`, táctil por `TOUCH_CTRL_READ`, hebra residente `lvglui`, monitor `lvglmon`, espejo de fotograma `lvgl_shadow`) y `lvglfs.c` (LV_FS sobre FAT) |
| `lvgl/` | `lvgl/` | la librería LVGL v8.3.11 y su `lv_conf.h` (Montserrat 14/26 con los glifos `LV_SYMBOL_*`, `LV_COLOR_DEPTH 16`) |
| `java/sdk/` | `java/sdk/` | SDK java: declaraciones de `esp32p4.LVGL/HW/FAT/Net/Power/Sys` + `app.mk`, las reglas comunes de cada app |
| `apps/java/` | `apps/java/` | `workspace` (launcher) y las apps `ajustes`, `galeria`, `info`, `monitor`, `musica`, más las sondas `NatTest` y `JpgTest` |
| `include/lvgl.h` | `include/lvgl.h` | header del *device* lvgl (`lvglInit`, `lvglControl`, `lvgl_lock/unlock`) |
| `shell/xsh_java.c` | `shell/xsh_java.c` | comando `java <clase>`, lanza una `jvmTask` con `JVM_STK` |
| `examples/p4shot.py` | `examples/p4shot.py` | captura de pantalla por telnet leyendo `lvgl_shadow` |

## 2. Cómo reactivar

### 2.1 Mover de vuelta

```sh
cd /home/daniel/xinu-p4.2/parked/java-gui
mv jvm ../../jvm
mv device/jvm device/lvgl ../../device/
mv lvgl ../../lvgl
mv java ../../java
mv apps/java ../../apps/
mv include/lvgl.h ../../include/
mv shell/xsh_java.c ../../shell/
mv examples/p4shot.py ../../examples/
```

### 2.2 Volver a enganchar al build

**`compile/platforms/esp32p4/platformVars`** — tres líneas:

```make
INCLUDE  += -I$(TOPDIR)/lvgl        # lv_conf_internal.h busca lv_conf.h aqui
INCLUDE  += -I$(TOPDIR)/jvm         # simple_jvm.h, jvm_map.h, jvm_io.h
APPCOMPS := shell apps fatfs network mailbox lvgl
DEVICES  := tty uart-esp32p4 psram nam lcd-esp32p4 sdmmc-esp32p4 \
            sdblock-esp32p4 fat gpio i2c touch power lvgl audio \
            sdio-esp32p4 espeth-esp32p4 raw udp tcp telnet jvm
```

El orden de `INCLUDE` importa: `-I$(TOPDIR)/include` va primero, así que
`#include <lvgl.h>` es el header del **device** y la librería se alcanza por la
ruta larga `lvgl/lvgl.h`.

**`compile/platforms/esp32p4/xinu.conf`** — el bloque del device y su entrada
en el devtab (declarado **después** de `LCD0` y `TOUCH0`, porque `lvglInit()`
registra el flush contra `control(LCD0,...)` y el puntero contra
`control(TOUCH0,...)`):

```
lvgl:
	on HARDWARE -i lvglInit     -n lvglControl
```
```
LVGL0     is lvgl     on HARDWARE
```

`system/conf.c` y `include/conf.h` **no se editan a mano**: los regenera
`config/config` desde `xinu.conf` en cada `make`, y recalculan las posiciones
del devtab solos.

**`shell/Makerules`**: `C_FILES += xsh_java.c`.
**`shell/shell.c`**: `{"java", FALSE, xsh_java},` en `commandtab`.
**`include/shell.h`**: los prototipos `xsh_lvgl` y `xsh_java`.

**`include/thread.h`** — las dos cotas de pila, con la medida que las justifica
(pico de `java JpgTest` 5.956 B; pico del launcher 10.884 B de 16.384; el
kernel no deja página guardada bajo la pila, así que pasarse pisa el bloque
contiguo del heap de L2MEM):

```c
#define JVM_STK         16384   /* hebra que ejecuta Java */
#define JVM_STK_LAUNCH  32768   /* launcher residente */
```

**`system/main.c`** — el panel se lanzaba como tarea residente, no en el hilo
de `main` (el bucle del launcher no vuelve nunca y la shell tiene que nacer):

```c
extern thread jvmTask(char *path, int argc, char **argv);
...
int child = create(jvmTask, JVM_STK_LAUNCH, INITPRIO, "workspace", 3,
                   (char *)"/java/workspace.class", 0, 0);
...
ready(child, RESCHED_YES);
```

Ojo: `main()` usa la bandeja de mensajes para esperar la muerte de `netboot`
antes de crear la shell.  El `recvclr()`/`disable()`/`restore()` que queda hoy
es justo el que sostenía ese `ready(child)`: si se restaura el `create`, hay que
devolver también el `ready(child, RESCHED_YES)` entre `disable()` y `restore()`.

### 2.3 Compilar y subir las apps

```sh
cd apps/java/workspace && make && make send      # deja /java/workspace.class
cd apps/java/galeria  && make && make send       # deja /java/apps/galeria/galeria.class
```

`make send` usa la variable `BOARD` de `java/sdk/app.mk`, que **sigue poniendo
la IP vieja `192.168.1.146`** (mismo residuo que `sd/Makefile`); con la placa en
`172.16.0.117` hay que pasarla: `make send BOARD=172.16.0.117`.

### 2.4 Capturas de pantalla

`examples/p4shot.py` lee el espejo de fotograma por telnet (puerto 23) y saca un
PNG.  La dirección del símbolo cambia con cada build del kernel:

```sh
nm compile/xinu.elf | grep lvgl_shadow          # -> p.ej. 4ff432b8
python3 examples/p4shot.py /tmp/placa.png --sym 4ff432b8
python3 examples/p4shot.py /tmp/fila2.png --sym 4ff432b8 --y0 228 --alto 112
```

El cuadro completo tarda ~60 s (307.200 B en volcados ASCII de 16 B por línea);
una banda sale en ~10 s y es lo que sirve para iterar.  Requiere `PIL` del
`python3` del sistema (no está en el entorno `/home/daniel/esp-idf-py`).

## 3. Estado en que se deja

**Funcionando y medido en placa**: flush por SPI2 con dos franjas de 320x40,
táctil con gestos, monitor `lvglmon` que serializa la API, hebra `lvglui`,
JPEG por sjpg sobre `lv_fs`+FAT, apagado de pantalla por reposo (2 min) con
despertar que se traga el primer dedo, launcher con rejilla 3x2, tarjetero de
segundo plano, banner de app al frente y cierre cooperativo por `MSG_QUIT`.

**Roto o sin cerrar** (tareas del seguimiento, en el orden en que duele):

- **#118 / #88 — traps al morir una hebra java.** Firma vista:
  `mcause=0x38000005 mepc=0x40006e58 mtval=0x4 ra=0x40023d38 a0=0xc` y la
  variante `mepc=0x0`.  Escozina de pila pisada, no un `double free` sencillo.
  Se probó subiendo el launcher a 32 KB (`JVM_STK_LAUNCH`) y blindando
  `psramBad()` contra el `%s` inválido; con eso, abrir y minimizar `ajustes` y
  `monitor` dejó de trapar en la pasada #1625, pero no está cerrado.
- **#114 — tabla de handles global.** `WIDGET_MAX` (160) es **una** tabla para
  el launcher y todas las apps; cada ficha gasta 4 handles.  Una app minimizada
  conserva su UI hasta que el launcher le manda `MSG_FREE` (`alFrente()`), y con
  cuatro apps en segundo plano la siguiente abría a medias (la galería cortaba
  su rejilla en la celda 4).
- **Mezcla de pinturas entre launcher y app.** En la última captura limpia se
  veía el texto de `monitor` (`usada 654 KB de 32580 KB`) pintado encima de la
  fila 1 de la rejilla, y los dos relojes del launcher con valores distintos.
  El espejo de fotograma **no es un fotograma**: es la unión de todas las áreas
  que LVGL invalidó desde el último refresco completo, así que parte de eso son
  residuos del lienzo; pero la superposición del texto de la app sobre la rejilla
  es real y es lo que hace parecer que "la app no abre".
- **#116** lista de redes WiFi reales en `ajustes`; **#117** MP3 de verdad con
  minimp3 (nada de esp-idf); **#115** gesto de salida uniforme en las 5 apps.
- El reloj del panel **no es la hora**: es `HW.millis()` (`MM:SS` de arranque),
  porque no hay native de RTC.  El RTC del AXP2101 se puede leer (solo lectura).

**Verificado antes de aparcar**: `make clean && make` del árbol sin GUI enlaza
(#1627) y `nm compile/xinu.elf` no tiene ni un símbolo `lvgl` ni `jvm`;
`system/conf.c` e `include/conf.h` regenerados salen sin `LVGL0` ni `NLVGL`.

## 4. Residuos que se dejan a propósito

- `include/syscall.h` conserva los `XINU_LVGL_*` y los envoltorios
  `sys_lvgl_*()`: son solo declaraciones, nunca tuvieron lado del kernel (las
  natives las llamaba la VM directamente), y quitarlos del `enum` desplazaría la
  numeración de los ecalls que vienen detrás.
- La SD conserva `/java/workspace.class` y `/java/apps/*`; el kernel ya no los
  carga, así que son espacio muerto hasta que se reactive o se borren.
- `shell/p4demoTask.c` (el panel en C, sin LVGL) sigue compilándose y sigue sin
  lanzarse.
- `docs/port-esp32p4/` conserva las notas de la fase GUI; no se movieron porque
  son historia del puerto, no código.
