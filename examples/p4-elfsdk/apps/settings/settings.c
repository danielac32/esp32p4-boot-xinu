/**
 * @file settings.c
 *
 * Ajustes del P4.  Por ahora dos entradas:
 *
 *   - WiFi: lista las redes que ve el C6, deja elegir una, abre un teclado para
 *     la clave y, al conectar, guarda la red en /wifi.txt.  El arranque
 *     (netBootTask) prueba despues ese fichero en orden, asi que al volver a
 *     encender se asocia sola.
 *   - Tiempo de pantalla: de momento solo informa; el apagado por inactividad
 *     no esta hecho.
 *
 *   elf /elf/settings.elf
 *
 * El escaneo, el join y el DHCP NO se reimplementan aqui: se llaman las tres
 * funciones que publica el kernel (wifinet.h), que son las mismas que usa
 * `netboot'.  La app solo pone la pantalla, el teclado y el fichero.
 *
 * La clave se escribe VISIBLE (sin asteriscos) a proposito: es un pad pequeno,
 * se teclea despacio y conviene ver lo que se ha puesto.
 *
 * Estado: todo en un bloque de PSRAM colgado del handle (la pantalla actual, la
 * lista del escaneo y los punteros a los objetos de LVGL).  Nada de estaticas
 * escribibles: el cargador de ELF no las soporta.  El teclado y el campo de
 * texto son widgets normales de LVGL; sus eventos corren en lvglui y solo
 * apuntan una accion en el estado (accion), que el bucle de main lee.
 */

#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <timer.h>
#include <memory.h>
#include <device.h>
#include <thread.h>
#include <network.h>
#include <fat_filelib.h>
#include <lvgl.h>
#include <appmgr.h>
#include <wifinet.h>
#include "gestos.h"

void exit(int codigo);

/* La lectura de senal vive en shell/xsh_wifi_scan.c y no tiene cabecera. */
extern int wifiSignalQuality(int32 rssi);

/* ---- constantes de la app ------------------------------------------------ */

#define FICHERO    "/wifi.txt"

#define P_MENU     0
#define P_WIFI     1
#define P_CLAVE    2

#define ID_WIFI     1
#define ID_TIEMPO   2
#define ID_APAGAR   3
#define ID_RED      10          /* 10 .. 10+WIFI_REDES_MAX-1                  */
#define ID_BUSCAR   20
#define ID_ATRAS_W  21
#define ID_CONECTAR 30
#define ID_ATRAS_C  31
#define ID_BORRAR   32
#define ID_SALIR    40

#define LISTA_MAX  16           /* redes que se muestran (con scroll)         */
#define FILAS      LISTA_MAX

#define C_TEXTO   0xE8D8B0      /* crema: titulos y avisos                     */
#define C_ESTADO  0x9FB3C8      /* gris azulado: linea de estado               */
#define C_ACCION  0x5DADE2      /* azul claro: botones de accion               */
#define C_BOTON   0xD7E3EE      /* gris claro: botones normales                */
#define C_FILA    0xC8D6E5      /* gris claro: filas de la lista               */

struct ajustes
{
    gestos_t *g;
    int pantalla;               /* P_*                                         */
    int nredes;                 /* -1 mientras se busca; 0..MAX despues        */
    int error;                  /* el ultimo escaneo no se pudo hacer          */
    struct wifi_red redes[LISTA_MAX];
    char ssid[WIFI_RED_SSID];   /* red elegida                                 */
    char texto[80];             /* buffer de snprintf                          */
    char fichero[2048];         /* /wifi.txt leido para mezclar redes          */
    char nuevo[2048];           /* /wifi.txt reescrito                         */
    volatile int accion;        /* 1 = el teclado pulso OK                     */
    lv_obj_t *titulo;
    lv_obj_t *estado;
    lv_obj_t *campo;            /* lv_textarea de la clave                     */
    lv_obj_t *tecla;            /* lv_keyboard                                 */
    lv_obj_t *lista;            /* contenedor con scroll de la lista WiFi      */
    int slot;
};

/* ---- rotulos ------------------------------------------------------------- */

static void poner_titulo(struct ajustes *m, const char *txt)
{
    lvgl_lock();
    lv_label_set_text(m->titulo, txt);
    lvgl_unlock();
}

static void poner_estado(struct ajustes *m, const char *txt)
{
    lvgl_lock();
    lv_label_set_text(m->estado, txt);
    lvgl_unlock();
}

