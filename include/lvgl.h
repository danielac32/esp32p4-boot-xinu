/**
 * @file lvgl.h
 *
 * LVGL device.  The vendored LVGL v8 library lives in lvgl/ (built as a
 * component with its own static heap); this device wires its display port to
 * the ST7796 LCD (LCD0) and its input port to the touch panel (TOUCH0).  Each
 * use is serialized with a monitor.
 *
 * Depends on LCD0 and TOUCH0 being initialized first (devtab order).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _LVGL_H_
#define _LVGL_H_

#include <device.h>

/* control(LVGL0, CMD, arg1, arg2) */
#define LVGL_CTRL_INIT  0x01 /**< vaciar el screen activo (sin lv_init)   */
#define LVGL_CTRL_TICK  0x02 /**< avanzar el tick de LVGL desde millis()  */
#define LVGL_CTRL_TASK  0x03 /**< ejecutar lv_timer_handler() una vez     */
#define LVGL_CTRL_READY 0x04 /**< OK si el device esta inicializado       */
#define LVGL_CTRL_IDLE  0x05 /**< arg1 = ms de inactividad para apagar el
                              * backlight; 0 = no apagar nunca            */

/* Los dos controles de arriba los usa la hebra residente (lvglTask) y, si una
 * app los pide, también LVGL.tick() de java: sirven para forzar un refresco
 * inmediato despues de montar la interfaz, no para llevar el reloj. */

/* Driver functions */
devcall lvglInit(device *);
devcall lvglControl(device *, int, long, long);

/* Hebra de la UI: reloj + lv_timer_handler() cada LVGL_TASK_MS.  La crea
 * lvglInit(); se expone para que el arranque la pueda lanzar desde otro sitio
 * y para que `ps' la muestre con nombre propio ("lvglui"). */
thread lvglTask(void);

/* Lock/unlock del monitor de LVGL (para natives que llamen a la API LVGL). */
void lvgl_lock(void);
void lvgl_unlock(void);

/* Limpieza del estado de una tarea java que se va (retorno de main o kill):
 * borra sus objetos, sus reservas de PSRAM y sus handles.  Lo llama el glue de
 * la VM (device/jvm/jvmglue.c) desde fuera del monitor. */
void lvgl_forget_task(int tid);

/* Registro del backend de ficheros de LVGL sobre la SD (letra 'S', ver
 * device/lvgl/lvglfs.c).  Lo llama lvglInit(); se expone para que las natives
 * sepan que una ruta "S:/..." es válida. */
void lvgl_fs_register(void);

/**
 * Volcar el espejo del fotograma a un BMP de 24 bits en la SD.
 *
 * El kernel no guarda en ningun sitio la imagen completa: LVGL trabaja por
 * franjas y el espejo lvgl_shadow es lo unico que tiene los 320x480 juntos.
 * Esto lo escribe tal cual, sin tocar el panel, para poder mirar la interfaz
 * desde un PC (curl /api/download?path=/shot.bmp) cuando la placa no esta a
 * la vista.  Devuelve OK o SYSERR.
 */
int lvglShotBmp(const char *ruta);

#endif                          /* _LVGL_H_ */
