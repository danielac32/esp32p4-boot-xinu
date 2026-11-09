/**
 * @file appmgr.h
 *
 * Registro compartido de aplicaciones (launcher <-> apps ELF).  Vive en el
 * kernel como una tabla fija y se exporta a las apps: como no hay MMU, todos
 * ven el mismo espacio de direcciones y basta un puntero/funciones exportadas
 * para compartir estado.
 *
 * Modelo cooperativo: solo la app en primer plano (APP_RUNNING) pinta y atiende
 * gestos; las de fondo (APP_BACKGROUND) ocultan su UI y pueden seguir vivas
 * (musica sigue sonando por el kernel).  El cierre es cooperativo: el launcher
 * marca APP_CLOSING y la app, al verlo, limpia y sale (nunca kill() directo,
 * que no borra LVGL ni para el audio).
 */
#ifndef _APPMGR_H_
#define _APPMGR_H_

#include <kernel.h>

#define APPMGR_MAX     8

#define APP_FREE       0
#define APP_RUNNING    1
#define APP_BACKGROUND 2
#define APP_CLOSING    3

struct app_entry
{
    char    name[16];   /* nombre corto (icono/tarjeta) */
    char    path[32];   /* ruta del ELF, p. ej. /elf/musica.elf */
    uint32  color;      /* color de la tarjeta */
    int     state;      /* APP_* */
    tid_typ tid;        /* tarea que corre la app */
    int     info;       /* estado propio (p. ej. reproduciendo) */
};

/* puntero a la tabla y cantidad de ranuras */
struct app_entry *appmgr_table(void);
int appmgr_count(void);

/* registro/ciclo de vida */
int appmgr_register(const char *name, const char *path, uint32 color, tid_typ tid);
int appmgr_unregister(int slot);
int appmgr_find_path(const char *path);
int appmgr_find_tid(tid_typ tid);

/* estado */
int  appmgr_get_state(int slot);
void appmgr_set_state(int slot, int state);
void appmgr_request_close(int slot);
int  appmgr_take_foreground(int slot);  /* marca slot RUNNING, las demas RUNNING->BACKGROUND */
void appmgr_set_info(int slot, int info);
int  appmgr_get_info(int slot);

#endif /* _APPMGR_H_ */
