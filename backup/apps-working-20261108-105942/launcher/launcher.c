/**
 * @file launcher.c
 *
 * El launcher del P4 sobre las dos librerias de apps/lib: gestos.[ch] (la
 * pantalla, el arrastre y los botones con id) y estado.[ch] (la banda de IP,
 * WiFi y bateria).  Sirve de plantilla: rejilla de seis teclas por dos paginas,
 * banda de estado arriba y una tecla de salida.  Las teclas todavia no lanzan
 * nada -- imprimen la eleccion por consola -- porque elf_run() anidado en la
 * misma hebra pisa el registro ELF de thrtab y el exit() del hijo mataria ESTE
 * hilo en vez del suyo (ver system/elfload.c y system/kill.c).
 *
 *   elf /elf/launcher.elf
 *
 * Las tres piezas de cualquier app de este arbol, y en este orden:
 *
 *   1. ARRANCAR LA PANTALLA.  gestos_nueva_partes(0) comprueba que el device
 *      lvgl este montado, reserva el estado en PSRAM y monta SOLO lo que esta
 *      app usa: el fondo que mide el dedo y el anillo de eventos.  No pide
 *      pastilla, flecha ni nota, asi que no se crean objetos que taparian la
 *      banda de estado ni se pinta mobiliario ajeno antes de tiempo.  Si algun
 *      dia se quiere el aviso visual, basta pasar GE_PASTILLA | GE_FLECHA.
 *      La medicion del arrastre no depende de los adornos: va en el fondo.
 *
 *   2. PONER LOS CONTROLES.  gestos_boton(g, id, texto, x, y, w, h, color): el
 *      id es lo que devuelve ev.id al pulsar, asi que la app no escribe ni un
 *      callback de LVGL.  Las seis teclas se crean UNA vez y al pasar pagina
 *      solo se reetiquetan (gestos_boton_texto), que es mas barato que borrar y
 *      crear y no consume el cupo de GE_BOTONES.
 *
 *   3. EL VIGIA.  Una hebra hija ('launcher-vig') refresca la banda de estado y
 *      arma el apagado por inactividad.  Va aparte por una razon concreta:
 *      estado_refrescar() llama a wifiSignalRead(), que es un RPC al C6 y puede
 *      bloquear hasta un segundo.  En el bucle de gestos ese segundo se lo
 *      tragaba el dedo; aqui se lo traga el vigia, que no pinta hasta volver.
 *
 *   4. EL BUCLE.  La UI la bombea la hebra residente 'lvglui' del kernel; aqui
 *      NO se llama a lv_timer_handler().  Cada vuelta saca un evento del anillo
 *      y, si no hay, duerme 50 ms -- sin ese sleep el bucle gira en vacio y se
 *      come la CPU que la hebra de la UI necesita para pintar.  Lo unico que se
 *      decide es un switch sobre ev.tipo: direccion del arrastre, o id del
 *      boton.  Al salir se para el vigia y se le espera antes de borrar nada.
 *
 * Tiempo de pantalla: el vigia arma LVGL_CTRL_IDLE con 2 minutos al entrar y lo
 * deja a 0 al salir.  La CUENTA no la lleva el app sino el device lvgl del
 * kernel (device/lvgl/lvglport.c): solo el ve todos los dedos del panel, y es
 * quien apaga el backlight, se salta el flush por SPI y despierta con el
 * siguiente toque (ese primer dedo se lo traga el, no llega al app).  Aqui solo
 * se pone la politica, y se pone UNA vez: cada LVGL_CTRL_IDLE reinicia la
 * cuenta, asi que repetirlo en un bucle dejaria la pantalla encendida siempre.
 *
 * OJO con `kill <tid>' desde la shell: mata la hebra del app pero no la del
 * vigia, que se queda ejecutando codigo de un bloque de PSRAM ya liberado.  La
 * salida buena es la tecla SALIR, que para el vigia y le espera.
 *
 * Reparto de la pantalla (320x480).  OJO: unscii_16 se genero desde unscii-8 a
 * 16 px, asi que cada caracter ocupa 16 px de ancho, no 8:
 *
 *    y=8    banda de estado (304x62): rallitas + IP / bateria + EN LINEA
 *    y=78   chip: "pagina 1/2", o la direccion del ultimo arrastre
 *    y=106  rejilla 2x3, teclas de 145x76, hasta y=354
 *    y=364  los dos puntos de pagina
 *    y=384  la pista de uso
 *    y=410  SALIR (148x56)
 */
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <memory.h>               /* psram_malloc / psram_free                 */
#include <thread.h>               /* create / ready / receive / getprio        */
#include <device.h>               /* control / getdev: LVGL0 y su politica     */
#include <timer.h>
#include <lvgl.h>                 /* el device Xinu: lvgl_lock / lvgl_unlock   */
#include "gestos.h"
#include "estado.h"

