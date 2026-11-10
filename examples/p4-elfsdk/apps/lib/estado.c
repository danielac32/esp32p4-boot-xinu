/**
 * @file estado.c
 * Status bar implementation (linked into each ELF). No writable file-scope
 * statics; const tables in .rodata. State in PSRAM handle.
 */
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <memory.h>
#include <device.h>
#include <power.h>
#include <network.h>
#include <espeth.h>
#include <timer.h>
#include <lvgl.h>
#include "estado.h"

extern int wifiSignalRead(int32 *rssi, uint32 *channel);
extern int wifiSignalQuality(int32 rssi);

#define ES_RALLITAS 4
#define ES_BAT_AN   34
#define ES_BAT_AL   18
#define ES_RELLENO  28
#define ES_PALABRA  16

static const int es_alto[ES_RALLITAS] = { 6, 10, 14, 18 };

struct estado
{
    lv_obj_t *caja;
    lv_obj_t *bara[ES_RALLITAS];
    lv_obj_t *ip;
    lv_obj_t *cuerpo;
    lv_obj_t *pico;
    lv_obj_t *relleno;
    lv_obj_t *pct;
    lv_obj_t *linea;
    int power;
    int bateria;
    int enlace;
    int nivel;
    char ip_txt[24];
    unsigned tic_dato;
    unsigned tic_rssi;
};

