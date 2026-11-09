/**
 * @file gestos.c
 *
 * Implementacion de gestos.h.  Va enlaza dentro de cada app ('ld -r' junto a
 * su main), asi que cumple las dos reglas del formato ELF de este arbol: solo
 * llama a lo que publica system/exports.c y no tiene ni una estatica
 * escribible de fichero --todo el estado sale de un psram_malloc y viaja como
 * 'user_data' de los callbacks-- porque el cargador no aplica la relocacion
 * R_RISCV_LO12_S con la que el compilador accede a una estatica por ancla local.
 */
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <memory.h>
#include <lvgl.h>               /* el device Xinu: LVGL0 y LVGL_CTRL_READY     */
#include "gestos.h"

/* Puntos minimos de recorrido para que un arrastre cuente como gesto. */
#define GE_LIMO 40

/* Anillo de eventos y tope de botones.  El tope es alto porque una lista con
 * scroll vive al reves que un launcher de paginas: las filas se crean TODAS de
 * una vez dentro del contenido que se desplaza (lo que se mueve es el
 * contenedor, no las filas), asi que 24 canciones son 24 botones. */
#define GE_ANILLO  8
#define GE_BOTONES 32

/* La flecha se dibuja dentro de un cuadro de LADO x LADO; los puntos son
 * relativos a su origen, asi que todo tiene que caer en positivos. */
#define LADO        160
#define CENTRO      (LADO / 2)
#define LARGO_PUNTA 58
#define LARGO_COLA  44
#define RETROCESO   24
#define APERTURA    22
#define GRUESO      14

/* Los cuatro colores de direccion.  El verde de ARRIBA iba a 0x21D07A: con los
 * 16 bits del panel (RGB565) su azul tiraba del verde hacia el verde azulado y
 * quedaba apagado al lado del rojo, el azul y el ambar.  Con 0x2FFF5E el canal
 * verde va al maximo y el rotulo negro se lee igual. */
#define COLOR_ARRIBA  0x2FFF5E
#define COLOR_ABAJO   0xE8453C
#define COLOR_IZQ     0x38A5FF
#define COLOR_DER     0xFFB302
#define COLOR_REPOSO  0x7A8C9C
#define COLOR_TEXTO   0xEAF2F8

/** Un boton de los que crea la app: su id, su rotulo y a quien pertenece. */
struct boton
{
    gestos_t *g;
    int id;
    lv_obj_t *btn;
    lv_obj_t *lbl;
};

struct gestos
{
    lv_obj_t *fondo;            /* a pantalla completa, el que caza el dedo    */
    lv_obj_t *panel;            /* pastilla del rotulo, del color de la dir    */
    lv_obj_t *aviso;            /* rotulo grande: la direccion en texto        */
    lv_obj_t *nota;             /* linea libre para la app                     */
    lv_obj_t *asta;             /* lv_line con el eje de la flecha             */
    lv_obj_t *cabeza;           /* lv_line con las dos barbas y la punta       */
    lv_point_t pts[2];          /* {punta, cola}                               */
    lv_point_t cab[3];          /* {barba1, punta, barba2}                     */
    struct boton botones[GE_BOTONES];
    int nbotones;
    /* El anillo lo produce lvglui (dentro del handler, con el candado cogido)
     * y lo consume la hebra del app sin candado: uno solo escribe delante y el
     * otro detras, y por eso los indices y el payload van en volatile. */
    volatile gev_t anillo[GE_ANILLO];
    volatile int cola, tope;
    volatile int prensado;      /* 1 con el dedo apoyado: x1/y1 validos        */
    volatile int x1, y1;
    volatile int arrastrado;    /* el ultimo levantamiento cruzo GE_LIMO       */
    int dir_pintada;            /* ultima direccion procesada, para no repetir   */
    int flecha_oculta;
};

static int entero(int v)
{
    return v < 0 ? -v : v;
}

/* La direccion segun el vector: manda el eje dominante y, como y=0 es el
 * borde superior, dy<0 es que el dedo sube. */
static int direccion(int dx, int dy)
{
    if (entero(dx) >= entero(dy))
    {
        return dx > 0 ? GE_DER : GE_IZQ;
    }
    return dy > 0 ? GE_ABAJO : GE_ARRIBA;
}

const char *gestos_dir_texto(int dir)
{
    switch (dir)
    {
    case GE_ARRIBA: return "de abajo HACIA ARRIBA";
    case GE_ABAJO:  return "de arriba HACIA ABAJO";
    case GE_IZQ:    return "de derecha A LA IZQUIERDA";
    case GE_DER:    return "de izquierda A LA DERECHA";
    default:        return "sin arrastre";
    }
}

