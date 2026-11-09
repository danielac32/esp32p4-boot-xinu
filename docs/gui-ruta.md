# Ruta del entorno grafico (2026-11-06)

Objetivo: una tablet.  Un entorno grafico que carga apps ELF, con un estandar
de diseno (estructura tipo HTML, layouts listos, capa de abstraccion) y con
rendimiento de juego simple.  Este documento responde a las preguntas de hoy
--por que va lento, por que los ecos, que pasa con LVGL y el touch-- y parte el
trabajo en fases pequenas, cada una validada en placa antes de la siguiente.

## 1. Por que se ve lento (medido, no opinado)

**El reloj del kernel corre un 13% lento.**  Medido hoy con `syscheck` en
placa: el mtime cuenta 360.061 cuentas por ms de kernel cuando `clkfreq`
declara 360.000.000, y las lineas de heartbeat (1/s del kernel) llegan cada
~1,13 s reales.  O sea: el PLL del core va a ~319 MHz reales, declarado 360.
Todo lo que va por `millis()`/`sleep()` --animaciones, gestos, temporizadores--
corre un 13% por debajo de lo que dicen los numeros.  (La medicion anterior
daba 1,26; conviene repetirla en cada fase, la cifra cambia con el estado.)

**El cable de la pantalla manda.**  El SPI2 va a 80 MHz en modo directo
(`device/lcd-esp32p4/lcdSpi.c`, `lcdSpiSetClock` con `equ_sysclk`), el maximo
que admite el ST7796.  Un fotograma completo son 320x480x2 = 307.200 bytes:
30,7 ms SOLO de cable.  Y el flush es SINCRONO por sondeo: `lvgl_flush_cb`
(`device/lvgl/lvglport.c`) llama a `control(LCD0, LCD_CTRL_BLIT)` y la CPU se
queda girando sobre `CMD.USR` en rafagas de 64 bytes hasta vaciar el FIFO.
Resultado: render y blit se serializan (los dos buffers de dibujo no solapan
nada si el flush bloquea) y durante el blit la CPU no hace otra cosa.  Un
cambio de pantalla completa no puede bajar de ~35-40 ms => 25 fps de techo
teorico, ~15-20 en la practica.

**El refresco de LVGL tiene su propio techo.**  `LV_DISP_DEF_REFR_PERIOD` es
30 ms de reloj LVGL (=34 ms reales con el factor de arriba): hasta los cambios
pequenos pagan ese compas.  La hebra residente `lvglui` pregunta cada 5 ms, asi
que la estructura esta bien; el techo es el periodo, no la hebra.

**El touch ya esta integrado y no es el problema.**  `lvgl_touch_cb` sondea el
FT6336 por I2C0 dentro de cada `lv_timer_handler()`; el muestreo es en vivo y
barato.  Lo que falta no es integrarlo, es que el resto del bucle no lo
arrastre.

