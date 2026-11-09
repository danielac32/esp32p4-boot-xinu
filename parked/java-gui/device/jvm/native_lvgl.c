/*
 * native_lvgl.c — natives de esp32p4.LVGL sobre el LVGL0 del ESP32-P4.
 *
 * El contrato es el de jvm/lib/esp32p4/LVGL.java (los .class del SDK se
 * enlazan contra esos nombres): un handle entero por widget, colores
 * 0xRRGGBB, tick() = avanzar reloj + render, y pollEvent()/eventWidget()
 * para los toques.
 *
 * El arbol de objetos de LVGL NO es reentrante y el device lvgl lo serializa
 * con su monitor, asi que cada rafaga de llamadas va entre lvgl_lock() y
 * lvgl_unlock().  Se coge el monitor por operacion de Java, no por cada lv_*:
 * una rafaga (crear el boton y poner su texto) tiene que verse atomica desde
 * otra hebra java que refresque la misma pantalla.
 *
 * El handle es un indice + 1 en wtab[], no el puntero del objeto (asi lo
 * hacia el puerto viejo): la VM no tiene GC, y un int que se queda viejo tiene
 * que ser inofensivo (la llamada devuelve fallo), no un salto a memoria
 * libre.  Las clases del widget se guardan en el slot porque setText() de un
 * boton escribe en su label hijo, no en el boton.
 *
 * Unicas dos cosas que mueren de verdad son del().  Por eso el buffer PSRAM
 * de una imagen vive en su slot y solo se libera cuando LVGL borra el objeto
 * (del(), clear(), init()); forget sin borrar el objeto nunca libera la
 * reserva, porque el pixel data sigue colgado del árbol.
 *
 * init() pinta la pantalla en blanco a proposito (deja el screen activo vacio
 * y olvida sus handles) porque hoy la placa corre UNA app java por vez desde
 * la shell.  Con el workspace (#82) eso cambia: cada app tendra su propio
 * espacio de handles y su screen, y init() dejara de tocar el arbol ajeno.
 * Mientras tanto los screens que crea screen() se acumulan --unos cientos de
 * bytes por arranque de app--, que es el precio de no borrar lo que otra vida
 * pudo haber dejado colgado.
 *
 * Convencion de pila: el ULTIMO argumento esta en la cima, y solo se empuja
 * valor de retorno cuando el descriptor de la firma lo pide (los void no
 * empujan nada, igual que HW.delay o IO.print).
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <kernel.h>
#include <device.h>
#include <stdio.h>
#include <string.h>
#include <memory.h>
#include <timer.h>

#include "simple_jvm.h"
#include "java_lib.h"
#include "jvm_io.h"
#include <fat_filelib.h>
#include <lcd.h>
#include <lvgl.h>

#include "lvgl/lvgl.h"

/* 32 B por slot: con 96 el launcher (barra de estado + rejilla + banner) mas
 * dos apps vivas se pasaba del limite y img()/button() empezaban a devolver 0
 * en mitad de una pantalla. */
#define WIDGET_MAX   160      /**< slots de handle                                   */
#define EVENT_QUEUE  16       /**< eventos pulsados pendientes                       */
#define LEVENT_MAX   8        /**< ultimos eventos por tarea java                    */

/* Que es cada handle, para que setText/setColor sepan sobre quien escribir. */
#define KIND_NONE    0
#define KIND_SCREEN  1
#define KIND_LABEL   2
#define KIND_BUTTON  3
#define KIND_RECT    4
#define KIND_IMG     5

struct widget_slot
{
    lv_obj_t *obj;            /* 0 = slot libre                                     */
    int kind;
    int owner;                /* tid de la tarea java que cree el objeto            */
    uint16_t *px;             /* RGB565 crudo en PSRAM; solo KIND_IMG              */
    lv_img_dsc_t dsc;         /* descriptor que apunta a px; solo KIND_IMG          */
    char *src;               /* ruta "S:/..." en PSRAM; JPEG, ver img()           */
};

struct event_slot
{
    int handle;
    int code;
    int dir;                  /* LV_DIR_* solo si code == LV_EVENT_GESTURE          */
    int owner;                /* tid dueno del widget: ver pollEvent()              */
};

/* El ultimo evento consumido es por tarea: si fuera una unica variable, la
 * hebra en segundo plano se llevaria el handle de la que esta en frente. */
struct levent_slot
{
    int tid;
    int handle;
    int code;
    int dir;
};

static struct widget_slot wtab[WIDGET_MAX];

/* Handle del screen que tenia el panel justo antes del ultimo screen().  Sirve
 * para devolverselo a su dueno cuando la app que lo tomo se va sin que nadie
 * se lo pida (arrancada desde la shell, o muerta en frente). */
static int screen_prev;
static struct event_slot  eq[EVENT_QUEUE];
static int eq_head, eq_count;
static struct levent_slot levent[LEVENT_MAX];
static int lvgl_up = 0;

static void pop_text(StackFrame *stack, SimpleConstantPool *p, char *out,
                     int max)
{
    jvm_pop_text(stack, p, out, max);
}

/* ---- estado del device ------------------------------------------------ */

/* El devtab inicializa LVGL0 en el arranque; si la reserva de PSRAM de los
 * buffers de dibujo fallo, el device queda SYSERR y las natives devuelven
 * "sin UI" en lugar de correr por punteros nulos.  Se vuelve a preguntar
 * hasta que responda OK: un no del principio no puede quedar cacheado. */
static int ui_ready(void)
{
    if (!lvgl_up)
    {
        lvgl_up = (control(LVGL0, LVGL_CTRL_READY, 0, 0) == OK) ? 1 : 0;
    }
    return lvgl_up;
}

/* ---- tabla de handles ------------------------------------------------- */

static int widget_add(lv_obj_t *obj, int kind)
{
    int i;

    if (obj == 0)
    {
        return 0;
    }
    for (i = 0; i < WIDGET_MAX; i++)
    {
        if (wtab[i].obj == 0)
        {
            wtab[i].obj = obj;
            wtab[i].kind = kind;
            wtab[i].owner = (int) gettid();
            wtab[i].px = 0;
            wtab[i].src = 0;
            return i + 1;
        }
    }
    /* Sin slots: se tira el objeto y se devuelve fallo, para que la pila de
     * la VM no quede con un handle que apunta a la nada. */
    lv_obj_del(obj);
    return 0;
}

static struct widget_slot *widget_slot(int handle)
{
    if (handle <= 0 || handle > WIDGET_MAX || wtab[handle - 1].obj == 0)
    {
        return 0;
    }
    return &wtab[handle - 1];
}

/* Slot solo si lo creo la tarea que llama: el launcher y las apps comparten
 * la tabla, y un handle ajeno (por numero) no autoriza a borrar ni a mover.
 * Los screens hacen de excepcion: ver widget_slot_screen(). */