unsigned int gestos_dir_color(int dir)
{
    switch (dir)
    {
    case GE_ARRIBA: return COLOR_ARRIBA;
    case GE_ABAJO:  return COLOR_ABAJO;
    case GE_IZQ:    return COLOR_IZQ;
    case GE_DER:    return COLOR_DER;
    default:        return COLOR_REPOSO;
    }
}

/** Apila un evento; se pierde si el anillo esta lleno (nadie estaba mirando). */
static void apilar(gestos_t *g, int tipo, int dir, int x1, int y1, int x2,
                   int y2, int id)
{
    int n = (g->tope + 1) & (GE_ANILLO - 1);
    volatile gev_t *e;

    if (n == g->cola)
    {
        return;
    }
    e = &g->anillo[g->tope];
    e->tipo = tipo;
    e->dir = dir;
    e->id = id;
    e->x1 = x1;
    e->y1 = y1;
    e->x2 = x2;
    e->y2 = y2;
    e->dx = x2 - x1;
    e->dy = y2 - y1;
    g->tope = n;
}

/* ---- las tres piezas visuales; todas con el candado ya cogido -------------- */

/* Flecha mirando a `d`: u es el sentido del dedo y p su perpendicular.  La
 * punta va mas lejos que la cola, asi que se ve hacia donde va aunque el
 * recorrido sea corto. */
static void girar(gestos_t *g, int d)
{
    int ux = 0, uy = 0, px = 0, py = 0;
    unsigned int c = gestos_dir_color(d);
    int tx, ty;

    switch (d)
    {
    case GE_ARRIBA: uy = -1; px = 1;  break;
    case GE_ABAJO:  uy = 1;  px = 1;  break;
    case GE_IZQ:    ux = -1; py = 1;  break;
    case GE_DER:    ux = 1;  py = 1;  break;
    default:        uy = -1; px = 1;  break;
    }

    tx = CENTRO + ux * LARGO_PUNTA;
    ty = CENTRO + uy * LARGO_PUNTA;

    g->pts[0].x = tx;
    g->pts[0].y = ty;
    g->pts[1].x = CENTRO - ux * LARGO_COLA;
    g->pts[1].y = CENTRO - uy * LARGO_COLA;

    g->cab[0].x = tx - ux * RETROCESO + px * APERTURA;
    g->cab[0].y = ty - uy * RETROCESO + py * APERTURA;
    g->cab[1].x = tx;
    g->cab[1].y = ty;
    g->cab[2].x = tx - ux * RETROCESO - px * APERTURA;
    g->cab[2].y = ty - uy * RETROCESO - py * APERTURA;

    /* Cada adorno es opcional: si la app no pidio GE_FLECHA en
     * gestos_nueva_partes() estos punteros son NULL y aqui no se toca nada. */
    if (NULL != g->asta)
    {
        lv_line_set_points(g->asta, g->pts, 2);
        lv_obj_set_style_line_color(g->asta, lv_color_hex(c), 0);
    }
    if (NULL != g->cabeza)
    {
        lv_line_set_points(g->cabeza, g->cab, 3);
        lv_obj_set_style_line_color(g->cabeza, lv_color_hex(c), 0);
    }
}

static void rotulo(gestos_t *g, const char *txt, unsigned int fondo)
{
    if (NULL != g->aviso)
    {
        lv_label_set_text(g->aviso, txt);
    }
    if (NULL != g->panel)
    {
        lv_obj_set_style_bg_color(g->panel, lv_color_hex(fondo), 0);
    }
}

/* Direccion en la pantalla: flecha + rotulo + color.  dir_pintada se anota aqui
 * y no en girar(): con la flecha oculta girar() no corre, y si el dato quedara
 * viejo el guard de PRESSING (d != dir_pintada) llamaria a mostrar() en CADA
 * muestra del dedo en vez de una por cambio de eje. */
static void mostrar(gestos_t *g, int d)
{
    if (!g->flecha_oculta)
    {
        girar(g, d);
    }
    /* El rotulo es hijo de la pastilla, y lv_obj_area_is_visible() solo mira el
     * flag HIDDEN del propio objeto, no el de sus padres: escribirle el texto a
     * un aviso dentro de una pastilla oculta invalida igual sus 300x46 px y
     * manda dos franjas de SPI por cada arrastre.  Con la pastilla fuera no se
     * le escribe nada; gestos_panel(g,1) la repinta al volver. */
    if (NULL != g->panel && !lv_obj_has_flag(g->panel, LV_OBJ_FLAG_HIDDEN))
    {
        rotulo(g, gestos_dir_texto(d), gestos_dir_color(d));
    }
    g->dir_pintada = d;
}

