/**
 * @file monitor.c
 *
 * Monitor de tareas para el P4: la version en pantalla del comando `ps`.
 *
 *   elf /apps/tareas/app.elf
 *
 * Lista a ancho fijo con scroll vertical (pantalla pequena): una fila por
 * tarea con TID, nombre, estado, %CPU instantaneo y una barra ASCII.  Al
 * tocar una tarea se abre una ventana de detalle con sus datos y un boton
 * MATAR para matarla.  Se vuelve a la lista con el mismo gesto que en el
 * resto de apps (arrastre hacia arriba desde el borde inferior), y desde la
 * lista, ese gesto sale de la app.
 *
 * MATAR mata la hebra, que es de lo que va este monitor: el ciclo de vida de
 * las apps ELF (running/background/closing) es cosa del appmgr y del launcher,
 * no de aqui.  Lo unico que se hace despues del kill es tirar la ranura del
 * appmgr si esa hebra tenia una: kill() libera la pila y la imagen ELF pero no
 * toca la tabla, y una ranura apuntando a un tid muerto deja el launcher
 * oculto detras de los widgets zombi y la app imposible de relanzar.
 *
 * CPU% es instantaneo: delta de cpu_ticks (1 tick = 1 ms) entre dos muestras
 * sobre el delta de millis() de la app.  El tope es 100%.
 *
 * Estado: PSRAM via psram_malloc(), nada de estaticas escribibles; los
 * callbacks de LVGL solo corren en lvglui.  El bucle de main saca eventos
 * con ui_poll() y es el unico que llama a exit().
 */

#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <timer.h>
#include <memory.h>
#include <device.h>
#include <thread.h>
#include <lvgl.h>
#include <appmgr.h>
#include <taskmon.h>
#include "gestos.h"

void exit(int codigo);

LV_FONT_DECLARE(lv_font_unscii_8);

/* ---- layout -------------------------------------------------------------- */

#define COL_X     8
#define COL_Y     48
#define COL_W     300
#define COL_H     340
#define ROW_H     17
/* taskmon_get() corta en max y recorre por tid ascendente: con 24 las que
 * faltaban eran justo las ultimas creadas (las hebras de audio de las apps).
 * NTHREAD es 100; 48 filas son 2496 B de array local sobre una pila de 16 KB. */
#define MAX_ROWS  48

#define ID_PAUSA  30
#define ID_SALIR  40
#define ID_MATAR  41

#define P_LISTA   0
#define P_DETALLE 1

#define C_FONDO   0x0B1622
#define C_TEXTO   0xE8D8B0
#define C_AMBER   0xFFB302
#define C_GRIS    0x5D6D7E
#define C_WHITE   0xFFFFFF
#define C_CYAN    0x2EC0E4
#define C_ROJO    0xE74C3C
#define C_VERDE   0x2ECC71
#define C_BOTON   0xD7E3EE

struct tareas_app
{
    ui_input_t *g;
    lv_obj_t *col;              /* contenedor scrollable con las filas        */
    lv_obj_t *pie;              /* pie con el conteo                          */
    lv_obj_t *detalle;          /* panel del detalle (NULL si no existe)      */
    lv_obj_t *det_txt;          /* rotulo con los datos de la tarea           */
    lv_obj_t *btn_matar;        /* MATAR: se oculta al salir del detalle     */

    struct taskmon_row filas[MAX_ROWS]; /* instantanea usada por las filas    */
    int n_filas;
    int pantalla;               /* P_LISTA / P_DETALLE                        */
    int sel_tid;                /* tid de la tarea seleccionada (-1 = nada)   */

    unsigned long prev_cpu[MAX_ROWS];   /* cpu_ticks de la ultima muestra     */
    int prev_tid[MAX_ROWS];             /* tid de cada entrada prev           */
    unsigned long prev_ms;              /* millis() de la ultima muestra      */
    int n_prev;

    int pausa;
    int slot;
};

/* ---- utilidades ---------------------------------------------------------- */

static const char *estado_corto(int s)
{
    static const char * const n[] = { "curr", "free", "ready", "recv",
                                      "sleep", "susp", "wait", "rtim", "migr" };

    if (s >= 1 && s <= 9)
    {
        return n[s - 1];
    }
    return "???";
}

static const char *estado_largo(int s)
{
    static const char * const n[] = {
        "curr (en CPU)", "free", "ready (lista)", "recv (esperando mensaje)",
        "sleep", "susp", "wait (semaphore)", "rtim (timeout)", "migr"
    };

    if (s >= 1 && s <= 9)
    {
        return n[s - 1];
    }
    return "???";
}

static void barra(int pct, char *out, size_t cap)
{
    int lleno = (pct * 8 + 50) / 100;
    int i, o = 0;

    if (lleno < 0) lleno = 0;
    if (lleno > 8) lleno = 8;

    for (i = 0; i < 8; i++)
    {
        out[o++] = (i < lleno) ? '#' : '-';
    }
    out[o] = '\0';
    (void)cap;
}