static struct widget_slot *widget_slot_mine(int handle)
{
    struct widget_slot *s = widget_slot(handle);

    if (s != 0 && s->owner == (int) gettid())
    {
        return s;
    }
    return 0;
}

/* Un screen es recurso comun del panel: cualquiera puede mirarlo y cargarlo,
 * pero solo su dueno lo borra. */
static struct widget_slot *widget_slot_screen(int handle)
{
    struct widget_slot *s = widget_slot(handle);

    return (s != 0 && s->kind == KIND_SCREEN) ? s : 0;
}

static lv_obj_t *widget_obj(int handle)
{
    struct widget_slot *s = widget_slot(handle);

    return s ? s->obj : 0;
}

/* Objeto solo si es de la tarea que llama: para mutar (setPos/setSize). */
static lv_obj_t *widget_obj_mine(int handle)
{
    struct widget_slot *s = widget_slot_mine(handle);

    return s ? s->obj : 0;
}

/* El screen de la tarea que llama, o 0 si aun no ha llamado a screen(). */
static lv_obj_t *my_screen(void)
{
    int i, tid = (int) gettid();
    lv_obj_t *scr = 0;

    for (i = 0; i < WIDGET_MAX; i++)
    {
        if (wtab[i].obj != 0 && wtab[i].kind == KIND_SCREEN &&
            wtab[i].owner == tid)
        {
            scr = wtab[i].obj;      /* el mas reciente gana */
        }
    }
    return scr;
}

/* Slot del screen mas reciente de `tid`, o -1 si no tiene ninguno registrado. */
static int screen_slot_de(int tid)
{
    int i, mejor = -1;

    for (i = 0; i < WIDGET_MAX; i++)
    {
        if (wtab[i].obj != 0 && wtab[i].kind == KIND_SCREEN &&
            wtab[i].owner == tid)
        {
            mejor = i;                  /* el mas reciente gana */
        }
    }
    return mejor;
}

/* Handle (+1) del slot que registra ese objeto, 0 si no esta registrado. */
static int screen_handle_de(lv_obj_t *obj)
{
    int i;

    for (i = 0; i < WIDGET_MAX; i++)
    {
        if (wtab[i].obj != 0 && wtab[i].obj == obj &&
            wtab[i].kind == KIND_SCREEN)
        {
            return i + 1;
        }
    }
    return 0;
}

/**
 * Apartar el panel de los screens de `tid`.
 *
 * lv_obj_del() sobre el screen activo deja disp->act_scr en NULL y la hebra
 * residente lvglui lo refresca enseguida: lv_refr.c:309 llama a
 * lv_obj_update_layout(disp_refr->act_scr) sin mirarlo y esa lectura sobre NULL
 * es el load access fault (mepc dentro de lv_obj_update_layout, mtval=0x22) que
 * se vio al cerrar una app.  El display necesita siempre una pantalla, asi que
 * si el panel esta mostrando un screen de la tarea que muere se carga otra
 * antes del borrado, en este orden:
 *
 *   1. el screen que tenia el panel antes de que esta tarea lo tomara
 *      (screen_prev) --es el dueno legitimo del panel;
 *   2. el screen de quien la creo (el launcher es el padre);
 *   3. cualquier otro screen registrado;
 *   4. una pantalla vacia, creada aqui mismo.
 *
 * El orden importa: con el 3 antes que el 1, una sonda arrancada desde la shell
 * devolvia el panel a una app minimizada en vez de al launcher, y el dedo
 * siguiente caia sobre la app que se creia ya fuera.
 *
 * Sin esto solo era seguro cerrar por las buenas desde el launcher, que carga su
 * pantalla antes de mandar MSG_QUIT; una app arrancada desde la shell se mataba
 * a si misma con el screen delante.
 */
static void screen_apartar(int tid)
{
    lv_obj_t *act = lv_scr_act();
    lv_obj_t *nuevo;
    struct widget_slot *s;
    int i, en_pantalla = 0, sust;

    if (act == 0)
    {
        return;
    }
    for (i = 0; i < WIDGET_MAX; i++)
    {
        if (wtab[i].obj == act && wtab[i].kind == KIND_SCREEN &&
            wtab[i].owner == tid)
        {
            en_pantalla = 1;
            break;
        }
    }
    if (!en_pantalla)
    {
        return;                         /* el panel no muestra un screen de tid */
    }

    s = widget_slot(screen_prev);
    if (s != 0 && s->kind == KIND_SCREEN && s->owner != tid)
    {
        lv_scr_load(s->obj);
        return;
    }

    sust = isbadtid(tid) ? -1 : screen_slot_de((int) thrtab[tid].parent);
    if (sust < 0)
    {
        for (i = 0; i < WIDGET_MAX; i++)
        {
            if (wtab[i].obj != 0 && wtab[i].kind == KIND_SCREEN &&
                wtab[i].owner != tid && wtab[i].owner != 0)
            {
                sust = i;
            }
        }
    }
    if (sust >= 0)
    {
        lv_scr_load(wtab[sust].obj);
        return;
    }

    /* Nada registrado que poner: se crea una pantalla vacia.  No se registra
     * porque su dueno es la vida que viene, no esta que se va; son unos
     * cientos de bytes por app huerfana. */
    nuevo = lv_obj_create(NULL);
    if (nuevo != 0)
    {
        lv_obj_clear_flag(nuevo, LV_OBJ_FLAG_SCROLLABLE);
        lv_scr_load(nuevo);
    }
}

/* Un handle viejo, 0 o de otra tarea cae al screen de quien llama; si no
 * tiene, al activo.  Con varias apps vivas, "el activo" puede ser el del
 * launcher: pintar encima del launcher seria romper la interfaz de otra tarea,
 * asi que la hebra que no tiene screen propio no hereda el de nadie. */
static lv_obj_t *widget_parent(int handle)
{
    lv_obj_t *o = widget_obj(handle);
    lv_obj_t *mine;

    if (o != 0)
    {
        return o;
    }
    mine = my_screen();
    return mine ? mine : lv_scr_act();
}

/* Olvida el slot sin tocar el objeto: para el caso en que LVGL ya lo borro
 * (lv_obj_clean) o en que se le va a borrar justo despues.  El dueno del slot
 * decide si la reserva de pixeles se libera, porque eso depende de que el
 * objeto haya muerto de verdad. */
static void widget_forget(int handle)
{
    struct widget_slot *s = widget_slot(handle);
    int i;

    if (s == 0)
    {
        return;
    }
    s->obj = 0;
    s->kind = KIND_NONE;
    s->owner = 0;

    /* Tirar del evento en cola que senalaba a este widget: si no,
     * eventWidget() podria devolver un handle ya liberado. */
    for (i = 0; i < eq_count; i++)
    {
        int k = (eq_head + i) % EVENT_QUEUE;

        if (eq[k].handle == handle)
        {
            eq[k].handle = 0;
        }
    }
}