**La galeria tiene su propio coste sin medir.**  El JPEG se decodifica por
software (tjpgd dentro de LVGL) y el coste real de las miniaturas no esta
medido (tarea #100).  Antes de redisenar la galeria hay que saber si duele el
decode, el blit o el montaje.

**Primera pasada cara.**  Medido hoy: `plantilla` (hoy `apps/ejemplo/`) recien
subida tardo 7,4 s en
un demo de 3 s de kernel (primera lectura FAT + primer refresco completo); en
reposo el mismo demo tarda 3,4 s (el 1,13x justo).  La primera pasada afecta al
arranque del launcher y al abrir cada app; se mide en R1.

## 2. Por que los ecos del reproductor

Dos causas, una ya casi demostrada:

* **El ES8311 es mono y reproduce SOLO el slot derecho.**  Lo dice el propio
  driver del codec: el ADC vuelca en el slot izquierdo y el DAC reproduce el
  slot contrario, el derecho.  Con una cancion estereo cuyo canal derecho lleva
  delay/reverb, lo que sale es justo "un eco".  Y el tono de 440 Hz grabado
  SOLO en el canal izquierdo (`/mus/lsolo.mp3`) se oyo "nada" --coherente con
  que el DAC no toca ese slot.  La prueba que lo cierra es el tono derecho
  (`/mus/rsolo.mp3`), y el arreglo es de una linea: downmix estereo->mono
  (media de L y R en los dos slots) antes del anillo, en la ruta MP3.
* **Vueltas del anillo bajo carga.**  En headless NO hay vueltas (`huecos=1`,
  cola al 93% durante 5 s).  Con la UI viva no se ha medido nunca: si el blit
  sincrono deja al escritor sin CPU mas de 341 ms (la talla del anillo), el DMA
  repite audio viejo y eso SI es eco entrecortado.  Se mide en R2.

Y un tercero menor: el volumen del DAC no lo pone nadie al abrir (el registro
0x32 arranca a 0 = -95,5 dB).  `mp3test` ya lo hace; las apps deben hacerlo
igual al abrir `/dev/audio0`.

## 3. La ruta, fase a fase

**R0 -- la base del SDK (HECHO hoy, validado en placa).**  `include/sdk.h`: UNA
cabecera que reune todos los exports (kernel, LVGL device y libreria,
audio/touch/lcd/power/espeth, FAT) mas las tres ayudas de `apps/lib/sdk.c`
(bateria, wifi, dedo).  `apps/ejemplo/` (la antigua `apps/plantilla/`): el
esqueleto canonico de app (pinta
sobre el screen global con `lv_obj_clean` al entrar, bucle de dedo con boton y
X para salir, modo `demo` sin dedos).  Makefile de 4 lineas, sin generadores:
`app.mk` sigue comprobando contra `system/exports.c`.  Probado en placa: carga,
pinta (captura por `/api/shot`), sale limpio.

**R1 -- los numeros del refresco.**  Una app `bench` que mida en placa: blit
completo por `LCD_CTRL_BLIT`, refresco LVGL con pantalla llena, decode de una
miniatura JPEG, lectura del touch, `sleep(50)` bajo carga, primera pasada de
FAT de un ELF.  Salida: la tabla de costes que decide que se toca en R3 y R5.
(Recoge las tareas #141, #80 y #100.)

**R2 -- audio correcto.**  Confirmar el slot con `/mus/rsolo.mp3`; downmix
estereo->mono en la ruta MP3; volumen por defecto al abrir; medir
`huecos` con la interfaz delante.  Al final, el reproductor suena a la cancion,
no al canal.

**R3 -- el salto de rendimiento.**  Flush asincrono: que el blit del GPSPI2
vaya por DMA (out_ch) y `lv_disp_flush_ready()` se llame al terminar, de modo
que LVGL renderice la franja siguiente MIENTRAS el cable vacia la anterior y la
CPU quede libre durante el blit.  Con 80 MHz de cable el techo sube a 30+ fps
de pantalla completa.  Con precaucion: el DMA de SPI2 es el unico cambio
en el device y se valida con el banco de pruebas del LCD.

**R4 -- el estandar tipo HTML (ui-kit).**  Sobre LVGL, una capa fina declarativa:
contenedores de columna/fila con peso y margen (el flexbox minimo), estilos por
tokens (la paleta ya esta en `ejemplo.c`), widgets estandar (boton, lista,
imagen, barra, puntos).  La plantilla (hoy `apps/ejemplo/`) pasa a montarse
con el ui-kit y sirve de
demo.  Sin motor web: HTML es la metafora, no el codigo.

**R5 -- rehacer las apps sobre el ui-kit.**  Galeria (miniaturas cacheadas y
paginadas, primero con los numeros de R1), musica (downmix de R2 + contador de
huecos visible), voz.  Una por una, cada una validada en placa.

**R6 -- estabilidad.**  El trap #142 de musica (`mepc=0x3d094540` fuera de toda
imagen, `ra` basura: parece una direccion de retorno corrompida, a medir con el
volcado de traps y el censo del cargador).  Lanzar una app al arranque si hace
falta (no hay launcher: el comando `elf` es el unico camino, y `halt()` ya
reinicia la placa).

## 4. Lo que NO cambia

* **LVGL se queda.**  El renderer software RGB565 es el correcto para 320x480;
  el problema esta en el camino al panel (flush sincrono) y en el reloj, no en
  la libreria.  Cambiarla ahora seria tirar la fase sin haber medido el
  enemigo.
* **El modelo kernel + apps ELF se queda.**  Hebra residente `lvglui` para el
  refresco, pantalla global compartida (cada app limpia al entrar), SDK unica
  para escribirlas.
* **Las reglas del ABI se quedan** (sin estaticas escribibles, candado para
  LVGL, nada de `lv_timer_handler()` desde apps): cada una tiene detras un
  fallo medido y documentado en la app de ejemplo
  `examples/p4-elfsdk/apps/ejemplo/`.
