/**
 * @file lvglfs.c
 *
 * Backend de filesystem de LVGL sobre la FAT de la tarjeta SD.
 *
 * LVGL no llama a la libc: sus decodificadores de imagen (sjpg, png) y sus
 * fuentes en fichero work con rutas "L:/camino" donde L es la letra de un
 * lv_fs_drv_t registrado.  Esta unidad monta la letra 'S' (de SD) y traduce
 * esas llamadas a fl_fopen/fl_fread/... de fatfs/, que ya tienen su propio
 * monitor reentrante instalado por el device `fat' en el arranque.
 *
 * Con esto una app java puede hacer LVGL.img(scr, "/img/foto.jpg", ...) y LVGL
 * decodifica el JPEG por su cuenta; native_lvgl.c solo pone el prefijo 'S:'.
 * El path que llega aqui ya viene sin letra (lv_fs.c se salta la letra y los
 * dos puntos), o sea que es una ruta absoluta de la tarjeta tal cual la
 * entiende fl_fopen: "/img/foto.jpg".
 *
 * Lo que NO se implementa: escritura y lectura de directorios.  LVGL solo pide
 * leer imagenes; listar el contenido de /img lo hace la capa java por sus
 * natives FAT, no la libreria.  Dejar los cb a NULL hace que lv_fs devuelva
 * LV_FS_RES_NOT_IMP, que es un fallo limpio, no un salto.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <kernel.h>
#include <device.h>
#include <stdbool.h>
#include <stdint.h>
#include <fat.h>
#include <fat_filelib.h>

#include "lvgl/lvgl.h"

#ifndef SEEK_SET
#define SEEK_SET 0
#endif
#ifndef SEEK_CUR
#define SEEK_CUR 1
#endif
#ifndef SEEK_END
#define SEEK_END 2
#endif

/* La letra con la que las rutas java se refieren a la tarjeta. */
#define LVGL_FS_LETTER 'S'

static lv_fs_drv_t fs_drv;

static bool fs_ready_cb(lv_fs_drv_t *drv)
{
    (void)drv;

    /* Se pregunta al device fat, no a un cache propio: el volcado de la SD
     * puede haber fallado tarde y la letra tiene que dejar de servir solos
     * en ese momento.  (Es lo mismo que hace fs_mounted() en la shell, pero
     * un device no depende de shell/.) */
    return control(FAT0, FAT_CTRL_MOUNTED, 0, 0) == 1 ? true : false;
}

static void *fs_open_cb(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode)
{
    (void)drv;

    /* LVGL pasa LV_FS_MODE_WR|RD; aqui solo hay lectura (no se escribe desde
     * la libreria), asi que "r" siempre. */
    (void)mode;

    return fl_fopen(path, "r");
}

static lv_fs_res_t fs_close_cb(lv_fs_drv_t *drv, void *file_p)
{
    (void)drv;

    if (file_p == 0)
    {
        return LV_FS_RES_INV_PARAM;
    }
    fl_fclose(file_p);
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_read_cb(lv_fs_drv_t *drv, void *file_p, void *buf,
                              uint32_t btr, uint32_t *br)
{
    int n;

    (void)drv;

    if (file_p == 0 || br == 0)
    {
        return LV_FS_RES_INV_PARAM;
    }

    /* fl_fread cuenta elementos de `size' bytes: con size=1 devuelve bytes. */
    n = fl_fread(buf, 1, (int)btr, file_p);
    *br = (n > 0) ? (uint32_t)n : 0;

    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_seek_cb(lv_fs_drv_t *drv, void *file_p, uint32_t pos,
                              lv_fs_whence_t whence)
{
    int origin;

    (void)drv;

    if (file_p == 0)
    {
        return LV_FS_RES_INV_PARAM;
    }

    switch (whence)
    {
    case LV_FS_SEEK_SET:
        origin = SEEK_SET;
        break;
    case LV_FS_SEEK_CUR:
        origin = SEEK_CUR;
        break;
    case LV_FS_SEEK_END:
        origin = SEEK_END;
        break;
    default:
        return LV_FS_RES_INV_PARAM;
    }

    return (fl_fseek(file_p, (long)pos, origin) == 0) ? LV_FS_RES_OK
                                                      : LV_FS_RES_FS_ERR;
}

static lv_fs_res_t fs_tell_cb(lv_fs_drv_t *drv, void *file_p, uint32_t *pos_p)
{
    long p;

    (void)drv;

    if (file_p == 0 || pos_p == 0)
    {
        return LV_FS_RES_INV_PARAM;
    }

    p = fl_ftell(file_p);
    if (p < 0)
    {
        return LV_FS_RES_FS_ERR;
    }
    *pos_p = (uint32_t)p;

    return LV_FS_RES_OK;
}

/**
 * @ingroup lvgl
 *
 * Registra el driver.  Se llama una sola vez desde lvglInit(), antes de
 * cualquier lv_img_set_src() con ruta.
 *
 * cache_size se queda a 0 (el que pone lv_fs_drv_init), igual que los backend
 * posix/FatFs que trae LVGL: sin cache intermedia de la libreria, cada
 * lv_fs_read es un f_read de la FAT, y la FAT ya tiene su ventana de 512 B.
 * Ponerla a mano solo tiene sentido si medir que el JPEG va lento.
 *
 * Las rutas que llegan son absolutas ("/img/foto.jpg") porque LVGL no conoce
 * el directorio de trabajo de la shell; una ruta relativa no la resuelve ni
 * aqui ni con fs_resolve().
 */
void lvgl_fs_register(void)
{
    lv_fs_drv_init(&fs_drv);

    fs_drv.letter = LVGL_FS_LETTER;
    fs_drv.ready_cb = fs_ready_cb;
    fs_drv.open_cb = fs_open_cb;
    fs_drv.close_cb = fs_close_cb;
    fs_drv.read_cb = fs_read_cb;
    fs_drv.seek_cb = fs_seek_cb;
    fs_drv.tell_cb = fs_tell_cb;

    lv_fs_drv_register(&fs_drv);
}
