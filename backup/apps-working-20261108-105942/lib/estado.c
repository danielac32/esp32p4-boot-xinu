/**
 * @file estado.c
 *
 * Implementacion de estado.h.  Como gestos.c, va enlazada dentro de la app
 * ('ld -r' junto a su main): solo llama a lo que publica system/exports.c y no
 * tiene ni una estatica escribible de fichero, porque el cargador no aplica la
 * relocacion R_RISCV_LO12_S con la que el compilador accede a una estatica por
 * ancla local.  Las tablas de abajo son const (.rodata) y se alcanzan con
 * lui/addi, que si estan soportadas.
 */
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <memory.h>
#include <device.h>
#include <power.h>                /* POWER_CTRL_PERCENT                        */
#include <network.h>              /* netLookup / netaddrsprintf / struct netif */
#include <espeth.h>               /* espethLinkIsUp                            */
#include <timer.h>                /* millis                                    */
#include <lvgl.h>                 /* el device Xinu: lvgl_lock / lvgl_unlock   */
#include "estado.h"

/* La lectura de senal vive en shell/xsh_wifi_scan.c y no tiene cabecera propia:
 * los mismos prototipos que declaran xsh_wifi.c y p4demoTask.c. */
extern int wifiSignalRead(int32 *rssi, uint32 *channel);
extern int wifiSignalQuality(int32 rssi);

#define ES_RALLITAS 4
#define ES_BAT_AN   34            /* el cuerpo de la bateria, pico aparte      */
#define ES_BAT_AL   18
#define ES_RELLENO  28            /* ES_BAT_AN - 2 de borde - 2 de aire        */
#define ES_PALABRA  16            /* ancho de un caracter de unscii_16         */

/* Las cuatro rallitas, de la mas baja a la mas alta.  Todas apoyan en y=28. */
static const int es_alto[ES_RALLITAS] = { 6, 10, 14, 18 };

struct estado
{
    lv_obj_t *caja;               /* la banda entera, hija de `padre`          */
    lv_obj_t *bara[ES_RALLITAS];  /* el icono del WiFi                         */
    lv_obj_t *ip;                 /* la direccion, o "sin red" / "dhcp..."     */
    lv_obj_t *cuerpo;             /* el contorno de la bateria                 */
    lv_obj_t *pico;               /* el terminal de la derecha                 */
    lv_obj_t *relleno;            /* el nivel: su ancho es el porcentaje       */
    lv_obj_t *pct;                /* "100%"                                    */
    lv_obj_t *linea;              /* "EN LINEA" / "SIN RED", a la derecha      */
    int power;                    /* descriptor de /dev/power0, SYSERR si no   */
    int bateria;                  /* 0..100, o -1 si no hay PMIC               */
    int enlace;                   /* 0 / 1                                     */
    int nivel;                    /* rallitas encendidas, 0..ES_RALLITAS       */
    char ip_txt[24];
    unsigned tic_dato;            /* millis de la ultima lectura barata        */
    unsigned tic_rssi;            /* millis del ultimo RPC al C6               */
};

/* ---- los ladrillos de LVGL; todos con el candado ya cogido ----------------- */

/* Objeto limpio de los adornos del tema.  La tabla de exports solo tiene los
 * setters sueltos del pad (no hay lv_obj_set_style_pad_all), asi que van los
 * cuatro.  Y se quita CLICKABLE: lv_obj.c pone el flag en el constructor de
 * CUALQUIER objeto, y el buscador del indev devuelve el primero que pisa el
 * punto; con el flag puesto, una rallita o el contorno de la bateria son zonas
 * muertas donde el dedo no llega al fondo y el arrastre no se mide. */
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

/* ---- leer y pintar -------------------------------------------------------- */

/**
 * Relee las fuentes.  Con `senal` pide ademas el RSSI al C6, que es la unica
 * llamada que puede bloquear (hasta 1000 ms).  Devuelve 1 si algo cambio, que
 * es la condicion para volver a pintar: LVGL invalida el area en cada setter,
 * y repintar la banda entera cada segundo sin haber cambiado nada es trabajo
 * tirado en un panel que va por SPI.
 */
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
        if (v < 0 || v > 100)
        {
            v = -1;
        }
    }
    if ((int)v != e->bateria)
    {
        e->bateria = (int)v;
        cambio = 1;
    }

    v = espethLinkIsUp() ? 1 : 0;
    if ((int)v != e->enlace)
    {
        e->enlace = (int)v;
        /* Al asociarse se encienden las cuatro hasta que el RSSI diga otra
         * cosa: verde inmediato, que es lo que se quiere ver al conectar. */
        e->nivel = e->enlace ? ES_RALLITAS : 0;
        cambio = 1;
    }

    nif = netLookup(getdev("ETH0"));
    if (NULL == nif)
    {
        strcpy(ip, "sin red");
    }
    else
    {
        netaddrsprintf(ip, &nif->ip);
        if (0 == strcmp(ip, "0.0.0.0") || 0 == strcmp(ip, "NULL")
            || 0 == strcmp(ip, "ERROR"))
        {
            strcpy(ip, e->enlace ? "dhcp..." : "sin red");
        }
    }
    if (0 != strcmp(ip, e->ip_txt))
    {
        strcpy(e->ip_txt, ip);
        cambio = 1;
    }

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
            else
            {
                /* Enlace arriba pero el C6 no contesto: no se apaga el icono
                 * por un RPC perdido, se quedan las cuatro encendidas. */
                nivel = ES_RALLITAS;
            }
        }
        if (nivel != e->nivel)
        {
            e->nivel = nivel;
            cambio = 1;
        }
    }

    return cambio;
}