/**
 * Crea los dos rotulos fijos (titulo y estado) como hijos del fondo.  Se crean
 * una sola vez y se cambia su texto al navegar; no cuentan para el cupo de
 * botones de la libreria de gestos.
 */
static void crear_marco(struct ajustes *m)
{
    lvgl_lock();

    m->titulo = lv_label_create(gestos_raiz(m->g));
    lv_label_set_text(m->titulo, "SETTINGS");
    lv_obj_set_style_text_color(m->titulo, lv_color_hex(C_TEXTO), 0);
    lv_obj_align(m->titulo, LV_ALIGN_TOP_LEFT, 10, 8);

    m->estado = lv_label_create(gestos_raiz(m->g));
    lv_label_set_text(m->estado, "");
    lv_obj_set_style_text_color(m->estado, lv_color_hex(C_ESTADO), 0);
    lv_obj_align(m->estado, LV_ALIGN_TOP_LEFT, 10, 40);

    lvgl_unlock();
}

/* ---- IP de ETH0 ---------------------------------------------------------- */

/**
 * "192.168.1.127" o "sin red".  Se resuelve en cada pintado porque el DHCP
 * puede haber cambiado la direccion (justo despues de conectar).
 */
static void ip_actual(char *dst, int cap)
{
    struct netif *nif = netLookup(getdev("ETH0"));

    (void)cap;
    if (NULL == nif)
    {
        strcpy(dst, "sin red");
        return;
    }
    netaddrsprintf(dst, &nif->ip);
    if (0 == strcmp(dst, "0.0.0.0") || 0 == strcmp(dst, "NULL")
        || 0 == strcmp(dst, "ERROR"))
    {
        strcpy(dst, "sin IP");
    }
}

/* ---- /wifi.txt ----------------------------------------------------------- */

/** Lee el fichero entero en m->fichero.  Devuelve los bytes leidos. */
static int leer_fichero(struct ajustes *m)
{
    void *f;
    int n;

    m->fichero[0] = '\0';
    f = fl_fopen(FICHERO, "r");
    if (NULL == f)
    {
        return 0;
    }
    n = fl_fread(m->fichero, 1, sizeof(m->fichero) - 1, f);
    fl_fclose(f);
    if (n < 0)
    {
        n = 0;
    }
    m->fichero[n] = '\0';
    return n;
}

/**
 * Guarda una red como la primera de /wifi.txt.  Primero se lee el fichero y se
 * monta el resultado en memoria; solo despues se abre en "w" (que trunca), de
 * modo que un fallo al leer no deja el fichero vacio.  Si la red ya estaba, se
 * sustituye (no se duplica); el resto se conserva en el mismo orden.  El tope
 * es WIFI_REDES_MAX redes.
 */
static void guardar_red(struct ajustes *m, const char *ssid, const char *clave)
{
    char *p;
    char linea[WIFI_RED_SSID + WIFI_RED_CLAVE + 4];
    void *f;
    int o;
    int redes;

    /* Se lee el fichero actual para conservar otras redes.  La red que se
     * acaba de usar va PRIMERA (es la que el arranque debe probar antes) y no
     * se duplica.  Se reconstruye cada linea como "ssid;clave" (no se copian
     * fragmentos crudos) y se escribe todo de una vez: si algo falla antes de
     * abrir en "w", el fichero viejo queda intacto. */
    leer_fichero(m);

    o = snprintf(m->nuevo, sizeof(m->nuevo), "%s;%s\n", ssid, clave);
    if (o < 0)
    {
        o = 0;
    }
    if (o >= (int)sizeof(m->nuevo))
    {
        o = (int)sizeof(m->nuevo) - 1;
    }
    redes = 1;

    p = m->fichero;
    while ('\0' != *p && redes < WIFI_REDES_MAX)
    {
        char *fin = strchr(p, '\n');
        char *sep;
        char *s;
        char *k;
        int l = (NULL != fin) ? (int)(fin - p) : (int)strlen(p);

        if (l >= (int)sizeof(linea))
        {
            l = (int)sizeof(linea) - 1;
        }
        memcpy(linea, p, l);
        linea[l] = '\0';
        if (l > 0 && '\r' == linea[l - 1])
        {
            linea[l - 1] = '\0';
        }

        if (linea[0] != '\0' && linea[0] != '#')
        {
            sep = strchr(linea, ';');
            if (NULL != sep && sep != linea)
            {
                *sep = '\0';
                s = linea;
                k = sep + 1;
                if (0 != strcmp(s, ssid))
                {
                    int n = snprintf(m->nuevo + o,
                                     sizeof(m->nuevo) - (size_t)o,
                                     "%s;%s\n", s, k);

                    if (n > 0)
                    {
                        o += n;
                        if (o >= (int)sizeof(m->nuevo))
                        {
                            o = (int)sizeof(m->nuevo) - 1;
                            break;
                        }
                        redes++;
                    }
                }
            }
        }

        if (NULL == fin)
        {
            break;
        }
        p = fin + 1;
    }
    m->nuevo[o] = '\0';

    f = fl_fopen(FICHERO, "w");
    if (NULL == f)
    {
        printf("settings: no se pudo escribir %s\r\n", FICHERO);
        return;
    }
    fl_fwrite(m->nuevo, 1, (int)strlen(m->nuevo), f);
    fl_fclose(f);
    printf("settings: guardada \"%s\" en %s\r\n", ssid, FICHERO);
}