/* Barrido de seguridad: LVGL borra el arbol completo cuando muere un padre,
 * y un hijo registrado por la misma u otra tarea quedaria como puntero colgante.
 * Se comprueba cada slot con lv_obj_is_valid() --recorre las pantallas del
 * display-- y se olvida lo que ya no esta.  Caro (O(arbol x slots)), pero
 * solo despues de un borrado en bloque. */
static void widget_sweep_dead(void)
{
    int i;

    for (i = 0; i < WIDGET_MAX; i++)
    {
        if (wtab[i].obj != 0 && !lv_obj_is_valid(wtab[i].obj))
        {
            if (wtab[i].px != 0)
            {
                psram_free(wtab[i].px);
                wtab[i].px = 0;
            }
            if (wtab[i].src != 0)
            {
                psram_free(wtab[i].src);
                wtab[i].src = 0;
            }
            wtab[i].obj = 0;
            wtab[i].kind = KIND_NONE;
            wtab[i].owner = 0;
        }
    }
}

/* Vaciar la parte de la tabla que pertenece a `tid` despues de que la tarea
 * haya borrado sus propios objetos: sus widgets murieron con ellos, asi que su
 * reserva de pixeles se devuelve aqui.  Los demas duenios (el launcher y las
 * apps en segundo plano) conservan sus objetos y sus handles intactos; por eso
 * se filtra por duenio y no se tocan los KIND_SCREEN. */
static void widget_clean_owner(int tid)
{
    int i;

    for (i = 0; i < WIDGET_MAX; i++)
    {
        if (wtab[i].obj == 0 || wtab[i].owner != tid)
        {
            continue;
        }
        if (wtab[i].kind == KIND_SCREEN)
        {
            continue;
        }
        if (wtab[i].px != 0)
        {
            psram_free(wtab[i].px);
            wtab[i].px = 0;
        }
        if (wtab[i].src != 0)
        {
            psram_free(wtab[i].src);
            wtab[i].src = 0;
        }
        wtab[i].obj = 0;
        wtab[i].kind = KIND_NONE;
        wtab[i].owner = 0;
    }
    for (i = 0; i < eq_count; i++)
    {
        int k = (eq_head + i) % EVENT_QUEUE;

        if (eq[k].owner == tid)
        {
            eq[k].handle = 0;
        }
    }
    for (i = 0; i < LEVENT_MAX; i++)
    {
        if (levent[i].tid == tid)
        {
            levent[i].tid = 0;
            levent[i].handle = 0;
            levent[i].code = 0;
        }
    }
}

/* Borrar todos los objetos de `tid` (salvo sus screens) y olvidar sus slots.
 * Es lo que hace clear() y lo que hara el kernel cuando la tarea muera.  Los
 * hijos de un objeto borrado en bloque se recogen con el barrido. */
static void widget_delete_owner(int tid)
{
    int i;

    for (i = 0; i < WIDGET_MAX; i++)
    {
        if (wtab[i].obj == 0 || wtab[i].owner != tid ||
            wtab[i].kind == KIND_SCREEN)
        {
            continue;
        }
        lv_obj_del(wtab[i].obj);
        if (wtab[i].src != 0)
        {
            psram_free(wtab[i].src);
        }
        if (wtab[i].px != 0)
        {
            psram_free(wtab[i].px);
        }
        wtab[i].obj = 0;
        wtab[i].kind = KIND_NONE;
        wtab[i].owner = 0;
    }
    widget_sweep_dead();
    widget_clean_owner(tid);
}

/* ---- ultimo evento por tarea ------------------------------------------ */

static void levent_set(int tid, int handle, int code, int dir)
{
    int i, free_ = -1;

    for (i = 0; i < LEVENT_MAX; i++)
    {
        if (levent[i].tid == tid)
        {
            levent[i].handle = handle;
            levent[i].code = code;
            levent[i].dir = dir;
            return;
        }
        if (levent[i].tid == 0 && free_ < 0)
        {
            free_ = i;
        }
    }
    if (free_ >= 0)
    {
        levent[free_].tid = tid;
        levent[free_].handle = handle;
        levent[free_].code = code;
        levent[free_].dir = dir;
    }
}

static int levent_get(int tid)
{
    int i;

    for (i = 0; i < LEVENT_MAX; i++)
    {
        if (levent[i].tid == tid)
        {
            return levent[i].handle;
        }
    }
    return 0;
}

static int levent_field(int tid, int field)
{
    int i;

    for (i = 0; i < LEVENT_MAX; i++)
    {
        if (levent[i].tid == tid)
        {
            return field == 0 ? levent[i].code : levent[i].dir;
        }
    }
    return 0;
}

/* ---- cola de eventos -------------------------------------------------- */

static void event_push(lv_obj_t *obj, int code, int dir)
{
    lv_obj_t *o = obj;
    int handle = 0;
    int owner = 0;
    int i, depth;

    /* El indev entrega el evento en el objeto que pico el dedo, que puede ser
     * un hijo interno del widget que la app cree (el caption de lv_btn, por
     * ejemplo).  Los hijos internos no estan en la tabla, asi que se sube al
     * ancestro registrado mas cercano: ese es el handle y el dueno que la app
     * espera, y sin esto el evento cae con handle 0 y el filtro por dueno lo
     * tira. */
    for (depth = 0; depth < 8 && o != 0 && handle == 0; depth++)
    {
        for (i = 0; i < WIDGET_MAX; i++)
        {
            if (wtab[i].obj == o)
            {
                handle = i + 1;
                owner = wtab[i].owner;
                break;
            }
        }
        if (handle == 0)
        {
            o = lv_obj_get_parent(o);
        }
    }
    if (eq_count < EVENT_QUEUE)
    {
        struct event_slot *s = &eq[(eq_head + eq_count) % EVENT_QUEUE];

        s->handle = handle;
        s->code = code;
        s->dir = dir;
        s->owner = owner;
        eq_count++;
    }
    /* Lleno: se descarta el evento nuevo antes que el viejo; el dedo va mas
     * rapido que el bucle de la app y lo que importa es el ultimo estado. */
}

/* Quita la posicion `i` (relativa a eq_head) desplazando el resto. */
static void event_drop(int i)
{
    int j;

    for (j = i; j < eq_count - 1; j++)
    {
        eq[(eq_head + j) % EVENT_QUEUE] = eq[(eq_head + j + 1) % EVENT_QUEUE];
    }
    eq_count--;
}

/* Saca el primer evento de `tid` conservando el orden relativo de los demas:
 * la cola es unica, pero cada tarea solo ve los widgets que cree.  Devuelve el
 * handle y, por `code` y `dir`, el codigo y la direccion de gesto asociados. */