static void pintar(estado_t *e)
{
    char txt[12];
    unsigned int c;
    int i;
    int ancho;

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
        strcpy(txt, "--");
        c = ES_PAPEL;
    }
    else
    {
        ancho = e->bateria * ES_RELLENO / 100;
        if (ancho < 1)
        {
            ancho = 1;            /* con 0% se ve una raya, no un hueco */
        }
        lv_obj_set_width(e->relleno, ancho);
        lv_obj_clear_flag(e->relleno, LV_OBJ_FLAG_HIDDEN);
        snprintf(txt, sizeof(txt), "%d%%", e->bateria);
        c = (e->bateria <= 20) ? ES_ROJO
            : (e->bateria <= 40) ? ES_AMBAR : ES_VERDE;
    }
    lv_obj_set_style_bg_color(e->relleno, lv_color_hex(c), 0);
    lv_label_set_text(e->pct, txt);
    lv_obj_set_style_text_color(e->pct, lv_color_hex(c), 0);

    //lv_label_set_text(e->linea, e->enlace ? "EN LINEA" : "SIN RED");
    //lv_obj_set_style_text_color(e->linea,
      //                          lv_color_hex(e->enlace ? ES_VERDE : ES_ROJO), 0);

    lvgl_unlock();
}

/* ---- la API --------------------------------------------------------------- */

estado_t *estado_nueva(lv_obj_t *padre, int x, int y, int w)
{
    estado_t *e;
    int i;

    if (NULL == padre || w < 200)
    {
        return NULL;
    }

    e = psram_malloc(sizeof(estado_t));
    if (NULL == e)
    {
        printf("estado: no hay PSRAM para el handle\r\n");
        return NULL;
    }
    memset(e, 0, sizeof(estado_t));
    e->bateria = -1;
    e->enlace = -1;               /* -1 fuerza el primer pintado */
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

    /* Linea 1: las rallitas del WiFi y la IP. */
    for (i = 0; i < ES_RALLITAS; i++)
    {
        e->bara[i] = tabla(e->caja, 10 + i * 7, 28 - es_alto[i],
                           4, es_alto[i], ES_APAGADO, 1);
    }
    e->ip = rotulo(e->caja, 44, 11, w - 54, "", ES_PAPEL, LV_TEXT_ALIGN_LEFT);
    lv_label_set_long_mode(e->ip, LV_LABEL_LONG_DOT);

    /* Linea 2: el logo de la bateria (cuerpo con borde, pico y relleno), el
     * porcentaje y la palabra del enlace.  El relleno es hijo de la caja y no
     * del cuerpo: con el borde de 2 px del cuerpo, un hijo heredaria su origen
     * interior y habria que descontar el borde dos veces. */
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

    /* Primero lo barato y se pinta, para que la banda no salga en blanco; el
     * RSSI se pide en la primera vuelta del bucle de la app, con la caja ya
     * visible (el que refresca LVGL es la hebra residente, no esta). */
    medir(e, 0);
    pintar(e);
    e->tic_dato = millis();
    e->tic_rssi = e->tic_dato - ES_RSSI_MS;
    return e;
}

void estado_refrescar(estado_t *e)
{
    unsigned ahora;
    int senal;

    if (NULL == e)
    {
        return;
    }

    ahora = millis();
    senal = ((unsigned)(ahora - e->tic_rssi) >= ES_RSSI_MS);
    if (!senal && (unsigned)(ahora - e->tic_dato) < ES_DATO_MS)
    {
        return;
    }
    if (senal)
    {
        e->tic_rssi = ahora;
    }
    e->tic_dato = ahora;

    if (medir(e, senal))
    {
        pintar(e);
    }
}

void estado_cerrar(estado_t *e)
{
    if (NULL == e)
    {
        return;
    }
    lvgl_lock();
    lv_obj_del(e->caja);
    lvgl_unlock();
    psram_free(e);
}