static lv_obj_t *rotulo(lv_obj_t *raiz, const char *txt, int x, int y,
                        unsigned rgb, lv_coord_t ancho)
{
    lv_obj_t *l = lv_label_create(raiz);

    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(l, lv_color_hex(rgb), 0);
    lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_width(l, ancho);
    lv_label_set_text(l, txt);
    return l;
}

/* ---- lista --------------------------------------------------------------- */

/* Borra las filas y las pinta de nuevo, posicionando cada una.  Scrollable:
 * la altura del contenido supera la del contenedor, y el scroll vertical
 * se hace con el dedo. */
static void pintar(struct tareas_app *t)
{
    struct taskmon_row filas[MAX_ROWS];
    int n, i;
    int elfs = 0;
    char linea[64];
    char bar[10];
    unsigned long ahora;
    int y = 0;

    n = taskmon_get(filas, MAX_ROWS);
    ahora = (unsigned long)millis();

    lvgl_lock();
    lv_obj_clean(t->col);

    for (i = 0; i < n; i++)
    {
        struct taskmon_row *f = &filas[i];
        lv_obj_t *l;
        int pct = 0;
        int k, j = -1;

        if (f->elf_task)
        {
            elfs++;
        }

        /* CPU% instantaneo: busco la muestra anterior de este tid */
        for (k = 0; k < t->n_prev; k++)
        {
            if (t->prev_tid[k] == f->tid)
            {
                j = k;
                break;
            }
        }
        if (j >= 0 && t->prev_ms > 0 && ahora > t->prev_ms)
        {
            unsigned long dc = f->cpu_ticks - t->prev_cpu[j];
            unsigned long dt = ahora - t->prev_ms;

            pct = (int)((dc * 100) / dt);
            if (pct > 100) pct = 100;
        }

        barra(pct, bar, sizeof(bar));
        snprintf(linea, sizeof(linea), "%2d %-14s %4s %3d%c %s",
                 f->tid, f->name, estado_corto(f->state), pct, '%', bar);

        l = lv_label_create(t->col);
        lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(
            f->cpu_ticks > 0 ? C_WHITE : C_GRIS), 0);
        lv_obj_set_pos(l, 0, y);
        lv_label_set_text(l, linea);
        y += ROW_H;
    }

    t->n_prev = n;
    for (i = 0; i < n; i++)
    {
        t->prev_tid[i] = filas[i].tid;
        t->prev_cpu[i] = filas[i].cpu_ticks;
    }
    t->prev_ms = ahora;
    memcpy(t->filas, filas, sizeof(filas));
    t->n_filas = n;

    snprintf(linea, sizeof(linea), "%d tareas%s   ELF: %d",
             n, t->pausa ? "  (PAUSA)" : "", elfs);
    lv_label_set_text(t->pie, linea);
    lvgl_unlock();
}

/* ---- detalle ------------------------------------------------------------- */

static const struct taskmon_row *buscar_tid(const struct tareas_app *t, int tid)
{
    int i;

    for (i = 0; i < t->n_filas; i++)
    {
        if (t->filas[i].tid == tid)
        {
            return &t->filas[i];
        }
    }
    return NULL;
}