static int event_pop_owner(int tid, int *code, int *dir)
{
    int i, k, found = -1;

    for (i = 0; i < eq_count; i++)
    {
        k = (eq_head + i) % EVENT_QUEUE;
        if (eq[k].owner == tid && eq[k].handle != 0)
        {
            found = i;
            break;
        }
    }
    /* Los eventos de un widget ya borrado se descartan siempre, haya o no uno
     * valido: si no, quedarian en la cola comiendo huecos. */
    for (i = eq_count - 1; i >= 0; i--)
    {
        k = (eq_head + i) % EVENT_QUEUE;
        if (eq[k].owner == tid && eq[k].handle == 0)
        {
            event_drop(i);
            if (found > i)
            {
                found--;
            }
            if (found == i)
            {
                found = -1;
            }
        }
    }
    if (found < 0)
    {
        return 0;
    }
    k = (eq_head + found) % EVENT_QUEUE;
    *code = eq[k].code;
    *dir = eq[k].dir;
    {
        int handle = eq[k].handle;

        event_drop(found);
        return handle;
    }
}

/* Callback comun de los widgets tocables.  Los gestos llevan ademas la
 * direccion que midio el indev, para que la app distinga un swipe de un toque. */
static void widget_event_cb(lv_event_t *e)
{
    lv_obj_t *target = (lv_obj_t *) lv_event_get_target(e);
    int code = (int) lv_event_get_code(e);
    int dir = 0;

    if (code == LV_EVENT_GESTURE)
    {
        lv_indev_t *indev = lv_indev_get_act();

        if (indev != 0)
        {
            dir = (int) lv_indev_get_gesture_dir(indev);
        }
    }
    event_push(target, code, dir);
}

/* Solo los botones y las pantallas se marcan como clicables.  LVGL entrega el
 * toque al objeto clicable mas de arriba en el punto, y la rejilla de la
 * galeria vive de lo contrario de lo que uno esperaria: la miniatura va
 * ENCIMA del boton de la celda, y solo pasa algo si la imagen NO es clicable y
 * el dedo cae en el boton de debajo.  Lo mismo con el caption interno de
 * lv_btn.  Por eso label, rect e img no llevan el flag: decoracion que deja
 * pasar el dedo.
 *
 * El callback se registra igualmente en los que son clicables: un swipe que
 * empieza encima de un boton lo recibe ese boton, y uno que empieza en zona
 * vacia lo recibe la pantalla (con SCROLLABLE limpiada, ver screen()). */