void exit(int codigo);

/* Rejilla: dos columnas por tres filas. */
#define COLUMNAS   2
#define FILAS      3
#define POR_PAGINA (COLUMNAS * FILAS)
#define PAGINAS    3

#define ANCHO 145
#define ALTO  76
#define X0    11
#define Y0    106
#define HGX   8
#define HGY   10

#define ID_SALIR 9

/* La hebra vigia.  8 KB de pila es lo que usa 'lvglui', que hace el render
 * entero; el vigia solo lee datos y escribe cuatro rotulos, asi que le sobra.
 * VIGIA_MS es el periodo del sondeo, NO el del refresco: estado_refrescar() se
 * gatea sola (ES_DATO_MS 1 s para enlace/bateria/IP, ES_RSSI_MS 10 s para el
 * RPC del RSSI) y no pinta nada si el dato no cambio. */
#define VIGIA_STK  8192
#define VIGIA_MS   200
#define APAGADO_MS 120000         /* 2 minutos sin dedo -> backlight fuera     */

/* La paleta vintage de apps/musica: baquelita, laton y papel.  Las teclas son
 * versiones apagadas de los colores del esqueleto anterior, todas lo bastante
 * claras para que el rotulo negro que pone gestos_boton se lea encima. */
#define C_FONDO 0x1B1408
#define C_CAJA  0x2A1E10
#define C_PAPEL 0xE8D8B0
#define C_LATON 0xB08D57
#define C_AMBAR 0xFFB302
#define C_ROJO  0xC0392B
#define C_APAGO 0x4A3B28
#define C_BORDE 0x241A0E

/* Los nombres de las paginas.  Es .rodata con R_RISCV_32 sobre las cadenas:
 * el cargador la aplica; lo que no se puede son tablas escribibles. */
static const char *nombres[PAGINAS][POR_PAGINA] =
{
    { "GALERIA", "MUSICA", "AJUSTES", "VOZ", "RELOJ", "CONSOLA" },
    { "RED", "BATERIA", "ARCHIVOS", "JUEGOS", "CAMARA", "INFO" },
    { "BOOT", "APAGAR" ,"CONTRASEÑAS","OTRO", "OTRO" ,"OTRO",}
};

static const unsigned int colores[POR_PAGINA] =
{
    0xA8B5A0, 0xE0B25E, 0xB08D57, 0xC98A7A, 0x9FB4C7, 0xBFA8C9
};

/** El estado de la app, en PSRAM: el ABI no admite estaticas escribibles. */
struct launcher
{
    gestos_t *g;
    estado_t *e;
    lv_obj_t *chip;               /* la linea de pagina / direccion            */
    lv_obj_t *punto[PAGINAS];     /* los puntos de abajo                       */
    lv_obj_t *tecla[POR_PAGINA];
    int pag;
    tid_typ vigia;                /* hebra de la banda de estado y la pantalla */
    volatile int cerrar;          /* lo pone el bucle, lo lee el vigia         */
};

/* ---- las piezas de pantalla ----------------------------------------------- */

/** Cambia el chip de arriba.  El candado lo toma la app porque estos setters
 *  son de LVGL y no pasan por ninguna funcion de gestos.h. */
static void chip(struct launcher *l, const char *txt, unsigned int rgb)
{
    lvgl_lock();
    lv_label_set_text(l->chip, txt);
    lv_obj_set_style_text_color(l->chip, lv_color_hex(rgb), 0);
    lvgl_unlock();
}

static void chip_pagina(struct launcher *l)
{
    char txt[24];

    snprintf(txt, sizeof(txt), "pagina %d/%d", l->pag + 1, PAGINAS);
    chip(l, txt, C_PAPEL);
}

/* El nombre largo de gestos_dir_texto() ("de izquierda A LA DERECHA") son 25
 * caracteres, 400 px: no cabe en los 304 de la banda.  Aqui va la palabra seca,
 * con el color que la libreria le da a cada direccion. */
static const char *dir_corto(int dir)
{
    switch (dir)
    {
    case GE_ARRIBA: return "ARRIBA";
    case GE_ABAJO:  return "ABAJO";
    case GE_IZQ:    return "IZQUIERDA";
    case GE_DER:    return "DERECHA";
    default:        return "sin arrastre";
    }
}

/** Reetiqueta las seis teclas y enciende el punto de la pagina nueva. */
static void pagina(struct launcher *l, int p)
{
    int i;

    l->pag = p;
    for (i = 0; i < POR_PAGINA; i++)
    {
        gestos_boton_texto(l->g, i + 1, nombres[p][i]);
    }

    lvgl_lock();
    for (i = 0; i < PAGINAS; i++)
    {
        lv_obj_set_style_bg_color(l->punto[i],
                                  lv_color_hex(i == p ? C_AMBAR : C_APAGO), 0);
    }
    lvgl_unlock();
}

