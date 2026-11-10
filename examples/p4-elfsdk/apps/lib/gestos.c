/**
 * @file gestos.c
 * Input/gesture library implementation (linked into each ELF). No writable
 * file-scope statics; all state in PSRAM. Callbacks run in lvglui (lock held)
 * and only enqueue events.
 */
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <memory.h>
#include <lvgl.h>
#include "gestos.h"

#define GE_LIMO     40
#define GE_ANILLO   8
#define GE_BOTONES  32

#define UI_W            320
#define UI_H            480
#define UI_EDGE_MARGIN  48

#define LADO        160
#define CENTRO      (LADO / 2)
#define LARGO_PUNTA 58
#define LARGO_COLA  44
#define RETROCESO   24
#define APERTURA    22
#define GRUESO      14

#define COLOR_ARRIBA  0x2FFF5E
#define COLOR_ABAJO   0xE8453C
#define COLOR_IZQ     0x38A5FF
#define COLOR_DER     0xFFB302
#define COLOR_REPOSO  0x7A8C9C
#define COLOR_TEXTO   0xEAF2F8

struct boton
{
    gestos_t *g;
    int id;
    lv_obj_t *btn;
    lv_obj_t *lbl;
};

struct gestos
{
    lv_obj_t *fondo;
    lv_obj_t *panel;
    lv_obj_t *aviso;
    lv_obj_t *nota;
    lv_obj_t *asta;
    lv_obj_t *cabeza;
    lv_point_t pts[2];
    lv_point_t cab[3];
    struct boton botones[GE_BOTONES];
    int nbotones;
    /* SPSC ring: lvglui produces (lock held), app thread consumes. */
    volatile gev_t anillo[GE_ANILLO];
    volatile int cola, tope;
    volatile int prensado;
    volatile int x1, y1;
    volatile int arrastrado;
    int dir_pintada;
    int flecha_oculta;
};

static int entero(int v) { return v < 0 ? -v : v; }

static int direccion(int dx, int dy)
{
    if (entero(dx) >= entero(dy))
        return dx > 0 ? UI_DIR_RIGHT : UI_DIR_LEFT;
    return dy > 0 ? UI_DIR_DOWN : UI_DIR_UP;
}

static int start_edge(int x1, int y1)
{
    (void)x1;
    if (y1 >= (UI_H - UI_EDGE_MARGIN)) return UI_EDGE_BOTTOM;
    if (y1 <= UI_EDGE_MARGIN)          return UI_EDGE_TOP;
    if (x1 <= UI_EDGE_MARGIN)          return UI_EDGE_LEFT;
    if (x1 >= (UI_W - UI_EDGE_MARGIN)) return UI_EDGE_RIGHT;
    return UI_EDGE_NONE;
}

const char *ui_dir_name(int dir)
{
    switch (dir)
    {
    case UI_DIR_UP:    return "de abajo HACIA ARRIBA";
    case UI_DIR_DOWN:  return "de arriba HACIA ABAJO";
    case UI_DIR_LEFT:  return "de derecha A LA IZQUIERDA";
    case UI_DIR_RIGHT: return "de izquierda A LA DERECHA";
    default:           return "sin arrastre";
    }
}

unsigned int ui_dir_color(int dir)
{
    switch (dir)
    {
    case UI_DIR_UP:    return COLOR_ARRIBA;
    case UI_DIR_DOWN:  return COLOR_ABAJO;
    case UI_DIR_LEFT:  return COLOR_IZQ;
    case UI_DIR_RIGHT: return COLOR_DER;
    default:           return COLOR_REPOSO;
    }
}

static void apilar(gestos_t *g, int tipo, int dir, int x1, int y1, int x2,
                   int y2, int id, int edge)
{
    int n = (g->tope + 1) & (GE_ANILLO - 1);
    volatile gev_t *e;

    if (n == g->cola) return;
    e = &g->anillo[g->tope];
    e->tipo = tipo;
    e->dir = dir;
    e->id = id;
    e->x1 = x1; e->y1 = y1;
    e->x2 = x2; e->y2 = y2;
    e->dx = x2 - x1; e->dy = y2 - y1;
    e->edge = edge;
    g->tope = n;
}

