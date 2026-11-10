/**
 * @file gestos.h
 *
 * Input/gesture library for P4 ELF apps (linked into each ELF via ld -r).
 * ELF rules: call ONLY symbols exported by system/exports.c, and keep NO
 * writable file-scope statics (R_RISCV_LO12_S local-anchor relocs not applied)
 * — all state lives in a PSRAM handle.
 *
 * The resident 'lvglui' thread pumps LVGL (app never calls lv_timer_handler()).
 * Callbacks run in lvglui with lvgl_lock() held and only enqueue events; the
 * app polls ui_poll() from its own thread. lvgl_lock() is NOT recursive — do
 * not call these functions from an LVGL callback. exit() kills the calling
 * thread: call it from main's loop, never from a callback.
 */
#ifndef APPLIB_GESTOS_H
#define APPLIB_GESTOS_H

#include "lvgl/lvgl.h"

typedef struct gestos gestos_t;
typedef struct gestos ui_input_t;

/* ---- event types ---- */
#define UI_EVENT_NONE       0
#define UI_EVENT_SWIPE      1   /* traveled >= threshold */
#define UI_EVENT_TAP        2
#define UI_EVENT_LONG_PRESS 3
#define UI_EVENT_BUTTON     4

/* ---- direction (dominant axis) ---- */
#define UI_DIR_UP    0
#define UI_DIR_DOWN  1
#define UI_DIR_LEFT  2
#define UI_DIR_RIGHT 3
#define UI_DIR_NONE  4

/* ---- edge where the gesture started ---- */
#define UI_EDGE_NONE   0
#define UI_EDGE_TOP    1
#define UI_EDGE_BOTTOM 2
#define UI_EDGE_LEFT   3
#define UI_EDGE_RIGHT  4

/* Event. Field names tipo/dir/id kept for backward compatibility with gev_t;
 * 'edge' (start edge) is new. */
typedef struct
{
    int tipo;                   /* UI_EVENT_* (compat name) */
    int dir;                    /* UI_DIR_* */
    int id;                     /* UI_EVENT_BUTTON button id */
    int x1, y1, x2, y2;
    int dx, dy;
    int edge;                   /* UI_EDGE_* where it started */
} ui_event_t;
typedef ui_event_t gev_t;

/* ---- optional visual chrome ---- */
#define UI_CHROME_NONE  0x00
#define UI_CHROME_PILL  0x01
#define UI_CHROME_ARROW 0x02
#define UI_CHROME_NOTE  0x04
#define UI_CHROME_ALL   (UI_CHROME_PILL | UI_CHROME_ARROW | UI_CHROME_NOTE)

ui_input_t *ui_create(void);
ui_input_t *ui_create_ex(int chrome);
int ui_poll(ui_input_t *ui, ui_event_t *ev);

lv_obj_t *ui_button(ui_input_t *ui, int id, const char *label, int x, int y,
                    int w, int h, unsigned int rgb);
lv_obj_t *ui_button_on(ui_input_t *ui, int id, const char *label,
                       lv_obj_t *parent, int x, int y, int w, int h,
                       unsigned int rgb);
void ui_button_set_label(ui_input_t *ui, int id, const char *label);
void ui_buttons_clear(ui_input_t *ui);

lv_obj_t *ui_root(ui_input_t *ui);
void ui_show(ui_input_t *ui, int on);

void ui_set_title(ui_input_t *ui, const char *text);
void ui_set_note(ui_input_t *ui, const char *text);
void ui_show_arrow(ui_input_t *ui, int on);
void ui_set_arrow_dir(ui_input_t *ui, int dir);
void ui_show_panel(ui_input_t *ui, int on);

const char *ui_dir_name(int dir);
unsigned int ui_dir_color(int dir);

void ui_destroy(ui_input_t *ui);

/* ---- backward-compatible aliases ---- */
#define GE_NADA      UI_EVENT_NONE
#define GE_ARRASTRE  UI_EVENT_SWIPE
#define GE_TOQUE     UI_EVENT_TAP
#define GE_LARGA     UI_EVENT_LONG_PRESS
#define GE_BOTON     UI_EVENT_BUTTON

#define GE_ARRIBA UI_DIR_UP
#define GE_ABAJO  UI_DIR_DOWN
#define GE_IZQ    UI_DIR_LEFT
#define GE_DER    UI_DIR_RIGHT
#define GE_SINDIR UI_DIR_NONE

#define GE_PASTILLA UI_CHROME_PILL
#define GE_FLECHA   UI_CHROME_ARROW
#define GE_NOTA     UI_CHROME_NOTE
#define GE_ADORNOS  UI_CHROME_ALL

#define gestos_nueva()               ui_create()
#define gestos_nueva_partes(p)       ui_create_ex(p)
#define gestos_leer(g, ev)           ui_poll((g), (ev))
#define gestos_boton(g,i,t,x,y,w,h,c) ui_button((g),(i),(t),(x),(y),(w),(h),(c))
#define gestos_boton_en(g,i,t,p,x,y,w,h,c) ui_button_on((g),(i),(t),(p),(x),(y),(w),(h),(c))
#define gestos_boton_texto(g,i,t)    ui_button_set_label((g),(i),(t))
#define gestos_botones_limpiar(g)    ui_buttons_clear(g)
#define gestos_raiz(g)               ui_root(g)
#define gestos_show(g,on)            ui_show((g),(on))
#define gestos_rotulo(g,t)           ui_set_title((g),(t))
#define gestos_nota(g,t)             ui_set_note((g),(t))
#define gestos_flecha(g,on)          ui_show_arrow((g),(on))
#define gestos_pintar(g,d)           ui_set_arrow_dir((g),(d))
#define gestos_panel(g,on)           ui_show_panel((g),(on))
#define gestos_dir_texto(d)          ui_dir_name(d)
#define gestos_dir_color(d)          ui_dir_color(d)
#define gestos_cerrar(g)             ui_destroy(g)

#endif /* APPLIB_GESTOS_H */