/**
 * El adorno de las teclas: un borde oscuro de 2 px y la esquina menos redonda
 * que la de fabrica, para que parezcan teclas de un panel viejo y no pegatinas.
 * gestos_boton() devuelve el lv_obj justo para esto.  El rotulo de SALIR va en
 * papel porque el fondo es rojo y el negro que pone la libreria no se leeria.
 */
static void vestir_teclas(struct launcher *l, lv_obj_t *salir)
{
    int i;
    lv_obj_t *b;

    lvgl_lock();
    for (i = 0; i < POR_PAGINA; i++)
    {
        b = l->tecla[i];
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(C_BORDE), 0);
        lv_obj_set_style_radius(b, 10, 0);
    }
    lv_obj_set_style_border_width(salir, 2, 0);
    lv_obj_set_style_border_color(salir, lv_color_hex(C_BORDE), 0);
    lv_obj_set_style_radius(salir, 10, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(salir, 0),
                                lv_color_hex(C_PAPEL), 0);
    lvgl_unlock();
}

/* ---- el vigia ------------------------------------------------------------- */

/**
 * WiFi, bateria e IP, y la politica de pantalla.  Todo lo que no es un dedo.
 *
 * estado_refrescar() se gatea sola por millis y solo pinta si el dato cambio,
 * asi que llamarla cada VIGIA_MS no cuesta nada; lo caro es wifiSignalRead(),
 * un RPC al C6 con techo de 1 s, y por eso vive aqui y no en el bucle de gestos.
 *
 * El reloj de la inactividad NO esta en este bucle: lo lleva el device lvgl del
 * kernel, que es el unico que ve todos los dedos del panel.  Lo que hace esta
 * hebra es armarlo al entrar y desarmarlo al salir, de modo que la cuenta
 * empieza cuando el launcher toma la pantalla y la politica no se hereda al app
 * siguiente.  Se arma UNA vez: cada LVGL_CTRL_IDLE reinicia lastTouchMs, y
 * repetirlo aqui dejaria el backlight encendido para siempre.
 */
static int vigia(struct launcher *l)
{
    control(getdev("LVGL0"), LVGL_CTRL_IDLE, APAGADO_MS, 0);

    while (!l->cerrar)
    {
        estado_refrescar(l->e);
        sleep(VIGIA_MS);
    }

    /* 0 = no apagar nunca, que es el default del arranque; ademas despierta la
     * pantalla si estaba apagada, asi que el que venga detras no arranca a
     * oscuras. */
    control(getdev("LVGL0"), LVGL_CTRL_IDLE, 0, 0);
    return 0;
}

