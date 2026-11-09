/*
 * lv_conf.h — configuración de LVGL v8 (8.3.11) para Xinu en el ESP32-P4.
 *
 * LVGL está vendorizado en lvgl/ y se compila como componente propio (ver
 * lvgl/Makerules).  Todo lo que NO se define aqui cae al valor por defecto de
 * lv_conf_internal.h, asi que esta lista es solo lo que este puerto cambia.
 *
 * Para que lv_conf_internal.h encuentre este fichero, platformVars pone
 * -I$(TOPDIR)/lvgl en la ruta de includes.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* ---- color ---------------------------------------------------------- */

/* El panel ST7796 es RGB565 y el device lcd mueve uint16_t tal cual:
 * LCD_CTRL_BLIT ya manda el byte alto primero (lcd.h), asi que el intercambio
 * de bytes que LVGL ofrece para SPIs que exigen el orden inverso no hace
 * falta aqui. */
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

/* ---- memoria -------------------------------------------------------- */

/* El heap de LVGL va SOBRE PSRAM, no en .bss.  No es un detalle de estilo:
 * con LV_MEM_CUSTOM 0 la libreria reserva su pool como array estatico, y el
 * .bss de este puerto enlaza en HP_L2MEM (ld.script), que son 768 K
 * COMPARTIDOS con las pilas de las hebras --ahi es donde `create' saca los
 * JVM_STK de cada hebra java y donde el heap deja medidos 221.912 B--.  Los
 * objetos, estilos, fuentes y buffers vivos de LVGL salen en cambio de los
 * ~33 MB de PSRAM, y el presupuesto de L2MEM no se mueve. */
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE "memory.h"
#define LV_MEM_CUSTOM_ALLOC   psram_malloc
#define LV_MEM_CUSTOM_FREE    psram_free
#define LV_MEM_CUSTOM_REALLOC psram_realloc

/* ---- tick ----------------------------------------------------------- */

/* Sin reloj propio: el device lvgl avanza el tick de LVGL desde millis() de
 * Xinu cuando la app llama a LVGL.tick() (LVGL_CTRL_TICK). */
#define LV_TICK_CUSTOM 0

/* ---- trazas y asserts ----------------------------------------------- */

/* Sin trazas: LV_USE_LOG pide vfprintf y el puerto no lleva la libc de el
 * host.  Los asserts de puntero y de asignacion si se quedan: cuestan poco y
 * un objeto mal creado se convierte en un salto a 0 (mcause=2 en placa), que
 * es mucho peor de diagnosticar que un halt en lv_obj.c. */
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_STYLE 0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ 0

/* ---- bloques que no aplican a esta placa ---------------------------- */

/* Sin GPU (el P4 no trae 2D para el LP core y los aceleradores son de otros
 * SoCs), sin SDL, sin sistema de ficheros propio de LVGL y sin las demos de
 * la libreria: la demo y la UI son del proyecto. */
#define LV_USE_GPU_NXP_PXP 0
#define LV_USE_GPU_NXP_VG_LITE 0
#define LV_USE_GPU_SDL 0
#define LV_USE_SDL 0
#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0

/* LV_USE_FS_* se queda a 0 (por defecto): los backends que trae la libreria
 * son POSIX, stdio y FatFs, y ninguno es esta FAT (fatfs/ sobre fl_fopen).  El
 * puerto registra su propio driver contra fat_filelib --letter 'S'--, en
 * device/lvgl/lvglfs.c, y es lo que usan los decodificadores de imagen que
 * cargan por ruta. */

/* ---- decodificadores de imagen -------------------------------------- */

/* JPEG con el decodificador que trae LVGL: src/extra/libs/sjpg (TJpgDec,
 * elm-chan, en el arbol vendorizado).  Decodifica por teselas, asi que el pico
 * de memoria no es la imagen entera sino una franja, y no depende de nada
 * externo: la regla de bare-metal sigue en pie (nada de esp-idf ni de libjpeg
 * del host).  Con su driver de filesystem propio, una app java puede poner
 * LVGL.img(scr, "/img/foto.jpg", ...) y LVGL lee y decodea solo.
 *
 * PNG (LV_USE_PNG, lodepng) se queda fuera: cada decodificador es .rodata y
 * texto en la ranura del kernel, y de momento ningun asset del proyecto es
 * PNG. */
#define LV_USE_SJPG 1

/* ---- fuentes -------------------------------------------------------- */

/* Aspecto retro: UNSCII (fuente de terminal de 8x8 / 8x16, donada a LVGL
 * desde el proyecto UNSCII) es la que da el aire de terminal de los 80.  Es
 * la fuente POR DEFECTO de todo el arbol, asi que cualquier label de una app
 * sale ya en pixel sin que la app tenga que pedir nada.
 *
 *   - unscii_16: cuerpo normal (legible en 320x480, ~8 px por caracter).
 *   - unscii_8:  rotulos pequenos y la linea de pistas del dock.
 *
 * Montserrat 26 se queda porque los titulos grandes de las apps (galeria,
 * musica) la piden explicitamente; la 14 se apaga: era la de por defecto y
 * ya no hace falta. */
#define LV_FONT_MONTSERRAT_14 0
#define LV_FONT_MONTSERRAT_26 1
#define LV_FONT_UNSCII_8 1
#define LV_FONT_UNSCII_16 1
#define LV_FONT_DEFAULT &lv_font_unscii_16

#endif /* LV_CONF_H */