static void abrir_detalle(struct tareas_app *t, int tid)
{
    const struct taskmon_row *f = buscar_tid(t, tid);
    char txt[256];

    if (NULL == f)
    {
        return;
    }
    t->sel_tid = tid;
    t->pantalla = P_DETALLE;

    snprintf(txt, sizeof(txt),
             "TID:      %d\n"
             "NOMBRE:   %s\n"
             "ESTADO:   %s\n"
             "PRIO:     %d   PADRE: %d\n"
             "CPU:      %lu ms\n"
             "PILA:     %lu/%lu B\n"
             "TIPO:     %s",
             f->tid, f->name, estado_largo(f->state),
             f->prio, f->parent,
             f->cpu_ticks, f->stk_peak, f->stk_total,
             f->elf_task ? "APP ELF" : "kernel");

    lvgl_lock();
    if (NULL == t->detalle)
    {
        t->detalle = lv_obj_create(ui_root(t->g));
        lv_obj_set_size(t->detalle, 320, 480);
        lv_obj_set_pos(t->detalle, 0, 0);
        lv_obj_set_style_bg_color(t->detalle, lv_color_hex(C_FONDO), 0);
        lv_obj_set_style_bg_opa(t->detalle, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(t->detalle, 2, 0);
        lv_obj_set_style_border_color(t->detalle, lv_color_hex(C_CYAN), 0);
        lv_obj_clear_flag(t->detalle, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(t->detalle, LV_OBJ_FLAG_CLICKABLE);

        /* El titulo se crea aqui, una sola vez: antes se hacia en cada
         * apertura y el panel acumulaba un rotulo por cada tarea visitada. */
        {
            lv_obj_t *tit = lv_label_create(t->detalle);

            lv_obj_clear_flag(tit, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_text_font(tit, &lv_font_unscii_8, 0);
            lv_obj_set_style_text_color(tit, lv_color_hex(C_AMBER), 0);
            lv_obj_set_pos(tit, 10, 8);
            lv_label_set_text(tit, "TAREA");
        }
        rotulo(t->detalle, "GESTO HACIA ARRIBA: VOLVER", 10, 452, C_GRIS, 300);
    }

    if (NULL == t->det_txt)
    {
        t->det_txt = lv_label_create(t->detalle);
        lv_obj_clear_flag(t->det_txt, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_text_font(t->det_txt, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(t->det_txt, lv_color_hex(C_TEXTO), 0);
        lv_obj_set_pos(t->det_txt, 10, 34);
        lv_obj_set_width(t->det_txt, 300);
    }
    lv_label_set_text(t->det_txt, txt);
    lv_obj_clear_flag(t->detalle, LV_OBJ_FLAG_HIDDEN);
    lvgl_unlock();

    /* MATAR se crea una sola vez y se oculta al salir, para no acumular
     * botones vivos ni perder ui_button() nunca.  No hay CERRAR: del detalle
     * se vuelve con el gesto de abajo hacia arriba, como en el resto de apps. */
    if (NULL == t->btn_matar)
    {
        t->btn_matar = ui_button(t->g, ID_MATAR, "MATAR", 95, 390, 130, 48, C_ROJO);
    }
    else
    {
        lvgl_lock();
        lv_obj_clear_flag(t->btn_matar, LV_OBJ_FLAG_HIDDEN);
        lvgl_unlock();
    }
}

static void cerrar_detalle(struct tareas_app *t)
{
    lvgl_lock();
    if (NULL != t->detalle)
    {
        lv_obj_add_flag(t->detalle, LV_OBJ_FLAG_HIDDEN);
    }
    if (NULL != t->btn_matar)
    {
        lv_obj_add_flag(t->btn_matar, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_unlock();

    t->pantalla = P_LISTA;
    t->sel_tid = -1;
    pintar(t);
}

/* ---- interfaz ------------------------------------------------------------ */

static void crear(struct tareas_app *t)
{
    lv_obj_t *raiz = ui_root(t->g);

    lvgl_lock();

    lv_obj_set_style_bg_color(raiz, lv_color_hex(C_FONDO), 0);
    lv_obj_set_style_bg_grad_dir(raiz, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(raiz, LV_OPA_COVER, 0);

    rotulo(raiz, "TAREAS", 10, 6, C_AMBER, 150);
    rotulo(raiz, "TID NOMBRE         ST   CPU", 10, 26, C_GRIS, 220);

    t->col = lv_obj_create(raiz);
    lv_obj_set_size(t->col, COL_W, COL_H);
    lv_obj_set_pos(t->col, COL_X, COL_Y);
    lv_obj_set_style_bg_opa(t->col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(t->col, 0, 0);
    lv_obj_set_style_pad_all(t->col, 0, 0);
    lv_obj_set_style_pad_row(t->col, 0, 0);
    lv_obj_add_flag(t->col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(t->col, LV_DIR_VER);
    /* lv_obj_create() nace CLICKABLE (lv_obj.c:436), asi que el CLICKED del
     * toque se quedaba aqui y nunca llegaba a ev_fondo, que la libreria solo
     * registra en el fondo: sin TAP no se abria el detalle.  Con EVENT_BUBBLE
     * el evento sube al padre (lv_event.c:478) y se convierte en UI_EVENT_TAP.
     * Un arrastre que llega a hacer scroll no emite CLICKED (lv_indev.c:974),
     * de modo que el scroll y el toque no se pisan. */
    lv_obj_add_flag(t->col, LV_OBJ_FLAG_EVENT_BUBBLE);

    t->pie = rotulo(raiz, "", 8, 394, C_CYAN, 240);

    lvgl_unlock();

    /* Mandos: X arriba-derecha; II (pausa/reanuda) abajo-derecha */
    ui_button(t->g, ID_SALIR, "X", 252, 8, 60, 40, C_ROJO);
    ui_button(t->g, ID_PAUSA, "II", 252, 412, 60, 40, C_BOTON);
}

/* ---- arranque ------------------------------------------------------------ */

static void ir_fondo(struct tareas_app *t, int *fuera);

static void procesar(struct tareas_app *t, ui_event_t *ev, int *fuera)
{
    if (UI_EVENT_SWIPE == ev->tipo)
    {
        /* Arriba en el borde inferior: en el detalle vuelve a la lista;
         * en la lista, al launcher. */
        if (UI_EDGE_BOTTOM == ev->edge && UI_DIR_UP == ev->dir)
        {
            if (P_DETALLE == t->pantalla)
            {
                cerrar_detalle(t);
            }
            else
            {
                ir_fondo(t, fuera);
            }
        }
        return;
    }

    if (UI_EVENT_TAP == ev->tipo)
    {
        if (P_LISTA != t->pantalla)
        {
            return;              /* en el detalle los toques no hacen nada */
        }
        /* ignora los toques sobre los botones X (252,8..48) e II (252,412..452) */
        if (ev->x1 >= 252 && ((ev->y1 >= 8 && ev->y1 < 48) ||
                              (ev->y1 >= 412 && ev->y1 < 452)))
        {
            return;
        }
        {
            int scroll, content_y, idx;

            lvgl_lock();
            scroll = (int)lv_obj_get_scroll_top(t->col);
            lvgl_unlock();
            content_y = ev->y1 - COL_Y + scroll;
            idx = content_y / ROW_H;

            if (idx >= 0 && idx < t->n_filas)
            {
                abrir_detalle(t, t->filas[idx].tid);
            }
        }
        return;
    }

    if (UI_EVENT_BUTTON != ev->tipo) return;

    if (ID_SALIR == ev->id)
    {
        *fuera = 1;
        return;
    }

    if (ID_PAUSA == ev->id)
    {
        t->pausa = !t->pausa;
        if (P_LISTA == t->pantalla)
        {
            pintar(t);
        }
        return;
    }

    if (ID_MATAR == ev->id)
    {
        if (P_DETALLE == t->pantalla && t->sel_tid >= 0)
        {
            tid_typ objetivo = (tid_typ)t->sel_tid;
            int slot;

            if (gettid() == objetivo)
            {
                *fuera = 1;    /* matar la app es cerrarla */
                return;
            }
            (void)kill(objetivo);
            /* kill() no toca el appmgr: si la hebra era una app ELF, su
             * ranura queda apuntando a un tid libre y el launcher se esconde
             * detras de sus widgets para siempre.  Tirar la ranura no es
             * cerrar la app cooperativamente, es borrar un dato muerto. */
            slot = appmgr_find_tid(objetivo);
            if (slot >= 0)
            {
                appmgr_unregister(slot);
            }
            cerrar_detalle(t);  /* vuelve a la lista y repinta */
        }
        return;
    }
}

static void salir(struct tareas_app *t)
{
    if (t->slot >= 0)
    {
        appmgr_unregister(t->slot);
    }
    ui_destroy(t->g);
    psram_free(t);
}

static void ir_fondo(struct tareas_app *t, int *fuera)
{
    ui_event_t ev;
    int s;

    if (t->slot < 0)
    {
        return;
    }
    ui_show(t->g, 0);
    appmgr_set_state(t->slot, APP_BACKGROUND);
    while (!*fuera)
    {
        s = appmgr_get_state(t->slot);
        if (APP_RUNNING == s)
        {
            ui_show(t->g, 1);
            return;
        }
        if (APP_CLOSING == s || APP_FREE == s)
        {
            *fuera = 1;
            return;
        }
        while (ui_poll(t->g, &ev))
        {
            ;
        }
        sleep(50);
    }
}

int main(int argc, char *argv[])
{
    struct tareas_app *t;
    ui_event_t ev;
    int fuera = 0;
    unsigned long t_ant = 0;

    (void)argc;
    (void)argv;

    printf("tareas: arranca (tid=%d)\r\n", gettid());

    t = (struct tareas_app *)psram_malloc(sizeof(struct tareas_app));
    if (NULL == t) { printf("tareas: sin PSRAM\r\n"); exit(1); }
    memset(t, 0, sizeof(struct tareas_app));
    t->slot = SYSERR;
    t->sel_tid = -1;

    t->g = ui_create_ex(UI_CHROME_NONE);
    if (NULL == t->g) { psram_free(t); exit(1); }

    t->slot = appmgr_register("Tareas", "/apps/tareas/app.elf", 0x2EC0E4,
                              gettid());

    crear(t);
    pintar(t);
    t_ant = (unsigned long)millis();

    while (!fuera)
    {
        while (ui_poll(t->g, &ev))
        {
            procesar(t, &ev, &fuera);
            if (fuera) break;
        }

        if (!fuera && !t->pausa && P_LISTA == t->pantalla &&
            (unsigned long)millis() - t_ant >= 1500)
        {
            pintar(t);
            t_ant = (unsigned long)millis();
        }

        if (!fuera && t->slot >= 0 &&
            APP_CLOSING == appmgr_get_state(t->slot))
        {
            fuera = 1;
        }
        if (!fuera) sleep(15);
    }

    salir(t);
    exit(0);
    return 0;
}