static void widget_enable_touch(lv_obj_t *o)
{
    if (o == 0)
    {
        return;
    }
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(o, widget_event_cb, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(o, widget_event_cb, LV_EVENT_GESTURE, 0);
}

/* ---- color ------------------------------------------------------------ */

/* 0xRRGGBB del SDK a lv_color_t; lv_color_hex hace la conversion a RGB565
 * (LV_COLOR_DEPTH 16), que es lo que el panel y el blit esperan. */
static lv_color_t to_color(int rgb)
{
    return lv_color_hex((uint32_t) rgb & 0xFFFFFFU);
}

/* ---- texto formateado, sin printf del kernel -------------------------- */

/*
 * setTextFmt() recibe el formato de un .class que esta en la SD, y un %s en
 * ese formato haria que el kernel siguiera un puntero que no existe.  No se le
 * pasa el string a lv_snprintf: se interpretan a mano %d, %x y %%, y cualquier
 * otro especificador se copia literal.  Dos enteros de argumentos, dos
 * conversiones como maximo.
 */
static void fmt_int(char *out, int max, int *pos, long value, int hex)
{
    char tmp[24];
    int n = 0;
    unsigned long v;
    int negative = 0;

    if (!hex && value < 0)
    {
        negative = 1;
        v = (unsigned long) (-value);
    }
    else
    {
        v = (unsigned long) value;
    }

    do
    {
        unsigned long d = v % (hex ? 16UL : 10UL);

        tmp[n++] = (char) (d < 10UL ? '0' + d : 'a' + d - 10UL);
        v /= (hex ? 16UL : 10UL);
    } while (v && n < (int) sizeof(tmp));

    if (negative)
    {
        tmp[n++] = '-';
    }
    while (n > 0 && *pos < max - 1)
    {
        out[(*pos)++] = tmp[--n];
    }
}

static void fmt_take(char c, char *out, int max, int *pos)
{
    if (*pos < max - 1)
    {
        out[*pos] = c;
        (*pos)++;
    }
}

static void format_two(const char *fmt, int a, int b, char *out, int max)
{
    int pos = 0;
    int arg = 0;
    const char *s;

    out[0] = 0;
    for (s = fmt; *s; s++)
    {
        if (*s != '%')
        {
            fmt_take(*s, out, max, &pos);
            continue;
        }
        s++;
        switch (*s)
        {
        case 'd':
        case 'i':
            fmt_int(out, max, &pos, arg == 0 ? (long) a : (long) b, 0);
            arg++;
            break;
        case 'x':
            fmt_int(out, max, &pos, arg == 0 ? (long) a : (long) b, 1);
            arg++;
            break;
        case '%':
            fmt_take('%', out, max, &pos);
            break;
        case 0:
            out[pos] = 0;
            return;
        default:
            /* %f, %s, lo que sea: se copia tal cual, sin tocar argumentos. */
            fmt_take('%', out, max, &pos);
            fmt_take(*s, out, max, &pos);
            break;
        }
    }
    out[pos] = 0;
}

/* ---- natives ---------------------------------------------------------- */

/* init(): LVGL ya arranco en el devtab; aqui la tarea tira SU interfaz y
 * olvida sus handles de la vida anterior.  No se toca la de las demas: con el
 * launcher y una app en segundo plano, un init() global borraria la UI ajena. */
int esp32p4_lvgl_init(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    (void) stack;
    (void) p;
    (void) type;

    if (!ui_ready())
    {
        printf("[lvgl] LVGL0 no inicializado\r\n");
        return 0;
    }
    lvgl_lock();
    widget_delete_owner((int) gettid());
    lvgl_unlock();
    return 0;
}

/* screen(): pantalla nueva, cargada y con handle. */
int esp32p4_lvgl_screen(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    lv_obj_t *scr;
    int h;
    int anterior;

    (void) p;
    (void) type;

    if (!ui_ready())
    {
        pushInt(stack, 0);
        return 0;
    }
    lvgl_lock();
    anterior = screen_handle_de(lv_scr_act());
    scr = lv_obj_create(NULL);
    if (scr != 0)
    {
        lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
        widget_enable_touch(scr);
    }
    /* Se carga solo si el slot quedo registrado: widget_add(), al no haber
     * hueco, borra el objeto que le dan, y borrar el screen ya activo dejaria
     * al display sin pantalla (ver screen_apartar). */
    h = widget_add(scr, KIND_SCREEN);
    if (h != 0)
    {
        screen_prev = anterior;         /* a quien le quitamos el panel */
        lv_scr_load(scr);
    }
    lvgl_unlock();
    pushInt(stack, h);
    return 0;
}

/* activeScreen() -> handle del screen cargado, 0 si no esta registrado.
 * Lo usa el launcher para poner su banner sobre la pantalla de la app en
 * frente sin conocer el handle que esa app se guardo. */
int esp32p4_lvgl_activeScreen(StackFrame *stack, SimpleConstantPool *p,
                              char *type)
{
    int i;
    lv_obj_t *act;

    (void) p;
    (void) type;

    if (!ui_ready())
    {
        pushInt(stack, 0);
        return 0;
    }
    lvgl_lock();
    act = lv_scr_act();
    for (i = 0; i < WIDGET_MAX; i++)
    {
        if (wtab[i].obj == act && wtab[i].kind == KIND_SCREEN)
        {
            lvgl_unlock();
            pushInt(stack, i + 1);
            return 0;
        }
    }
    lvgl_unlock();
    pushInt(stack, 0);
    return 0;
}

/* loadScreen(h): cargar un screen ya registrado (el propio o el de una app en
 * segundo plano al reanudarse).  Solo pantallas: un handle de widget no es
 * una pantalla y no debe colarse. */
int esp32p4_lvgl_loadScreen(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int h = popInt(stack);
    struct widget_slot *s;

    (void) p;
    (void) type;

    if (!ui_ready())
    {
        return 0;
    }
    lvgl_lock();
    s = widget_slot_screen(h);
    if (s != 0)
    {
        lv_scr_load(s->obj);
    }
    lvgl_unlock();
    return 0;
}

/* clear(color): tira los objetos de la tarea que llama y pinta de color su
 * pantalla (el contrato de LVGL.java).  Con el banner del launcher colgando
 * del mismo screen, ese banner es propiedad de otra tarea y sobrevive; los
 * demas screens (apps en segundo plano) no se tocan. */
int esp32p4_lvgl_clear(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int color = popInt(stack);
    int tid = (int) gettid();
    lv_obj_t *scr;

    (void) p;
    (void) type;

    if (!ui_ready())
    {
        return 0;
    }
    lvgl_lock();
    widget_delete_owner(tid);
    scr = my_screen();
    if (scr == 0)
    {
        scr = lv_scr_act();
    }
    lv_obj_set_style_bg_color(scr, to_color(color), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lvgl_unlock();
    return 0;
}

/* label(parent, text, x, y) -> handle */
int esp32p4_lvgl_label(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char text[128];
    lv_obj_t *o;
    int parent, x, y, h;

    (void) type;
    y = popInt(stack);
    x = popInt(stack);
    pop_text(stack, p, text, sizeof(text));
    parent = popInt(stack);

    if (!ui_ready())
    {
        pushInt(stack, 0);
        return 0;
    }
    lvgl_lock();
    o = lv_label_create(widget_parent(parent));
    if (o != 0)
    {
        lv_label_set_text(o, text);
        lv_obj_set_pos(o, x, y);
        lv_obj_set_style_text_color(o, lv_color_white(), 0);
    }
    h = widget_add(o, KIND_LABEL);
    lvgl_unlock();
    pushInt(stack, h);
    return 0;
}

/* button(parent, text, x, y, w, h) -> handle (con su label hijo y evento) */
int esp32p4_lvgl_button(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char text[64];
    lv_obj_t *btn, *cap;
    int parent, x, y, ww, hh, h;

    (void) type;
    hh = popInt(stack);
    ww = popInt(stack);
    y = popInt(stack);
    x = popInt(stack);
    pop_text(stack, p, text, sizeof(text));
    parent = popInt(stack);

    if (!ui_ready())
    {
        pushInt(stack, 0);
        return 0;
    }
    lvgl_lock();
    btn = lv_btn_create(widget_parent(parent));
    if (btn != 0)
    {
        lv_obj_set_pos(btn, x, y);
        lv_obj_set_size(btn, ww, hh);
        cap = lv_label_create(btn);
        if (cap != 0)
        {
            lv_label_set_text(cap, text);
            lv_obj_center(cap);
        }
        widget_enable_touch(btn);
    }
    h = widget_add(btn, KIND_BUTTON);
    lvgl_unlock();
    pushInt(stack, h);
    return 0;
}

/* rect(parent, x, y, w, h, color) -> handle: caja de color sin decoracion. */
int esp32p4_lvgl_rect(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    lv_obj_t *o;
    int parent, x, y, ww, hh, color, h;

    (void) p;
    (void) type;
    color = popInt(stack);
    hh = popInt(stack);
    ww = popInt(stack);
    y = popInt(stack);
    x = popInt(stack);
    parent = popInt(stack);

    if (!ui_ready())
    {
        pushInt(stack, 0);
        return 0;
    }
    lvgl_lock();
    o = lv_obj_create(widget_parent(parent));
    if (o != 0)
    {
        lv_obj_set_pos(o, x, y);
        lv_obj_set_size(o, ww, hh);
        lv_obj_set_style_bg_color(o, to_color(color), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(o, 0, 0);
        lv_obj_set_style_radius(o, 0, 0);
        lv_obj_set_style_pad_all(o, 0, 0);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    }
    h = widget_add(o, KIND_RECT);
    lvgl_unlock();
    pushInt(stack, h);
    return 0;
}

/* setText(w, text): en un boton se escribe en su label hijo. */
int esp32p4_lvgl_setText(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char text[256];
    struct widget_slot *s;
    int h;

    (void) p;
    (void) type;
    pop_text(stack, p, text, sizeof(text));
    h = popInt(stack);

    if (!ui_ready())
    {
        return 0;
    }
    lvgl_lock();
    s = widget_slot_mine(h);
    if (s != 0)
    {
        if (s->kind == KIND_BUTTON)
        {
            lv_obj_t *cap = lv_obj_get_child(s->obj, 0);

            if (cap != 0)
            {
                lv_label_set_text(cap, text);
            }
        }
        else if (s->kind == KIND_LABEL)
        {
            lv_label_set_text(s->obj, text);
        }
    }
    lvgl_unlock();
    return 0;
}

/* setTextFmt(w, fmt, a, b) */
int esp32p4_lvgl_setTextFmt(StackFrame *stack, SimpleConstantPool *p,
                            char *type)
{
    char fmt[64], text[160];
    struct widget_slot *s;
    int a, b, h;

    (void) type;
    b = popInt(stack);
    a = popInt(stack);
    pop_text(stack, p, fmt, sizeof(fmt));
    h = popInt(stack);

    if (!ui_ready())
    {
        return 0;
    }
    format_two(fmt, a, b, text, sizeof(text));

    lvgl_lock();
    s = widget_slot_mine(h);
    if (s != 0)
    {
        if (s->kind == KIND_BUTTON)
        {
            lv_obj_t *cap = lv_obj_get_child(s->obj, 0);

            if (cap != 0)
            {
                lv_label_set_text(cap, text);
            }
        }
        else if (s->kind == KIND_LABEL)
        {
            lv_label_set_text(s->obj, text);
        }
    }
    lvgl_unlock();
    return 0;
}

/* setPos(w, x, y) */
int esp32p4_lvgl_setPos(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int x, y, h;
    lv_obj_t *o;

    (void) p;
    (void) type;
    y = popInt(stack);
    x = popInt(stack);
    h = popInt(stack);

    if (!ui_ready())
    {
        return 0;
    }
    lvgl_lock();
    o = widget_obj_mine(h);
    if (o != 0)
    {
        lv_obj_set_pos(o, x, y);
    }
    lvgl_unlock();
    return 0;
}

/* setSize(w, width, height) */
int esp32p4_lvgl_setSize(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int ww, hh, h;
    lv_obj_t *o;

    (void) p;
    (void) type;
    hh = popInt(stack);
    ww = popInt(stack);
    h = popInt(stack);

    if (!ui_ready())
    {
        return 0;
    }
    lvgl_lock();
    o = widget_obj_mine(h);
    if (o != 0)
    {
        lv_obj_set_size(o, ww, hh);
    }
    lvgl_unlock();
    return 0;
}

/* setColor(w, fg, bg) */
int esp32p4_lvgl_setColor(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int fg, bg, h;
    struct widget_slot *s;

    (void) p;
    (void) type;
    bg = popInt(stack);
    fg = popInt(stack);
    h = popInt(stack);

    if (!ui_ready())
    {
        return 0;
    }
    lvgl_lock();
    s = widget_slot_mine(h);
    if (s != 0)
    {
        lv_obj_set_style_text_color(s->obj, to_color(fg), 0);
        if (s->kind != KIND_LABEL)
        {
            /* En un label el fondo pintado taparia el propio texto si la caja
             * no tiene tamano; solo se rellena en botones, cajas e imagenes. */
            lv_obj_set_style_bg_color(s->obj, to_color(bg), 0);
            lv_obj_set_style_bg_opa(s->obj, LV_OPA_COVER, 0);
        }
    }
    lvgl_unlock();
    return 0;
}

/* setRadius(w, r): radio de las esquinas.  El tema de LVGL trae 5 px, que en
 * una tarjeta de 100x84 se ve cuadrada; las tarjetas van a 14 y las pastillas
 * a 11, que es lo que da el aspecto de píldora. */
int esp32p4_lvgl_setRadius(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int r = popInt(stack);
    int h = popInt(stack);
    struct widget_slot *s;

    (void) p;
    (void) type;
    if (!ui_ready())
    {
        return 0;
    }
    lvgl_lock();
    s = widget_slot_mine(h);
    if (s != 0)
    {
        lv_obj_set_style_radius(s->obj, r, 0);
    }
    lvgl_unlock();
    return 0;
}

/* setFlat(w): quita el degradado, el borde y la sombra que el tema pone en
 * botones y cajas; sin eso todo sale con relieve de 2014. */
int esp32p4_lvgl_setFlat(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int h = popInt(stack);
    struct widget_slot *s;

    (void) p;
    (void) type;
    if (!ui_ready())
    {
        return 0;
    }
    lvgl_lock();
    s = widget_slot_mine(h);
    if (s != 0)
    {
        lv_obj_set_style_bg_grad_color(s->obj, lv_obj_get_style_bg_color(s->obj, 0), 0);
        lv_obj_set_style_bg_grad_dir(s->obj, LV_GRAD_DIR_NONE, 0);
        lv_obj_set_style_border_width(s->obj, 0, 0);
        lv_obj_set_style_shadow_width(s->obj, 0, 0);
        lv_obj_set_style_outline_width(s->obj, 0, 0);
    }
    lvgl_unlock();
    return 0;
}

/* setPad(w, p): relleno interior, para que el texto no toque el borde. */
int esp32p4_lvgl_setPad(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int pad = popInt(stack);
    int h = popInt(stack);
    struct widget_slot *s;

    (void) p;
    (void) type;
    if (!ui_ready())
    {
        return 0;
    }
    lvgl_lock();
    s = widget_slot_mine(h);
    if (s != 0)
    {
        lv_obj_set_style_pad_all(s->obj, pad, 0);
    }
    lvgl_unlock();
    return 0;
}

/* setFont(w, px): cuerpo del texto.  Solo se compilan Montserrat 14 (la
 * por defecto) y 26 (los titulos), ver lvgl/lv_conf.h: cada cuerpo cuesta
 * decenas de KB de .rodata, y otro tamano se anade ahi, no aqui.  Un px que no
 * sea 14 ni 26 no toca nada, para que una app mal escrita no deje el widget sin
 * fuente. */
int esp32p4_lvgl_setFont(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int px = popInt(stack);
    int h = popInt(stack);
    struct widget_slot *s;

    (void) p;
    (void) type;
    if (!ui_ready())
    {
        return 0;
    }
    lvgl_lock();
    s = widget_slot_mine(h);
    if (s != 0)
    {
        if (px == 14)
        {
            lv_obj_set_style_text_font(s->obj, &lv_font_montserrat_14, 0);
        }
        else if (px == 26)
        {
            lv_obj_set_style_text_font(s->obj, &lv_font_montserrat_26, 0);
        }
    }
    lvgl_unlock();
    return 0;
}

/* tick(): reloj de LVGL + refresco.  El device lleva el tick y el timer
 * handler bajo su monitor; se usan dos control() porque el monitor no es
 * reentrante y no se puede abrir un unico criticos desde aqui. */
int esp32p4_lvgl_tick(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    (void) stack;
    (void) p;
    (void) type;
    if (!ui_ready())
    {
        return 0;
    }
    control(LVGL0, LVGL_CTRL_TICK, 0, 0);
    control(LVGL0, LVGL_CTRL_TASK, 0, 0);
    return 0;
}

/* LVGL.idleOff(ms): politica de tiempo de pantalla.  Con ms > 0 el kernel
 * apaga el backlight cuando pasan ms sin que nadie toque el panel, y el primer
 * dedo lo vuelve a encender --ese dedo se traga y no llega a ninguna app, para
 * que despertar no sea tambien pulsar el boton que hubiera debajo.  ms = 0 deja
 * la pantalla encendida para siempre.
 *
 * El contador NO lo lleva el launcher: lleva los toques la hebra lvglui, que es
 * la unica que ve los de todas las apps y pantallas (device/lvgl/lvglport.c). */
int esp32p4_lvgl_idleOff(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int ms = popInt(stack);

    (void) p;
    (void) type;
    if (!ui_ready())
    {
        return 0;
    }
    control(LVGL0, LVGL_CTRL_IDLE, (long) ms, 0);
    return 0;
}

/* pollEvent() -> 1 si habia un evento de ESTA tarea y se consume. */
int esp32p4_lvgl_pollEvent(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int tid = (int) gettid();
    int code = 0, dir = 0;
    int handle;

    (void) p;
    (void) type;
    if (!ui_ready())
    {
        pushInt(stack, 0);
        return 0;
    }
    /* La cola la escribe el callback de LVGL, que corre dentro de
     * lv_timer_handler() bajo el monitor del device. */
    lvgl_lock();
    handle = event_pop_owner(tid, &code, &dir);
    if (handle != 0)
    {
        levent_set(tid, handle, code, dir);
    }
    lvgl_unlock();
    pushInt(stack, handle != 0);
    return 0;
}

/* eventWidget() -> handle del ultimo evento consumido por esta tarea. */
int esp32p4_lvgl_eventWidget(StackFrame *stack, SimpleConstantPool *p,
                             char *type)
{
    (void) p;
    (void) type;
    pushInt(stack, levent_get((int) gettid()));
    return 0;
}

/* eventCode() -> codigo LVGL del ultimo evento consumido (LV_EVENT_CLICKED,
 * LV_EVENT_GESTURE...).  Sin evento pendiente devuelve 0. */
int esp32p4_lvgl_eventCode(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    (void) p;
    (void) type;
    pushInt(stack, levent_field((int) gettid(), 0));
    return 0;
}

/* eventGesture() -> LV_DIR_* del ultimo gesto consumido (NONE/LEFT/RIGHT/
 * TOP/BOTTOM).  0 si el ultimo evento no fue un gesto. */
int esp32p4_lvgl_eventGesture(StackFrame *stack, SimpleConstantPool *p,
                              char *type)
{
    (void) p;
    (void) type;
    pushInt(stack, levent_field((int) gettid(), 1));
    return 0;
}

/* del(w): LVGL borra el objeto y sus hijos, y aqui se devuelve la reserva de
 * pixeles de una imagen y se olvida el handle.  Solo widgets propios; los
 * hijos que caigan con el padre se recogen con el barrido. */
int esp32p4_lvgl_del(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int h = popInt(stack);
    struct widget_slot *s;

    (void) p;
    (void) type;
    if (!ui_ready())
    {
        return 0;
    }
    lvgl_lock();
    s = widget_slot_mine(h);
    if (s != 0)
    {
        lv_obj_t *o = s->obj;
        uint16_t *px = s->px;
        char *src = s->src;

        if (s->kind == KIND_SCREEN && o == lv_scr_act())
        {
            screen_apartar((int) gettid());
        }
        widget_forget(h);
        lv_obj_del(o);
        if (px != 0)
        {
            psram_free(px);
        }
        if (src != 0)
        {
            psram_free(src);
        }
        widget_sweep_dead();
    }
    lvgl_unlock();
    return 0;
}

/* img(parent, path, x, y, w, h): un fichero de la SD como imagen.

 * Dos rutas, segun la extension:
 *
 *   - ".raw" (RGB565 crudo, w*h*2 B): se lee entero a PSRAM y el lv_img_dsc_t
 *     del slot apunta a esos pixeles.  LVGL no copia nada y no hay decodificador
 *     de por medio, asi que es la via barata para iconos y mascaras.
 *   - ".jpg"/".jpeg": no se toca el fichero aqui.  Se pasa a LVGL la ruta con
 *     la letra del backend ('S', device/lvgl/lvglfs.c) y sjpg --TJpgDec
 *     vendorizado en lvgl/src/extra/libs/sjpg-- decodifica por si solo.  w y h
 *     se ignoran: el tamano lo da la cabecera del JPEG.
 *
 * Lo que si es identico en los dos casos es la vida del buffer: LVGL guarda el
 * PUNTERO de la src (no hace copia) y lo vuelve a usar en cada refresco, asi
 * que px y src viven en el slot hasta que el objeto muera (del(), clear(),
 * init()).  Por eso img() nunca libera nada por su cuenta despues de
 * widget_add().
 *
 * Ojo con la talla: sjpg no decodifica por franjas para un JPEG normal, sino
 * que reserva la imagen entera en el heap de LVGL (x*y*3 bytes en PSRAM, ver
 * lv_sjpg.c decoder_open).  Un 960x1280 son 3,5 MB y un render completo por
 * SPI2; las imagenes de la galeria se pre-reducen a la resolucion del panel. */
static int path_is_jpeg(const char *path)
{
    static const char *sfx[] = {".jpg", ".jpeg"};
    size_t n = strlen(path);
    unsigned i;

    for (i = 0; i < sizeof(sfx) / sizeof(sfx[0]); i++)
    {
        size_t m = strlen(sfx[i]);
        const char *tail;
        size_t k;

        if (n < m)
        {
            continue;
        }
        tail = path + n - m;
        for (k = 0; k < m; k++)
        {
            char a = tail[k];

            if (a >= 'A' && a <= 'Z')
            {
                a += (char)('a' - 'A');
            }
            if (a != sfx[i][k])
            {
                break;
            }
        }
        if (k == m)
        {
            return 1;
        }
    }
    return 0;
}

/* JPEG: devuelve la ruta con letra 'S' ya reservada en PSRAM y puesta como src
 * del objeto, o 0 si no hay memoria.  La reserva es nuestra porque
 * lv_img_set_src() guarda el puntero y el decodificador lo vuelve a leer en
 * cada refresco. */
static char *img_set_file(lv_obj_t *o, const char *path)
{
    char full[132];
    char *src;
    size_t len;

    if (strlen(path) + 3 > sizeof(full))
    {
        return 0;
    }
    full[0] = 'S';
    full[1] = ':';
    strcpy(full + 2, path);

    len = strlen(full) + 1;
    src = (char *) psram_malloc(len);
    if (src == 0)
    {
        printf("[lvgl] img: sin PSRAM para la ruta\r\n");
        return 0;
    }
    memcpy(src, full, len);

    lv_img_set_src(o, src);
    return src;
}

int esp32p4_lvgl_img(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char path[128];
    lv_obj_t *o;
    struct widget_slot *s;
    uint16_t *px;
    uint32_t bytes;
    int parent, x, y, ww, hh, h = 0;
    int jpeg;
    void *f;
    size_t got;

    (void) type;
    hh = popInt(stack);
    ww = popInt(stack);
    y = popInt(stack);
    x = popInt(stack);
    pop_text(stack, p, path, sizeof(path));
    parent = popInt(stack);

    if (!ui_ready())
    {
        pushInt(stack, 0);
        return 0;
    }

    jpeg = path_is_jpeg(path);

    /* Un crudo sin tamano no se puede ni leer; el JPEG se autosita. */
    if (!jpeg && (ww <= 0 || hh <= 0))
    {
        pushInt(stack, 0);
        return 0;
    }

    /* Con una ruta, LVGL avisa de un fichero que no existe pintando nada: la
     * app se quedaria con un handle valido y sin imagen.  Se abre y cierra
     * aqui solo para devolver 0 (fuera del monitor de LVGL: es la comprobacion,
     * no toca la libreria). */
    if (jpeg)
    {
        f = fl_fopen(path, "r");
        if (f == 0)
        {
            printf("[lvgl] img: no se abre %s\r\n", path);
            pushInt(stack, 0);
            return 0;
        }
        fl_fclose(f);
    }

    lvgl_lock();

    if (jpeg)
    {
        o = lv_img_create(widget_parent(parent));
        h = widget_add(o, KIND_IMG);
        if (h == 0)
        {
            lvgl_unlock();
            pushInt(stack, 0);
            return 0;
        }
        s = widget_slot(h);
        s->src = img_set_file(o, path);
        if (s->src == 0)
        {
            /* Sin ruta no hay imagen que refrescar: se tira el objeto y se
             * devuelve el slot, para que la app vea el mismo 0 que veria con
             * un fichero inexistente. */
            widget_forget(h);
            lv_obj_del(o);
            h = 0;
        }
        else
        {
            lv_obj_set_pos(o, x, y);
        }
        lvgl_unlock();
        pushInt(stack, h);
        return 0;
    }

    bytes = (uint32_t) ww * (uint32_t) hh * 2U;

    f = fl_fopen(path, "r");
    if (f == 0)
    {
        lvgl_unlock();
        printf("[lvgl] img: no se abre %s\r\n", path);
        pushInt(stack, 0);
        return 0;
    }

    px = (uint16_t *) psram_malloc(bytes);
    if (px == 0)
    {
        lvgl_unlock();
        fl_fclose(f);
        printf("[lvgl] img: sin PSRAM para %u B\r\n", (unsigned) bytes);
        pushInt(stack, 0);
        return 0;
    }

    got = fl_fread(px, 1, bytes, f);
    fl_fclose(f);
    if (got != (size_t) bytes)
    {
        lvgl_unlock();
        psram_free(px);
        printf("[lvgl] img: %s da %u de %u B\r\n", path, (unsigned) got,
               (unsigned) bytes);
        pushInt(stack, 0);
        return 0;
    }

    o = lv_img_create(widget_parent(parent));
    h = widget_add(o, KIND_IMG);
    if (h != 0)
    {
        /* widget_add() tiro el objeto si no quedaban slots; en ese caso h==0
         * y lo que queda liberar la imagen leida. */
        s = widget_slot(h);
        s->px = px;
        s->dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        s->dsc.header.always_zero = 0;
        s->dsc.header.reserved = 0;
        s->dsc.header.w = (lv_coord_t) ww;
        s->dsc.header.h = (lv_coord_t) hh;
        s->dsc.data_size = bytes;
        s->dsc.data = (const uint8_t *) px;
        lv_img_set_src(o, &s->dsc);
        lv_obj_set_pos(o, x, y);
    }
    else
    {
        psram_free(px);
    }
    lvgl_unlock();
    pushInt(stack, h);
    return 0;
}

/* getWidth(w) */
int esp32p4_lvgl_getWidth(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int h = popInt(stack), v = 0;
    lv_obj_t *o;

    (void) p;
    (void) type;
    if (ui_ready())
    {
        lvgl_lock();
        o = widget_obj(h);
        if (o != 0)
        {
            v = (int) lv_obj_get_width(o);
        }
        lvgl_unlock();
    }
    pushInt(stack, v);
    return 0;
}

/* getHeight(w) */
int esp32p4_lvgl_getHeight(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int h = popInt(stack), v = 0;
    lv_obj_t *o;

    (void) p;
    (void) type;
    if (ui_ready())
    {
        lvgl_lock();
        o = widget_obj(h);
        if (o != 0)
        {
            v = (int) lv_obj_get_height(o);
        }
        lvgl_unlock();
    }
    pushInt(stack, v);
    return 0;
}

/* screenWidth() / screenHeight(): lo que el driver de pantalla registro.  Sin
 * display registrado (el device no arranco) lv_disp_get_*_res devuelve el
 * LV_*_RES_MAX de lv_conf.h, que no es la geometria del panel: se contesta
 * directamente con la del pin map. */
int esp32p4_lvgl_screenWidth(StackFrame *stack, SimpleConstantPool *p,
                             char *type)
{
    (void) p;
    (void) type;
    pushInt(stack, ui_ready() ? (int) lv_disp_get_hor_res(NULL) : LCD_W);
    return 0;
}

int esp32p4_lvgl_screenHeight(StackFrame *stack, SimpleConstantPool *p,
                              char *type)
{
    (void) p;
    (void) type;
    pushInt(stack, ui_ready() ? (int) lv_disp_get_ver_res(NULL) : LCD_H);
    return 0;
}

/* ---- limpieza por tarea ----------------------------------------------- */

/**
 * @ingroup jvm
 *
 * El kernel llama aqui cuando una tarea java se va: bien porque su main()
 * retorno, bien porque el launcher la cerro.  Sin esto quedarian objetos
 * huerfanos en el arbol de LVGL --pintados y sordos--, reservas de PSRAM
 * perdidas y handles de una vida anterior repartidos a la siguiente tarea que
 * ocupe el tid.
 *
 * Se hace bajo el monitor del device y desde un punto seguro: quien llama no
 * puede estar dentro de la API de LVGL, porque la tarea que se va ya no
 * ejecuta.  Los screens se borran al final y con ellos cae todo lo que cuelga,
 * incluido el banner del launcher si estaba pendiente de la pantalla de la app;
 * por eso el barrido posterior, que despierta los slots ajeno que quedaron
 * apuntando a la nada y devuelve su memoria.
 */
void lvgl_forget_task(int tid)
{
    int i;

    if (!ui_ready() || tid == 0)
    {
        return;
    }
    lvgl_lock();
    screen_apartar(tid);
    widget_delete_owner(tid);
    for (i = 0; i < WIDGET_MAX; i++)
    {
        if (wtab[i].obj != 0 && wtab[i].owner == tid &&
            wtab[i].kind == KIND_SCREEN)
        {
            lv_obj_del(wtab[i].obj);
        }
    }
    widget_sweep_dead();
    widget_clean_owner(tid);
    for (i = 0; i < WIDGET_MAX; i++)
    {
        if (wtab[i].obj != 0 && wtab[i].owner == tid)
        {
            wtab[i].obj = 0;
            wtab[i].kind = KIND_NONE;
            wtab[i].owner = 0;
        }
    }
    lvgl_unlock();
}