static void girar(gestos_t *g, int d)
{
    int ux = 0, uy = 0, px = 0, py = 0;
    unsigned int c = ui_dir_color(d);
    int tx, ty;

    switch (d)
    {
    case UI_DIR_UP:    uy = -1; px = 1; break;
    case UI_DIR_DOWN:  uy = 1;  px = 1; break;
    case UI_DIR_LEFT:  ux = -1; py = 1; break;
    case UI_DIR_RIGHT: ux = 1;  py = 1; break;
    default:           uy = -1; px = 1; break;
    }

    tx = CENTRO + ux * LARGO_PUNTA;
    ty = CENTRO + uy * LARGO_PUNTA;
    g->pts[0].x = tx; g->pts[0].y = ty;
    g->pts[1].x = CENTRO - ux * LARGO_COLA;
    g->pts[1].y = CENTRO - uy * LARGO_COLA;
    g->cab[0].x = tx - ux * RETROCESO + px * APERTURA;
    g->cab[0].y = ty - uy * RETROCESO + py * APERTURA;
    g->cab[1].x = tx; g->cab[1].y = ty;
    g->cab[2].x = tx - ux * RETROCESO - px * APERTURA;
    g->cab[2].y = ty - uy * RETROCESO - py * APERTURA;

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
    if (NULL != g->aviso) lv_label_set_text(g->aviso, txt);
    if (NULL != g->panel)
        lv_obj_set_style_bg_color(g->panel, lv_color_hex(fondo), 0);
}

static void mostrar(gestos_t *g, int d)
{
    if (!g->flecha_oculta) girar(g, d);
    if (NULL != g->panel && !lv_obj_has_flag(g->panel, LV_OBJ_FLAG_HIDDEN))
        rotulo(g, ui_dir_name(d), ui_dir_color(d));
    g->dir_pintada = d;
}

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

/* Callbacks: run in lvglui, enqueue + paint, NEVER print. */

static void ev_fondo(lv_event_t *e)
{
    lv_event_code_t cod = lv_event_get_code(e);
    gestos_t *g = lv_event_get_user_data(e);
    lv_indev_t *in = lv_event_get_indev(e);
    lv_point_t p;
    int dx, dy, d;

    if (NULL == in) return;
    lv_indev_get_point(in, &p);

    if (cod == LV_EVENT_PRESSED)
    {
        g->x1 = p.x; g->y1 = p.y;
        g->prensado = 1;
        g->arrastrado = 0;
    }
    else if (cod == LV_EVENT_PRESSING)
    {
        if (g->prensado != 0)
        {
            dx = p.x - g->x1; dy = p.y - g->y1;
            if (entero(dx) >= GE_LIMO || entero(dy) >= GE_LIMO)
            {
                d = direccion(dx, dy);
                if (d != g->dir_pintada) mostrar(g, d);
            }
        }
    }
    else if (cod == LV_EVENT_RELEASED)
    {
        if (g->prensado != 0)
        {
            g->prensado = 0;
            dx = p.x - g->x1; dy = p.y - g->y1;
            g->arrastrado = (entero(dx) >= GE_LIMO || entero(dy) >= GE_LIMO);
            if (g->arrastrado)
            {
                d = direccion(dx, dy);
                apilar(g, UI_EVENT_SWIPE, d, g->x1, g->y1, p.x, p.y, 0,
                       start_edge(g->x1, g->y1));
                mostrar(g, d);
            }
        }
    }
    else if (cod == LV_EVENT_CLICKED)
    {
        apilar(g, UI_EVENT_TAP, UI_DIR_NONE, p.x, p.y, p.x, p.y, 0,
               UI_EDGE_NONE);
        rotulo(g, "TOQUE, sin arrastre", COLOR_REPOSO);
    }
    else if (cod == LV_EVENT_LONG_PRESSED)
    {
        apilar(g, UI_EVENT_LONG_PRESS, UI_DIR_NONE, p.x, p.y, p.x, p.y, 0,
               UI_EDGE_NONE);
        rotulo(g, "PULSACION LARGA", COLOR_REPOSO);
    }
}