#ifdef PRUEBA_CONECTA
/** Primera red valida de m->fichero, para la prueba automatica. */
static int primera_red_fichero(struct ajustes *m, char *ssid, char *clave)
{
    char *p;

    leer_fichero(m);
    p = m->fichero;
    while ('\0' != *p)
    {
        char *fin = strchr(p, '\n');
        char *sep;
        int l = (NULL != fin) ? (int)(fin - p) : (int)strlen(p);

        if (l > 0 && '\r' == p[l - 1])
        {
            l--;
        }
        sep = strchr(p, ';');
        if (l > 1 && '#' != p[0] && NULL != sep && sep != p
            && (sep - p) < l)
        {
            int sl = (int)(sep - p);
            int cl = l - sl - 1;

            if (sl >= WIFI_RED_SSID) sl = WIFI_RED_SSID - 1;
            if (cl >= WIFI_RED_CLAVE) cl = WIFI_RED_CLAVE - 1;
            memcpy(ssid, p, sl);
            ssid[sl] = '\0';
            memcpy(clave, sep + 1, cl);
            clave[cl] = '\0';
            return 1;
        }
        if (NULL == fin)
        {
            break;
        }
        p = fin + 1;
    }
    return 0;
}
#endif

/* ---- pantallas ----------------------------------------------------------- */

/** Borra el teclado y el campo de texto, si estaban. */
static void cerrar_clave(struct ajustes *m)
{
    lvgl_lock();
    if (NULL != m->tecla)
    {
        /* primero el teclado: guarda un puntero al campo */
        lv_obj_del(m->tecla);
        m->tecla = NULL;
    }
    if (NULL != m->campo)
    {
        lv_obj_del(m->campo);
        m->campo = NULL;
    }
    lvgl_unlock();
}

static void cerrar_lista(struct ajustes *m)
{
    /* El contenedor lo borra el llamador DESPUES de gestos_botones_limpiar():
     * asi las filas (hijas rastreadas) ya se quitaron del tracker y no quedan
     * punteros colgando.  Aqui solo se va el contenedor vacio. */
    if (NULL != m->lista)
    {
        lvgl_lock();
        lv_obj_del(m->lista);
        m->lista = NULL;
        lvgl_unlock();
    }
}

static void pantalla_menu(struct ajustes *m)
{
    char ip[32];

    m->pantalla = P_MENU;
    gestos_botones_limpiar(m->g);
    cerrar_clave(m);
    cerrar_lista(m);

    poner_titulo(m, "SETTINGS");
    ip_actual(ip, sizeof(ip));
    snprintf(m->texto, sizeof(m->texto), "WiFi: %s", ip);
    poner_estado(m, m->texto);

    gestos_boton(m->g, ID_WIFI, "WiFi", 20, 110, 280, 56, C_ACCION);
    gestos_boton(m->g, ID_TIEMPO, "Screen timeout", 20, 178, 280, 56, C_BOTON);
    /* El driver del PMIC es solo lectura a proposito (escribir 0x10/los rails
     * puede tumbar la placa): el boton avisa en vez de apagar de verdad. */
    gestos_boton(m->g, ID_APAGAR, "Power off", 20, 246, 280, 56, 0xC0392B);
    gestos_boton(m->g, ID_SALIR, "X", 276, 6, 36, 32, C_BOTON);
}