/* Objeto limpio de los adornos del tema: relleno, borde, contorno y sombra.
 * La tabla de exports solo tiene los setters sueltos del pad (no existe
 * lv_obj_set_style_pad_all ni lv_obj_remove_style_all), asi que van los
 * cuatro.  Sin esto la pastilla redonda saca un rectangulo detras del circulo
 * y el desplazamiento interno mueve los hijos unos pocos pixeles. */
static void limpiar(lv_obj_t *o)
{
    lv_obj_set_style_pad_top(o, 0, 0);
    lv_obj_set_style_pad_bottom(o, 0, 0);
    lv_obj_set_style_pad_left(o, 0, 0);
    lv_obj_set_style_pad_right(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_outline_width(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

/* ---- los callbacks: corren en lvglui, apilan y pintan, NUNCA imprimen ------ */

static void ev_fondo(lv_event_t *e)
{
    lv_event_code_t cod = lv_event_get_code(e);
    gestos_t *g = lv_event_get_user_data(e);
    lv_indev_t *in = lv_event_get_indev(e);
    lv_point_t p;
    int dx, dy, d;

    /* El fondo se registro con LV_EVENT_ALL, o sea que tambien recibe los
     * codigos que no vienen del indev (DELETE al borrar la pantalla, SIZE,
     * VALUE_CHANGED...) y para esos lv_event_get_indev() devuelve NULL:
     * lv_indev_get_point() despeina un puntero nulo y el cargador no tiene a
     * quien avisar, asi que la pantalla se cae por trap.  Primero se mira el
     * codigo y, si no hay indev, se sale. */
    if (NULL == in)
    {
        return;
    }

    lv_indev_get_point(in, &p);

    if (cod == LV_EVENT_PRESSED)
    {
        g->x1 = p.x;
        g->y1 = p.y;
        g->prensado = 1;
    }
    else if (cod == LV_EVENT_PRESSING)
    {
        /* Traduccion en vivo: PRESSING va en cada lectura del indev, asi que
         * solo se repinta cuando el eje dominante cambia. */
        if (g->prensado != 0)
        {
            dx = p.x - g->x1;
            dy = p.y - g->y1;
            if (entero(dx) >= GE_LIMO || entero(dy) >= GE_LIMO)
            {
                d = direccion(dx, dy);
                if (d != g->dir_pintada)
                {
                    mostrar(g, d);
                }
            }
        }
    }
    else if (cod == LV_EVENT_RELEASED)
    {
        if (g->prensado != 0)
        {
            g->prensado = 0;
            dx = p.x - g->x1;
            dy = p.y - g->y1;
            g->arrastrado = (entero(dx) >= GE_LIMO || entero(dy) >= GE_LIMO);

            if (g->arrastrado)
            {
                d = direccion(dx, dy);
                apilar(g, GE_ARRASTRE, d, g->x1, g->y1, p.x, p.y, 0);
                mostrar(g, d);
            }
        }
    }
    else if (cod == LV_EVENT_CLICKED)
    {
        apilar(g, GE_TOQUE, GE_SINDIR, p.x, p.y, p.x, p.y, 0);
        rotulo(g, "TOQUE, sin arrastre", COLOR_REPOSO);
    }
    else if (cod == LV_EVENT_LONG_PRESSED)
    {
        apilar(g, GE_LARGA, GE_SINDIR, p.x, p.y, p.x, p.y, 0);
        rotulo(g, "PULSACION LARGA", COLOR_REPOSO);
    }
}

static void ev_boton(lv_event_t *e)
{
    struct boton *sl = lv_event_get_user_data(e);
    gestos_t *g = sl->g;
    lv_point_t p;

    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }
    /* RELEASED corre antes y, con el seguimiento del dedo tambien montado en
     * el boton, dejo la marca puesta: si el dedo recorrio GE_LIMO esto fue un
     * arrastre sobre el boton, no una pulsacion, y no se cuenta. */
    if (g->arrastrado)
    {
        return;
    }
    lv_indev_get_point(lv_event_get_indev(e), &p);
    apilar(g, GE_BOTON, GE_SINDIR, p.x, p.y, p.x, p.y, sl->id);
}

/* ---- la API --------------------------------------------------------------- */

gestos_t *gestos_nueva(void)
{
    return gestos_nueva_partes(GE_ADORNOS);
}

gestos_t *gestos_nueva_partes(int partes)
{
    gestos_t *g;

    if (SYSERR == control(LVGL0, LVGL_CTRL_READY, 0, 0))
    {
        printf("gestos: LVGL no esta inicializado\r\n");
        return NULL;
    }

    g = psram_malloc(sizeof(gestos_t));
    if (NULL == g)
    {
        printf("gestos: no hay PSRAM para el handle\r\n");
        return NULL;
    }
    memset(g, 0, sizeof(gestos_t));
    g->dir_pintada = GE_SINDIR;
    /* Sin flecha montada no hay nada que girar: mostrar() se ahorra el viaje a
     * girar() en cada PRESSING. */
    g->flecha_oculta = (0 == (partes & GE_FLECHA));

    lvgl_lock();

    /* El fondo caza el dedo.  CLICKABLE es obligatorio: sin el, el indev no lo
     * toma como objeto activo y no se entera de nada.  Esto es el nucleo de la
     * libreria y se monta siempre, pida la app adornos o no. */
    g->fondo = lv_obj_create(lv_scr_act());
    lv_obj_set_size(g->fondo, LV_PCT(100), LV_PCT(100));
    lv_obj_align(g->fondo, LV_ALIGN_CENTER, 0, 0);
    limpiar(g->fondo);
    lv_obj_set_style_bg_color(g->fondo, lv_color_hex(0x07131f), 0);
    lv_obj_set_style_bg_grad_dir(g->fondo, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(g->fondo, LV_OPA_COVER, 0);
    lv_obj_add_flag(g->fondo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g->fondo, ev_fondo, LV_EVENT_ALL, g);

    /* A partir de aqui, mobiliario opcional.  Todo lo que NO sea un boton se
     * queda sin LV_OBJ_FLAG_CLICKABLE: lv_obj.c pone el flag en el constructor
     * de CUALQUIER objeto, y el buscador del indev (lv_indev_search_obj) se
     * queda con el primero que pisa el punto.  Con el flag puesto, la pastilla,
     * la flecha y la nota serian zonas muertas donde el dedo no llega al fondo
     * ni al boton, o sea que el gesto no se mediria. */

    if (partes & GE_PASTILLA)
    {
        /* Pastilla: 300x46 arriba, redonda, del color de la direccion, con el
         * texto en negro.  El color de texto se pone a mano porque el tema de
         * LVGL saca gris sobre negro y con la fuente unscii de 16 no se lee. */
        g->panel = lv_obj_create(g->fondo);
        lv_obj_set_size(g->panel, 300, 46);
        lv_obj_align(g->panel, LV_ALIGN_TOP_MID, 0, 14);
        limpiar(g->panel);
        lv_obj_clear_flag(g->panel, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_grad_dir(g->panel, LV_GRAD_DIR_NONE, 0);
        lv_obj_set_style_radius(g->panel, LV_RADIUS_CIRCLE, 0);

        /* El rotulo nace VACIO, no con el "Text" por defecto de LVGL.  Lo pinta
         * rotulo() al primer arrastre; hasta entonces la pastilla es solo su
         * color de reposo. */
        g->aviso = lv_label_create(g->panel);
        lv_label_set_text(g->aviso, "");
        lv_obj_set_style_text_color(g->aviso, lv_color_hex(0x101820), 0);
        lv_obj_align(g->aviso, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(g->aviso, LV_OBJ_FLAG_CLICKABLE);
    }

    if (partes & GE_FLECHA)
    {
        /* Dos lineas gordas dentro de un cuadro de 160x160.  El tema da fondo
         * opaco a todo lv_obj y lv_line hereda de lv_obj: sin LV_OPA_TRANSP el
         * cuadro saldria como una losa gris encima. */
        g->asta = lv_line_create(g->fondo);
        lv_obj_set_size(g->asta, LADO, LADO);
        lv_obj_align(g->asta, LV_ALIGN_TOP_MID, 0, 74);
        lv_obj_set_style_bg_opa(g->asta, LV_OPA_TRANSP, 0);
        lv_obj_set_style_line_width(g->asta, GRUESO, 0);
        lv_obj_set_style_line_rounded(g->asta, true, 0);
        lv_obj_clear_flag(g->asta, LV_OBJ_FLAG_CLICKABLE);

        g->cabeza = lv_line_create(g->fondo);
        lv_obj_set_size(g->cabeza, LADO, LADO);
        lv_obj_align(g->cabeza, LV_ALIGN_TOP_MID, 0, 74);
        lv_obj_set_style_bg_opa(g->cabeza, LV_OPA_TRANSP, 0);
        lv_obj_set_style_line_width(g->cabeza, GRUESO, 0);
        lv_obj_set_style_line_rounded(g->cabeza, true, 0);
        lv_obj_clear_flag(g->cabeza, LV_OBJ_FLAG_CLICKABLE);

        girar(g, GE_ARRIBA);
        lv_obj_set_style_line_color(g->asta, lv_color_hex(COLOR_REPOSO), 0);
        lv_obj_set_style_line_color(g->cabeza, lv_color_hex(COLOR_REPOSO), 0);
    }

    if (partes & GE_NOTA)
    {
        /* Linea de texto libre, centrada en y=250.  Nace vacia y OCULTA: una
         * etiqueta de LVGL sin texto se queda con "Text" y en (0,0), que es el
         * fantasma que salia en la esquina al no configurarla. */
        g->nota = lv_label_create(g->fondo);
        lv_label_set_text(g->nota, "");
        lv_obj_set_style_text_color(g->nota, lv_color_hex(COLOR_TEXTO), 0);
        lv_obj_align(g->nota, LV_ALIGN_TOP_MID, 0, 250);
        lv_obj_clear_flag(g->nota, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(g->nota, LV_OBJ_FLAG_HIDDEN);
    }

    lvgl_unlock();
    return g;
}

int gestos_leer(gestos_t *g, gev_t *ev)
{
    int c = g->cola;
    volatile gev_t *e;

    if (c == g->tope)
    {
        return 0;
    }
    e = &g->anillo[c];
    ev->tipo = e->tipo;
    ev->dir = e->dir;
    ev->id = e->id;
    ev->x1 = e->x1;
    ev->y1 = e->y1;
    ev->x2 = e->x2;
    ev->y2 = e->y2;
    ev->dx = e->dx;
    ev->dy = e->dy;
    g->cola = (c + 1) & (GE_ANILLO - 1);
    return 1;
}

lv_obj_t *gestos_boton_en(gestos_t *g, int id, const char *txt, lv_obj_t *padre,
                          int x, int y, int w, int h, unsigned int rgb)
{
    struct boton *sl;
    lv_obj_t *b, *t;

    if (NULL == g || NULL == padre || g->nbotones >= GE_BOTONES)
    {
        return NULL;
    }
    sl = &g->botones[g->nbotones];
    sl->g = g;
    sl->id = id;

    lvgl_lock();
    b = lv_btn_create(padre);
    lv_obj_set_size(b, w, h);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(rgb), 0);
    /* Sin degradado: el tema pinta los botones con un degradado que, si solo se
     * cambia el color base, deja el final en gris. */
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_add_event_cb(b, ev_boton, LV_EVENT_CLICKED, sl);
    /* El arrastre tiene que medirse tambien aqui: LVGL entrega PRESSED,
     * PRESSING y RELEASED al objeto que recibe el toque, y ese es el boton, no
     * el fondo.  Sin estos tres callbacks el dedo que cae sobre un boton no
     * deja rastro y la pantalla se queda con zonas muertas para el gesto
     * (LV_OBJ_FLAG_GESTURE_BUBBLE no sirve: solo sube LV_EVENT_GESTURE, y aqui
     * lo que se mide es el vector entre PRESSED y RELEASED). */
    lv_obj_add_event_cb(b, ev_fondo, LV_EVENT_PRESSED, g);
    lv_obj_add_event_cb(b, ev_fondo, LV_EVENT_PRESSING, g);
    lv_obj_add_event_cb(b, ev_fondo, LV_EVENT_RELEASED, g);
    t = lv_label_create(b);
    lv_label_set_text(t, txt);
    lv_obj_set_style_text_color(t, lv_color_hex(0x101820), 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 0);
    sl->lbl = t;
    sl->btn = b;
    lvgl_unlock();

    g->nbotones++;
    return b;
}

lv_obj_t *gestos_boton(gestos_t *g, int id, const char *txt, int x, int y,
                       int w, int h, unsigned int rgb)
{
    if (NULL == g)
    {
        return NULL;
    }
    return gestos_boton_en(g, id, txt, g->fondo, x, y, w, h, rgb);
}

lv_obj_t *gestos_raiz(gestos_t *g)
{
    return g->fondo;
}

void gestos_show(gestos_t *g, int on)
{
    if (NULL == g || NULL == g->fondo)
    {
        return;
    }
    lvgl_lock();
    if (on)
    {
        lv_obj_clear_flag(g->fondo, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(g->fondo, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_unlock();
}

/* Cambia el rotulo del boton con ese id.  Es lo que permite un launcher con
 * paginas: los seis botones se crean una vez y se reetiquetan al pasar de
 * pagina, en vez de borrarlos y crearlos (y quedarse sin cupo). */
void gestos_boton_texto(gestos_t *g, int id, const char *txt)
{
    int i;

    lvgl_lock();
    for (i = 0; i < g->nbotones; i++)
    {
        if (g->botones[i].id == id)
        {
            lv_label_set_text(g->botones[i].lbl, txt);
            break;
        }
    }
    lvgl_unlock();
}

/* Borra los botones y devuelve el cupo.  Una app con mas de GE_BOTONES
 * controles en total (una lista de canciones y luego la pantalla de reproduccion)
 * no puede dejarlos todos creados: se borran al cambiar de pantalla.  El fondo,
 * la flecha, la pastilla y la nota no se tocan; sigue viva la medicion del dedo.
 * Los `struct boton` quedan apuntados a NULL: son el user_data de los callbacks,
 * pero sus objetos ya no estan en el arbol y LVGL no los vuelve a llamar. */
void gestos_botones_limpiar(gestos_t *g)
{
    int i;

    if (NULL == g)
    {
        return;
    }
    lvgl_lock();
    for (i = 0; i < g->nbotones; i++)
    {
        if (NULL != g->botones[i].btn)
        {
            lv_obj_del(g->botones[i].btn);
            g->botones[i].btn = NULL;
            g->botones[i].lbl = NULL;
        }
    }
    g->nbotones = 0;
    lvgl_unlock();
}

void gestos_rotulo(gestos_t *g, const char *txt)
{
    if (NULL == g || NULL == g->aviso)
    {
        return;
    }
    lvgl_lock();
    lv_label_set_text(g->aviso, txt);
    lvgl_unlock();
}

void gestos_nota(gestos_t *g, const char *txt)
{
    if (NULL == g || NULL == g->nota)
    {
        return;
    }
    lvgl_lock();
    lv_label_set_text(g->nota, txt);
    if ('\0' == txt[0])
    {
        lv_obj_add_flag(g->nota, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_clear_flag(g->nota, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_unlock();
}

void gestos_flecha(gestos_t *g, int on)
{
    if (NULL == g)
    {
        return;
    }
    lvgl_lock();
    g->flecha_oculta = !on;
    if (NULL != g->asta && NULL != g->cabeza)
    {
        if (on)
        {
            lv_obj_clear_flag(g->asta, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(g->cabeza, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(g->asta, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(g->cabeza, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lvgl_unlock();
}

void gestos_pintar(gestos_t *g, int dir)
{
    if (NULL == g)
    {
        return;
    }
    lvgl_lock();
    girar(g, dir);
    lvgl_unlock();
}

/* Esconder la pastilla no apaga la medicion: el arrastre se sigue midiendo y
 * apilando, y la direccion sigue llegando en ev.dir para quien la quiera
 * rotular con gestos_dir_texto()/gestos_dir_color().  Lo que si se apaga es la
 * escritura sobre sus objetos (ver mostrar()), asi que al volver hay que
 * repintarla con la ultima direccion; con GE_SINDIR se queda el "desliza el
 * dedo" del constructor, que es lo que habia antes de esconderla. */
void gestos_panel(gestos_t *g, int on)
{
    if (NULL == g || NULL == g->panel)
    {
        return;
    }
    lvgl_lock();
    if (on)
    {
        lv_obj_clear_flag(g->panel, LV_OBJ_FLAG_HIDDEN);
        if (g->dir_pintada != GE_SINDIR)
        {
            rotulo(g, gestos_dir_texto(g->dir_pintada),
                   gestos_dir_color(g->dir_pintada));
        }
    }
    else
    {
        lv_obj_add_flag(g->panel, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_unlock();
}

void gestos_cerrar(gestos_t *g)
{
    if (NULL == g)
    {
        return;
    }
    lvgl_lock();
    /* Borramos el fondo: se van con el los hijos y, sobre todo, los dos
     * lv_line, que guardan punteros a g->pts y g->cab. */
    lv_obj_del(g->fondo);
    lvgl_unlock();

    psram_free(g);
}