static void ev_boton(lv_event_t *e)
{
    struct boton *sl = lv_event_get_user_data(e);
    gestos_t *g = sl->g;
    lv_point_t p;

    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (g->arrastrado) return;
    lv_indev_get_point(lv_event_get_indev(e), &p);
    apilar(g, UI_EVENT_BUTTON, UI_DIR_NONE, p.x, p.y, p.x, p.y, sl->id,
           UI_EDGE_NONE);
}

/* ---- public API ---- */

ui_input_t *ui_create(void)
{
    return ui_create_ex(UI_CHROME_ALL);
}

ui_input_t *ui_create_ex(int chrome)
{
    gestos_t *g;

    if (SYSERR == control(LVGL0, LVGL_CTRL_READY, 0, 0))
    {
        printf("ui: LVGL no esta inicializado\r\n");
        return NULL;
    }

    g = psram_malloc(sizeof(gestos_t));
    if (NULL == g)
    {
        printf("ui: no hay PSRAM para el handle\r\n");
        return NULL;
    }
    memset(g, 0, sizeof(gestos_t));
    g->dir_pintada = UI_DIR_NONE;
    g->flecha_oculta = (0 == (chrome & UI_CHROME_ARROW));

    lvgl_lock();

    g->fondo = lv_obj_create(lv_scr_act());
    lv_obj_set_size(g->fondo, LV_PCT(100), LV_PCT(100));
    lv_obj_align(g->fondo, LV_ALIGN_CENTER, 0, 0);
    limpiar(g->fondo);
    lv_obj_set_style_bg_color(g->fondo, lv_color_hex(0x07131f), 0);
    lv_obj_set_style_bg_grad_dir(g->fondo, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(g->fondo, LV_OPA_COVER, 0);
    lv_obj_add_flag(g->fondo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g->fondo, ev_fondo, LV_EVENT_ALL, g);

    if (chrome & UI_CHROME_PILL)
    {
        g->panel = lv_obj_create(g->fondo);
        lv_obj_set_size(g->panel, 300, 46);
        lv_obj_align(g->panel, LV_ALIGN_TOP_MID, 0, 14);
        limpiar(g->panel);
        lv_obj_clear_flag(g->panel, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_grad_dir(g->panel, LV_GRAD_DIR_NONE, 0);
        lv_obj_set_style_radius(g->panel, LV_RADIUS_CIRCLE, 0);

        g->aviso = lv_label_create(g->panel);
        lv_label_set_text(g->aviso, "");
        lv_obj_set_style_text_color(g->aviso, lv_color_hex(0x101820), 0);
        lv_obj_align(g->aviso, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(g->aviso, LV_OBJ_FLAG_CLICKABLE);
    }

    if (chrome & UI_CHROME_ARROW)
    {
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

        girar(g, UI_DIR_UP);
        lv_obj_set_style_line_color(g->asta, lv_color_hex(COLOR_REPOSO), 0);
        lv_obj_set_style_line_color(g->cabeza, lv_color_hex(COLOR_REPOSO), 0);
    }

    if (chrome & UI_CHROME_NOTE)
    {
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

int ui_poll(ui_input_t *g, ui_event_t *ev)
{
    int c = g->cola;
    volatile gev_t *e;

    if (c == g->tope) return 0;
    e = &g->anillo[c];
    ev->tipo = e->tipo;
    ev->dir = e->dir;
    ev->id = e->id;
    ev->x1 = e->x1; ev->y1 = e->y1;
    ev->x2 = e->x2; ev->y2 = e->y2;
    ev->dx = e->dx; ev->dy = e->dy;
    ev->edge = e->edge;
    g->cola = (c + 1) & (GE_ANILLO - 1);
    return 1;
}

lv_obj_t *ui_button_on(ui_input_t *g, int id, const char *txt, lv_obj_t *padre,
                       int x, int y, int w, int h, unsigned int rgb)
{
    struct boton *sl;
    lv_obj_t *b, *t;

    if (NULL == g || NULL == padre || g->nbotones >= GE_BOTONES) return NULL;
    sl = &g->botones[g->nbotones];
    sl->g = g; sl->id = id;

    lvgl_lock();
    b = lv_btn_create(padre);
    lv_obj_set_size(b, w, h);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(rgb), 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_add_event_cb(b, ev_boton, LV_EVENT_CLICKED, sl);
    lv_obj_add_event_cb(b, ev_fondo, LV_EVENT_PRESSED, g);
    lv_obj_add_event_cb(b, ev_fondo, LV_EVENT_PRESSING, g);
    lv_obj_add_event_cb(b, ev_fondo, LV_EVENT_RELEASED, g);
    t = lv_label_create(b);
    lv_label_set_text(t, txt);
    lv_obj_set_style_text_color(t, lv_color_hex(0x101820), 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 0);
    sl->lbl = t; sl->btn = b;
    lvgl_unlock();

    g->nbotones++;
    return b;
}

lv_obj_t *ui_button(ui_input_t *g, int id, const char *txt, int x, int y,
                    int w, int h, unsigned int rgb)
{
    if (NULL == g) return NULL;
    return ui_button_on(g, id, txt, g->fondo, x, y, w, h, rgb);
}

lv_obj_t *ui_root(ui_input_t *g) { return g ? g->fondo : NULL; }

void ui_show(ui_input_t *g, int on)
{
    if (NULL == g || NULL == g->fondo) return;
    lvgl_lock();
    if (on) lv_obj_clear_flag(g->fondo, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(g->fondo, LV_OBJ_FLAG_HIDDEN);
    lvgl_unlock();
}

void ui_button_set_label(ui_input_t *g, int id, const char *txt)
{
    int i;
    if (NULL == g) return;
    lvgl_lock();
    for (i = 0; i < g->nbotones; i++)
        if (g->botones[i].id == id) { lv_label_set_text(g->botones[i].lbl, txt); break; }
    lvgl_unlock();
}

void ui_buttons_clear(ui_input_t *g)
{
    int i;
    if (NULL == g) return;
    lvgl_lock();
    for (i = 0; i < g->nbotones; i++)
        if (NULL != g->botones[i].btn)
        {
            lv_obj_del(g->botones[i].btn);
            g->botones[i].btn = NULL; g->botones[i].lbl = NULL;
        }
    g->nbotones = 0;
    lvgl_unlock();
}

void ui_set_title(ui_input_t *g, const char *txt)
{
    if (NULL == g || NULL == g->aviso) return;
    lvgl_lock();
    lv_label_set_text(g->aviso, txt);
    lvgl_unlock();
}

void ui_set_note(ui_input_t *g, const char *txt)
{
    if (NULL == g || NULL == g->nota) return;
    lvgl_lock();
    lv_label_set_text(g->nota, txt);
    if ('\0' == txt[0]) lv_obj_add_flag(g->nota, LV_OBJ_FLAG_HIDDEN);
    else                lv_obj_clear_flag(g->nota, LV_OBJ_FLAG_HIDDEN);
    lvgl_unlock();
}

void ui_show_arrow(ui_input_t *g, int on)
{
    if (NULL == g) return;
    lvgl_lock();
    g->flecha_oculta = !on;
    if (NULL != g->asta && NULL != g->cabeza)
    {
        if (on) { lv_obj_clear_flag(g->asta, LV_OBJ_FLAG_HIDDEN);
                  lv_obj_clear_flag(g->cabeza, LV_OBJ_FLAG_HIDDEN); }
        else    { lv_obj_add_flag(g->asta, LV_OBJ_FLAG_HIDDEN);
                  lv_obj_add_flag(g->cabeza, LV_OBJ_FLAG_HIDDEN); }
    }
    lvgl_unlock();
}

void ui_set_arrow_dir(ui_input_t *g, int dir)
{
    if (NULL == g) return;
    lvgl_lock();
    girar(g, dir);
    lvgl_unlock();
}

void ui_show_panel(ui_input_t *g, int on)
{
    if (NULL == g || NULL == g->panel) return;
    lvgl_lock();
    if (on)
    {
        lv_obj_clear_flag(g->panel, LV_OBJ_FLAG_HIDDEN);
        if (g->dir_pintada != UI_DIR_NONE)
            rotulo(g, ui_dir_name(g->dir_pintada), ui_dir_color(g->dir_pintada));
    }
    else
        lv_obj_add_flag(g->panel, LV_OBJ_FLAG_HIDDEN);
    lvgl_unlock();
}

void ui_destroy(ui_input_t *g)
{
    if (NULL == g) return;
    lvgl_lock();
    lv_obj_del(g->fondo);
    lvgl_unlock();
    psram_free(g);
}