/* ---- el bucle ------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    struct launcher *l;
    gev_t ev;
     
    int i;
    int fuera = 0;

    l = psram_malloc(sizeof(struct launcher));
    if (NULL == l)
    {
        printf("launcher: no hay PSRAM\r\n");
        exit(1);
    }
    memset(l, 0, sizeof(struct launcher));
    l->vigia = SYSERR;            /* 0 es un tid valido: el de la hebra nula   */

    /* 1. la pantalla */
    l->g = gestos_nueva_partes(0);
    if (NULL == l->g)
    {
        psram_free(l);
        exit(1);
    }

    lvgl_lock();
    lv_obj_set_style_bg_color(gestos_raiz(l->g), lv_color_hex(C_FONDO), 0);
    lvgl_unlock();

    /* 2. los controles */
    l->e = estado_nueva(gestos_raiz(l->g), 8, 8, 304);

    for (i = 0; i < POR_PAGINA; i++)
    {
        int fila = i / COLUMNAS;
        int col = i % COLUMNAS;

        l->tecla[i] = gestos_boton(l->g, i + 1, nombres[0][i],
                                   X0 + col * (ANCHO + HGX),
                                   Y0 + fila * (ALTO + HGY),
                                   ANCHO, ALTO, colores[i]);
    }
    //salir = gestos_boton(l->g, ID_SALIR, "SALIR", 86, 410, 148, 56, C_ROJO);
    //vestir_teclas(l, salir);

    lvgl_lock();
    l->chip = lv_label_create(gestos_raiz(l->g));
    lv_obj_set_size(l->chip, 304, 18);
    lv_obj_align(l->chip, LV_ALIGN_TOP_LEFT, 8, 78);
    lv_obj_set_style_text_align(l->chip, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(l->chip, LV_OBJ_FLAG_CLICKABLE);
    /* Nace vacio.  Una lv_label sin texto se queda con el "Text" de fabrica de
     * LVGL, y eso era el rotulo fantasma que salia entre la banda y la rejilla.
     * El chip se rellena al pulsar una tecla (chip()) o si algun dia se quiere
     * la pagina (chip_pagina()). */
    lv_label_set_text(l->chip, "");

    for (i = 0; i < PAGINAS; i++)
    {
        l->punto[i] = lv_obj_create(gestos_raiz(l->g));
        lv_obj_set_size(l->punto[i], 10, 10);
        lv_obj_align(l->punto[i], LV_ALIGN_TOP_LEFT, 145 + i * 20, 364);
        lv_obj_set_style_pad_top(l->punto[i], 0, 0);
        lv_obj_set_style_pad_bottom(l->punto[i], 0, 0);
        lv_obj_set_style_pad_left(l->punto[i], 0, 0);
        lv_obj_set_style_pad_right(l->punto[i], 0, 0);
        lv_obj_set_style_border_width(l->punto[i], 0, 0);
        lv_obj_set_style_outline_width(l->punto[i], 0, 0);
        lv_obj_set_style_shadow_width(l->punto[i], 0, 0);
        lv_obj_set_style_radius(l->punto[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_grad_dir(l->punto[i], LV_GRAD_DIR_NONE, 0);
        lv_obj_clear_flag(l->punto[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(l->punto[i], LV_OBJ_FLAG_CLICKABLE);
    }

   // o = lv_label_create(gestos_raiz(l->g));
    /*lv_label_set_text(o, "desliza para pasar");
    lv_obj_set_size(o, 304, 18);
    lv_obj_align(o, LV_ALIGN_TOP_LEFT, 8, 384);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(C_LATON), 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);*/
    lvgl_unlock();

    pagina(l, 0);
     
    /* 3. el vigia.  Misma prioridad que esta hebra: los dos se duermen en su
     * bucle y ninguno le come la CPU al otro.  El estado que ve es el bloque de
     * PSRAM de aqui, no una copia: volatile en 'cerrar' es lo unico que hace
     * falta porque el resto lo escribe solo esta hebra. */
    l->vigia = create(vigia, VIGIA_STK, getprio(gettid()), "launcher-vig", 1, l);
    if (SYSERR == (int)l->vigia)
    {
        printf("launcher: no se pudo crear el vigia\r\n");
        fuera = 1;                /* cae directo a la limpieza de abajo      */
    }
    else
    {
        ready(l->vigia, RESCHED_NO);
    }

    /* 4. el bucle: un evento del anillo y una decision sobre ev.tipo.  La banda
     * de estado ya no se refresca aqui, la lleva el vigia. */
    while (!fuera)
    {
        if (!gestos_leer(l->g, &ev))
        {
            //sleep(50);
            continue;
        }

        switch (ev.tipo)
        {
        case GE_ARRASTRE:
            /* La rejilla cabe entera: en vertical no hay nada que desplazar,
             * asi que los cuatro lados pasan pagina. */
            if (GE_IZQ == ev.dir || GE_ARRIBA == ev.dir)
            {
                pagina(l, (l->pag + 1) % PAGINAS);
            }
            else
            {
                pagina(l, (l->pag + PAGINAS - 1) % PAGINAS);
            }
            /*chip(l, dir_corto(ev.dir), gestos_dir_color(ev.dir));
            printf("launcher: arrastre %s -> pagina %d/%d\r\n",
                   gestos_dir_texto(ev.dir), l->pag + 1, PAGINAS);*/
            break;

        case GE_BOTON:
            #if 0
                    if (ID_SALIR == ev.id)
                    {
                        fuera = 1;
                        break;
                    }
                    /* Aqui es donde ira elf_run() cuando la carga tenga hebra propia.
                     * De momento la eleccion se imprime y el chip la confirma. */
                    chip(l, nombres[l->pag][ev.id - 1], C_AMBAR);
                    printf("launcher: tecla %d = %s\r\n", ev.id,
                           nombres[l->pag][ev.id - 1]);
            #endif
            break;

        case GE_TOQUE:
        case GE_LARGA:
            //chip_pagina(l);
            break;

        default:
            break;
        }
    }

    //printf("launcher: salida\r\n");

    /* Primero el vigia, y por las buenas.  Al devolver, userret()->kill() manda
     * su tid a esta hebra (su padre) y suelta su pila: ese receive() es el join.
     * Hacerle kill() desde aqui seria peor, porque kill() no suelta monitores y
     * si muriera con lvgl_lock tomado la UI entera se quedaria colgada.  Puede
     * tardar del orden de un segundo: es el techo del RPC del RSSI.  Hasta que
     * no llega el tid no se borra nada, asi que el vigia nunca lee estado
     * liberado ni pinta sobre objetos que ya no existen. */
    if (SYSERR != (int)l->vigia)
    {
        l->cerrar = 1;
        while (receive() != (int)l->vigia)
        {
            ;
        }
    }

    estado_cerrar(l->e);          /* antes de gestos_cerrar: la caja es hija */
    gestos_cerrar(l->g);
    psram_free(l);
    exit(0);
}