static void despojar(lv_obj_t *o)
{
    lv_obj_set_style_pad_top(o, 0, 0);
    lv_obj_set_style_pad_bottom(o, 0, 0);
    lv_obj_set_style_pad_left(o, 0, 0);
    lv_obj_set_style_pad_right(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_outline_width(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
}

static lv_obj_t *tabla(lv_obj_t *padre, int x, int y, int w, int h,
                       unsigned int rgb, int radio)
{
    lv_obj_t *o = lv_obj_create(padre);
    lv_obj_set_size(o, w, h);
    lv_obj_align(o, LV_ALIGN_TOP_LEFT, x, y);
    despojar(o);
    lv_obj_set_style_bg_color(o, lv_color_hex(rgb), 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_radius(o, radio, 0);
    return o;
}

static lv_obj_t *rotulo(lv_obj_t *padre, int x, int y, int w, const char *txt,
                        unsigned int rgb, lv_text_align_t ali)
{
    lv_obj_t *o = lv_label_create(padre);
    lv_obj_set_size(o, w, 18);
    lv_obj_align(o, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(o, lv_color_hex(rgb), 0);
    lv_obj_set_style_text_align(o, ali, 0);
    lv_label_set_text(o, txt);
    return o;
}

static int medir(estado_t *e, int senal)
{
    int cambio = 0;
    long v;
    int nivel;
    int q;
    int32 rssi = 0;
    uint32 canal = 0;
    struct netif *nif;
    char ip[24];

    v = -1;
    if (SYSERR != e->power)
    {
        v = control(e->power, POWER_CTRL_PERCENT, 0, 0);
        if (v < 0 || v > 100) v = -1;
    }
    if ((int)v != e->bateria) { e->bateria = (int)v; cambio = 1; }

    v = espethLinkIsUp() ? 1 : 0;
    if ((int)v != e->enlace)
    {
        e->enlace = (int)v;
        e->nivel = e->enlace ? ES_RALLITAS : 0;
        cambio = 1;
    }

    nif = netLookup(getdev("ETH0"));
    if (NULL == nif) strcpy(ip, "sin red");
    else
    {
        netaddrsprintf(ip, &nif->ip);
        if (0 == strcmp(ip, "0.0.0.0") || 0 == strcmp(ip, "NULL")
            || 0 == strcmp(ip, "ERROR"))
            strcpy(ip, e->enlace ? "dhcp..." : "sin red");
    }
    if (0 != strcmp(ip, e->ip_txt)) { strcpy(e->ip_txt, ip); cambio = 1; }

    if (senal)
    {
        nivel = 0;
        if (e->enlace)
        {
            if (OK == wifiSignalRead(&rssi, &canal))
            {
                q = wifiSignalQuality(rssi);
                nivel = (q >= 75) ? 4 : (q >= 50) ? 3 : (q >= 25) ? 2 : 1;
            }
            else nivel = ES_RALLITAS;
        }
        if (nivel != e->nivel) { e->nivel = nivel; cambio = 1; }
    }
    return cambio;
}

static void pintar(estado_t *e)
{
    char txt[12];
    unsigned int c;
    int i, ancho;

    lvgl_lock();
    for (i = 0; i < ES_RALLITAS; i++)
    {
        c = !e->enlace ? ES_ROJO : ((i < e->nivel) ? ES_VERDE : ES_APAGADO);
        lv_obj_set_style_bg_color(e->bara[i], lv_color_hex(c), 0);
    }
    lv_label_set_text(e->ip, e->ip_txt);
    if (e->bateria < 0)
    {
        lv_obj_add_flag(e->relleno, LV_OBJ_FLAG_HIDDEN);
        strcpy(txt, "--"); c = ES_PAPEL;
    }
    else
    {
        ancho = e->bateria * ES_RELLENO / 100;
        if (ancho < 1) ancho = 1;
        lv_obj_set_width(e->relleno, ancho);
        lv_obj_clear_flag(e->relleno, LV_OBJ_FLAG_HIDDEN);
        snprintf(txt, sizeof(txt), "%d%%", e->bateria);
        c = (e->bateria <= 20) ? ES_ROJO
            : (e->bateria <= 40) ? ES_AMBAR : ES_VERDE;
    }
    lv_obj_set_style_bg_color(e->relleno, lv_color_hex(c), 0);
    lv_label_set_text(e->pct, txt);
    lv_obj_set_style_text_color(e->pct, lv_color_hex(c), 0);
    lvgl_unlock();
}

statusbar_t *statusbar_create(lv_obj_t *padre, int x, int y, int w)
{
    estado_t *e;
    int i;

    if (NULL == padre || w < 200) return NULL;

    e = psram_malloc(sizeof(estado_t));
    if (NULL == e)
    {
        printf("statusbar: no hay PSRAM para el handle\r\n");
        return NULL;
    }
    memset(e, 0, sizeof(estado_t));
    e->bateria = -1;
    e->enlace = -1;
    e->nivel = -1;
    e->power = getdev("/dev/power0");

    lvgl_lock();

    e->caja = lv_obj_create(padre);
    lv_obj_set_size(e->caja, w, ES_ALTO);
    lv_obj_align(e->caja, LV_ALIGN_TOP_LEFT, x, y);
    despojar(e->caja);
    lv_obj_set_style_bg_color(e->caja, lv_color_hex(ES_CAJA), 0);
    lv_obj_set_style_bg_grad_dir(e->caja, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_radius(e->caja, 6, 0);
    lv_obj_set_style_border_width(e->caja, 1, 0);
    lv_obj_set_style_border_color(e->caja, lv_color_hex(ES_LATON), 0);

    for (i = 0; i < ES_RALLITAS; i++)
        e->bara[i] = tabla(e->caja, 10 + i * 7, 28 - es_alto[i],
                           4, es_alto[i], ES_APAGADO, 1);
    e->ip = rotulo(e->caja, 44, 11, w - 54, "", ES_PAPEL, LV_TEXT_ALIGN_LEFT);
    lv_label_set_long_mode(e->ip, LV_LABEL_LONG_DOT);

    e->cuerpo = tabla(e->caja, 10, 36, ES_BAT_AN, ES_BAT_AL, ES_CAJA, 3);
    lv_obj_set_style_bg_opa(e->cuerpo, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(e->cuerpo, 2, 0);
    lv_obj_set_style_border_color(e->cuerpo, lv_color_hex(ES_PAPEL), 0);
    e->pico = tabla(e->caja, 10 + ES_BAT_AN + 1, 41, 3, 8, ES_PAPEL, 1);
    e->relleno = tabla(e->caja, 13, 39, 1, 12, ES_VERDE, 1);
    e->pct = rotulo(e->caja, 56, 37, 4 * ES_PALABRA + 6, "--", ES_PAPEL,
                    LV_TEXT_ALIGN_LEFT);
    e->linea = rotulo(e->caja, w - 150, 37, 140, "", ES_PAPEL,
                      LV_TEXT_ALIGN_RIGHT);

    lvgl_unlock();

    medir(e, 0);
    pintar(e);
    e->tic_dato = millis();
    e->tic_rssi = e->tic_dato - ES_RSSI_MS;
    return e;
}

void statusbar_refresh(statusbar_t *e)
{
    unsigned ahora;
    int senal;

    if (NULL == e) return;
    ahora = millis();
    senal = ((unsigned)(ahora - e->tic_rssi) >= ES_RSSI_MS);
    if (!senal && (unsigned)(ahora - e->tic_dato) < ES_DATO_MS) return;
    if (senal) e->tic_rssi = ahora;
    e->tic_dato = ahora;
    if (medir(e, senal)) pintar(e);
}

void statusbar_destroy(statusbar_t *e)
{
    if (NULL == e) return;
    lvgl_lock();
    lv_obj_del(e->caja);
    lvgl_unlock();
    psram_free(e);
}

/* Compat symbols */
estado_t *estado_nueva(lv_obj_t *p, int x, int y, int w) { return statusbar_create(p,x,y,w); }
void estado_refrescar(estado_t *e) { statusbar_refresh(e); }
void estado_cerrar(estado_t *e) { statusbar_destroy(e); }