/** Nombre de la fila i: SSID recortado y, si hay RSSI, el porcentaje. */
static void texto_fila(struct ajustes *m, int i, char *dst, int cap)
{
    int l;

    snprintf(dst, cap, "%s", m->redes[i].ssid);
    if (strlen(dst) > 28)
    {
        dst[28] = '\0';
    }
    if (m->redes[i].hay_rssi)
    {
        l = (int)strlen(dst);
        snprintf(dst + l, cap - l, " %d%%",
                 wifiSignalQuality((int32)m->redes[i].rssi));
    }
}

static void pantalla_wifi(struct ajustes *m)
{
    char fila[80];
    char ip[32];
    int i;

    m->pantalla = P_WIFI;
    gestos_botones_limpiar(m->g);
    cerrar_clave(m);
    cerrar_lista(m);

    poner_titulo(m, "WiFi");
    if (m->error)
    {
        poner_estado(m, "Scan failed");
    }
    else if (m->nredes < 0)
    {
        poner_estado(m, "Scanning...");
    }
    else if (0 == m->nredes)
    {
        poner_estado(m, "No networks found");
    }
    else
    {
        ip_actual(ip, sizeof(ip));
        snprintf(m->texto, sizeof(m->texto), "%d network(s)  %s", m->nredes, ip);
        poner_estado(m, m->texto);
    }

    gestos_boton(m->g, ID_BUSCAR, "Scan", 10, 76, 180, 40, C_ACCION);
    gestos_boton(m->g, ID_ATRAS_W, "Back", 200, 76, 110, 40, C_BOTON);
    gestos_boton(m->g, ID_SALIR, "X", 276, 6, 36, 32, C_BOTON);

    /* Lista con scroll: viewport fijo y filas dentro.  Los botones se cuelgan
     * del contenedor (gestos_boton_en) para que el id siga llegando al anillo. */
    if (m->nredes > 0)
    {
        lvgl_lock();
        m->lista = lv_obj_create(gestos_raiz(m->g));
        lv_obj_set_size(m->lista, 304, 344);
        lv_obj_set_pos(m->lista, 8, 126);
        lv_obj_set_style_bg_opa(m->lista, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(m->lista, 0, 0);
        lv_obj_set_style_radius(m->lista, 0, 0);
        lv_obj_set_scroll_dir(m->lista, LV_DIR_VER);
        lvgl_unlock();

        for (i = 0; i < m->nredes && i < FILAS; i++)
        {
            texto_fila(m, i, fila, sizeof(fila));
            gestos_boton_en(m->g, ID_RED + i, fila, m->lista,
                            2, i * 42, 296, 38, C_FILA);
        }
    }
}

static void pantalla_clave(struct ajustes *m);

static void pantalla_clave(struct ajustes *m)
{
    char buf[64];

    m->pantalla = P_CLAVE;
    gestos_botones_limpiar(m->g);
    cerrar_clave(m);
    cerrar_lista(m);

    poner_titulo(m, "Password");
    snprintf(buf, sizeof(buf), "Network: %s", m->ssid);
    if (strlen(buf) > 36)
    {
        buf[36] = '\0';
    }
    poner_estado(m, buf);

    lvgl_lock();
    m->campo = lv_textarea_create(gestos_raiz(m->g));
    lv_obj_set_align(m->campo, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(m->campo, 10, 72);
    lv_obj_set_size(m->campo, 300, 44);
    lv_textarea_set_one_line(m->campo, 1);
    lv_textarea_set_max_length(m->campo, WIFI_RED_CLAVE - 1);
    lv_textarea_set_text(m->campo, "");
    /* Sin set_password_mode(): la clave se ve, que es lo que se pidio. */

    m->tecla = lv_keyboard_create(gestos_raiz(m->g));
    lv_obj_set_size(m->tecla, 320, 230);
    lv_obj_align(m->tecla, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_keyboard_set_mode(m->tecla, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(m->tecla, m->campo);
    lvgl_unlock();

    /* Dos filas encima del teclado: los tres botones no cabian en una. */
    gestos_boton(m->g, ID_CONECTAR, "Connect", 8, 122, 304, 42, C_ACCION);
    gestos_boton(m->g, ID_BORRAR, "Clear", 8, 168, 148, 38, C_BOTON);
    gestos_boton(m->g, ID_ATRAS_C, "Back", 164, 168, 148, 38, C_BOTON);
    gestos_boton(m->g, ID_SALIR, "X", 276, 6, 36, 32, C_BOTON);
}

/* ---- acciones ------------------------------------------------------------ */

/** Escanea y repinta.  Bloquea unos segundos; el LCD sigue pintando. */
static void escanear(struct ajustes *m)
{
    unsigned t0;
    int n;

    m->nredes = -1;
    m->error = 0;
    pantalla_wifi(m);

    if (OK != wifiAsegurar())
    {
        m->nredes = 0;
        m->error = 1;
        pantalla_wifi(m);
        return;
    }

    t0 = millis();
    n = wifiScanFill(m->redes, LISTA_MAX);
    if (n < 0)
    {
        m->nredes = 0;
        m->error = 1;
    }
    else
    {
        m->nredes = n;
    }
    printf("settings: escaneo -> %d red(es) en %u ms\r\n", m->nredes,
           millis() - t0);
    pantalla_wifi(m);
}

static void elegir_red(struct ajustes *m, int i)
{
    strncpy(m->ssid, m->redes[i].ssid, sizeof(m->ssid) - 1);
    m->ssid[sizeof(m->ssid) - 1] = '\0';
    printf("settings: elegida \"%s\"\r\n", m->ssid);
    pantalla_clave(m);
}

/**
 * Asocia, sube ETH0 y guarda la red.  Si el join falla se queda en la pantalla
 * de la clave para poder corregirla; el arranque seguira usando el fichero que
 * ya habia.
 */
static void conectar_red(struct ajustes *m, const char *clave)
{
    char ip[32];

    poner_estado(m, "Connecting...");
    printf("settings: conectando a \"%s\"...\r\n", m->ssid);

    if (OK != wifiRedConectar(m->ssid, clave))
    {
        printf("settings: no se pudo conectar a \"%s\"\r\n", m->ssid);
        poner_estado(m, "Connection failed");
        return;
    }

    /* Si asociar funciono, guardar y volver a la lista: ya no hace falta
     * quedarse en la pantalla de la clave. */
    guardar_red(m, m->ssid, clave);
    pantalla_wifi(m);
    ip_actual(ip, sizeof(ip));
    snprintf(m->texto, sizeof(m->texto), "Conectado: %s", ip);
    poner_estado(m, m->texto);
    printf("settings: conectado, IP %s\r\n", ip);
}

/** Lee la clave del campo y conecta. */
static void conectar_campo(struct ajustes *m)
{
    char clave[WIFI_RED_CLAVE];

    lvgl_lock();
    strncpy(clave, lv_textarea_get_text(m->campo), sizeof(clave) - 1);
    clave[sizeof(clave) - 1] = '\0';
    lvgl_unlock();

    if ('\0' == clave[0])
    {
        poner_estado(m, "Enter the password");
        return;
    }
    conectar_red(m, clave);
}

/* ---- eventos ------------------------------------------------------------- */

static void ir_fondo(struct ajustes *m, int *fuera);

static void procesar(struct ajustes *m, gev_t *ev, int *fuera)
{
    if (GE_ARRASTRE == ev->tipo)
    {
        if (GE_ARRIBA == ev->dir)
        {
            ir_fondo(m, fuera);
        }
        return;
    }
    if (GE_BOTON != ev->tipo)
    {
        return;
    }

    /* La X esta en todas las pantallas y siempre sale. */
    if (ID_SALIR == ev->id)
    {
        *fuera = 1;
        return;
    }

    if (P_MENU == m->pantalla)
    {
        if (ID_WIFI == ev->id)
        {
            escanear(m);
        }
        else if (ID_TIEMPO == ev->id)
        {
            poner_estado(m, "Screen timeout: not implemented");
        }
        else if (ID_APAGAR == ev->id)
        {
            /* El PMIC (AXP2101) es de solo lectura a proposito: escribir el
             * soft-PWROFF/los rails puede tumbar la placa.  No se apaga aqui. */
            poner_estado(m, "Power off: PMIC is read-only");
        }
    }
    else if (P_WIFI == m->pantalla)
    {
        if (ID_BUSCAR == ev->id)
        {
            escanear(m);
        }
        else if (ID_ATRAS_W == ev->id)
        {
            pantalla_menu(m);
        }
        else if (ev->id >= ID_RED && ev->id < ID_RED + m->nredes)
        {
            elegir_red(m, ev->id - ID_RED);
        }
    }
    else if (P_CLAVE == m->pantalla)
    {
        if (ID_CONECTAR == ev->id)
        {
            conectar_campo(m);
        }
        else if (ID_BORRAR == ev->id)
        {
            lvgl_lock();
            lv_textarea_set_text(m->campo, "");
            lvgl_unlock();
        }
        else if (ID_ATRAS_C == ev->id)
        {
            pantalla_wifi(m);
        }
    }
}

#ifdef PRUEBA_CONECTA
/** Coge la primera red de /wifi.txt, la busca en el escaneo y conecta. */
static void auto_conectar(struct ajustes *m)
{
    char ssid[WIFI_RED_SSID];
    char clave[WIFI_RED_CLAVE];
    int i;

    if (!primera_red_fichero(m, ssid, clave))
    {
        printf("settings: /wifi.txt vacio o ilegible\r\n");
        return;
    }
    for (i = 0; i < m->nredes; i++)
    {
        if (0 == strcmp(m->redes[i].ssid, ssid))
        {
            printf("settings: auto conecta a \"%s\"\r\n", ssid);
            strncpy(m->ssid, ssid, sizeof(m->ssid) - 1);
            pantalla_clave(m);
            lvgl_lock();
            lv_textarea_set_text(m->campo, clave);
            lvgl_unlock();
            conectar_red(m, clave);
            return;
        }
    }
    printf("settings: \"%s\" no esta en el escaneo\r\n", ssid);
}
#endif

/* ---- arranque ------------------------------------------------------------ */

static void salir(struct ajustes *m)
{
    if (m->slot >= 0)
    {
        appmgr_unregister(m->slot);
    }
    gestos_cerrar(m->g);        /* borra el fondo y todos sus hijos */
    psram_free(m);
}

static void ir_fondo(struct ajustes *m, int *fuera)
{
    gev_t ev;
    int s;

    if (m->slot < 0)
    {
        return;
    }
    gestos_show(m->g, 0);
    appmgr_set_state(m->slot, APP_BACKGROUND);
    while (!*fuera)
    {
        s = appmgr_get_state(m->slot);
        if (APP_RUNNING == s)
        {
            gestos_show(m->g, 1);
            return;
        }
        if (APP_CLOSING == s || APP_FREE == s)
        {
            *fuera = 1;
            return;
        }
        while (gestos_leer(m->g, &ev))
        {
            ;
        }
        sleep(50);
    }
}

int main(int argc, char *argv[])
{
    struct ajustes *m;
    gev_t ev;
    int fuera = 0;
#ifdef PRUEBA_AUTO
    unsigned t_auto = millis();
    int paso_auto = 0;
#endif

    (void)argc;
    (void)argv;

    printf("settings: arranca (tid=%d)\r\n", gettid());

    m = (struct ajustes *)psram_malloc(sizeof(struct ajustes));
    if (NULL == m)
    {
        printf("settings: no hay PSRAM para el estado\r\n");
        exit(1);
    }
    memset(m, 0, sizeof(struct ajustes));
    m->slot = SYSERR;

    /* Solo fondo, anillo de gestos y botones: la app pinta su cabecera. */
    m->g = gestos_nueva_partes(0);
    if (NULL == m->g)
    {
        psram_free(m);
        exit(1);
    }

    m->slot = appmgr_register("Settings", "/apps/settings/app.elf", 0x7A8C9C, gettid());

    crear_marco(m);
    pantalla_menu(m);

    while (!fuera)
    {
        while (gestos_leer(m->g, &ev))
        {
            procesar(m, &ev, &fuera);
            if (fuera)
            {
                break;
            }
        }

        /* El check del teclado (LV_EVENT_READY) solo deja una marca; la accion
         * se ejecuta aqui, en la hebra de la app, igual que un boton. */
        if (1 == m->accion && P_CLAVE == m->pantalla)
        {
            m->accion = 0;
            conectar_campo(m);
        }

#ifdef PRUEBA_AUTO
        /* Prueba sin dedo: abre WiFi, escanea y elige la primera red.  No va
         * en el binario normal; se compila con -DPRUEBA_AUTO. */
        if (0 == paso_auto && millis() - t_auto >= 1500u)
        {
            paso_auto = 1;
            printf("settings: auto abre WiFi\r\n");
            escanear(m);
        }
        else if (1 == paso_auto && millis() - t_auto >= 9000u)
        {
            paso_auto = 2;
            if (m->nredes > 0)
            {
                elegir_red(m, 0);
            }
        }
#ifdef PRUEBA_CONECTA
        else if (2 == paso_auto && millis() - t_auto >= 12000u)
        {
            paso_auto = 3;
            auto_conectar(m);
        }
#endif
#endif

        if (!fuera && m->slot >= 0 && APP_CLOSING == appmgr_get_state(m->slot))
        {
            fuera = 1;
        }
        if (!fuera)
        {
            sleep(20);
        }
    }

    salir(m);
    exit(0);
    return 0;
}
