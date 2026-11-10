/**
 * @file calculadora.c
 *
 * Calculadora para el pad LCD del P4.
 *
 *   elf /elf/calculadora.elf
 *
 * La aritmética es en FLOAT de precisión simple (rv32imafc tiene FPU),
 * con la entrada tecleada guardada como texto absoluto y parseada al vuelo
 * (no hay strtod en el kernel).  Suma, resta, producto y division.  Division
 * por cero, NaN y desbordamiento (|x| > FLT_MAX, incluido infinito) muestran
 * "ERR".  El punto decimal se abre con la tecla '.'; el signo, con '+/-'.
 *
 * El resultado se formatea a mano: la libc de Xinu ('_doprnt') no entiende %f,
 * asi que se usan 'floorf'/'modff' (exportados por el kernel) y '%d' con
 * decimales recortados; a partir de 1e9 o por debajo de 1e-6 se usa notacion
 * cientifica (1.5e+11).
 *
 * Estado: todo en un bloque de PSRAM colgado del handle.  Nada de estaticas
 * escribibles: el cargador de ELF no las soporta.  El bucle de main saca
 * eventos con ui_poll() y solo el llama a exit(); los callbacks de LVGL
 * corren en lvglui y solo apilan.
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
#include "gestos.h"

#define CALC_DEBUG

void exit(int codigo);

/* floorf es la unica funcion matematica que hace falta y el kernel la exporta */
extern float floorf(float x);

/* FLT_MAX: el limite de float de 32 bits (3.4028235e38f) */
#define FLT_MAXF  3.4028235e38f

/* ---- ids de tecla -------------------------------------------------------- */

#define T_0     0
#define T_1     1
#define T_2     2
#define T_3     3
#define T_4     4
#define T_5     5
#define T_6     6
#define T_7     7
#define T_8     8
#define T_9     9
#define T_MAS   10
#define T_MENOS 11
#define T_POR   12
#define T_DIV   13
#define T_IGUAL 14
#define T_CLR   15
#define T_SIGNO 16
#define T_BORRA 17
#define T_DP    19
#define T_SALIR 20

#define OP_NINGUNA 0
#define OP_MAS     1
#define OP_MENOS   2
#define OP_POR     3
#define OP_DIV     4

#define C_TEXTO  0xE8D8B0
#define C_NUM    0xD7E3EE
#define C_OP     0xF0B429
#define C_ACC    0x5DADE2
#define C_CLR    0xE74C3C
#define C_PANT   0x101820

struct calc
{
    ui_input_t *g;
    lv_obj_t *pantalla;         /* rotulo donde se ve el numero               */
    lv_obj_t *expr;             /* linea chica con la operacion pendiente     */
    float acum;                 /* operando izquierdo / resultado             */
    int   op;                   /* OP_* pendiente                             */
    int   nuevo;                /* el proximo digito empieza un numero nuevo  */
    int   error;                /* 1 = mostrar ERR hasta limpiar              */
    int   neg;                  /* la entrada en curso es negativa            */
    char  entrada[24];          /* texto absoluto tal y como se teclea        */
    char  ebuf[48];             /* linea de expresion                         */
    int   slot;                 /* ranura en el gestor de apps (-1 = solo)     */
};

/* ---- formato de numeros (sin %f) ----------------------------------------- */

/* true si r es NaN, infinito o se paso de float. */
static int mal(float r)
{
    return !(r == r) || r > FLT_MAXF || r < -FLT_MAXF;
}

/* Formatea v como "-123.456" o "1.5e+11"; ERR si NaN/inf/desbordamiento.
 * Hasta 9 enteros caben en int; a partir de ahi notacion cientifica. */
static void fmt_float(char *out, size_t n, float v)
{
    char ibuf[24], fbuf[12];
    int neg, big, e = 0, ip, q;
    float a, frac;

    if (mal(v))
    {
        snprintf(out, n, "ERR");
        return;
    }

    neg = (v < 0.0f);
    a = neg ? -v : v;
    big = (a >= 1e9f) || (a != 0.0f && a < 1e-6f);

    if (big)
    {
        while (a >= 10.0f) { a /= 10.0f; e++; }
        while (a < 1.0f && a != 0.0f) { a *= 10.0f; e--; }
    }

    ip = (int)floorf(a);        /* a < 1e9  o  1..9 en cientifica   */
    frac = a - (float)ip;
    q = (int)(frac * 1000000.0f + 0.5f);
    if (q >= 1000000) { q = 0; ip += 1; if (ip >= 10 && big) { ip = 1; e++; } }
    if (ip >= 1000000000) { ip = 999999999; q = 0; }

    snprintf(ibuf, sizeof(ibuf), "%d", ip);
    snprintf(fbuf, sizeof(fbuf), "%06d", q);
    {
        size_t l = strlen(fbuf);
        while (l > 0 && '0' == fbuf[l - 1]) { fbuf[--l] = '\0'; }
    }

    if (big)
    {
        if ('\0' != fbuf[0]) snprintf(out, n, "%s%s.%se%+d", neg ? "-" : "", ibuf, fbuf, e);
        else                 snprintf(out, n, "%s%se%+d",   neg ? "-" : "", ibuf, e);
    }
    else
    {
        if ('\0' != fbuf[0]) snprintf(out, n, "%s%s.%s", neg ? "-" : "", ibuf, fbuf);
        else                 snprintf(out, n, "%s%s",   neg ? "-" : "", ibuf);
    }
}

/* Parsea el texto absoluto (solo digitos y un punto) aplicando el signo. */
static float parse_entrada(const struct calc *c)
{
    const char *s = c->entrada;
    float ip = 0.0f, fp = 0.0f, esc = 0.1f;
    int punto = 0;

    for (; '\0' != *s; s++)
    {
        if ('.' == *s)
        {
            punto = 1;
            continue;
        }
        if (punto)
        {
            fp += (float)(*s - '0') * esc;
            esc *= 0.1f;
        }
        else
        {
            ip = ip * 10.0f + (float)(*s - '0');
        }
    }
    {
        float r = ip + fp;
        return c->neg ? -r : r;
    }
}

/* Valor actual: la entrada en curso, o el acumulador si se acaba de pulsar
 * un operador / '='. */
static float valor_actual(const struct calc *c)
{
    if (c->nuevo)
    {
        return c->acum;
    }
    return parse_entrada(c);
}

/* ---- pantalla ------------------------------------------------------------ */

static void mostrar(struct calc *c)
{
    char buf[40];

    if (c->error)
    {
        lvgl_lock();
        lv_label_set_text(c->pantalla, "ERR");
        lv_label_set_text(c->expr, "");
        lvgl_unlock();
        return;
    }

    if (c->nuevo)
    {
        fmt_float(buf, sizeof(buf), c->acum);
    }
    else if ('\0' == c->entrada[0])
    {
        snprintf(buf, sizeof(buf), "%s0", c->neg ? "-" : "");
    }
    else
    {
        snprintf(buf, sizeof(buf), "%s%s", c->neg ? "-" : "", c->entrada);
    }

    lvgl_lock();
    lv_label_set_text(c->pantalla, buf);
    lv_label_set_text(c->expr, c->ebuf);
    lvgl_unlock();
}

static void poner_expr(struct calc *c, const char *sop)
{
    char num[24];

    fmt_float(num, sizeof(num), c->acum);
    snprintf(c->ebuf, sizeof(c->ebuf), "%s %s", num, sop);
}

static void limpiar_todo(struct calc *c)
{
    c->acum = 0.0f;
    c->op = OP_NINGUNA;
    c->nuevo = 1;
    c->error = 0;
    c->neg = 0;
    c->entrada[0] = '\0';
    c->ebuf[0] = '\0';
}

static float binop(float a, float b, int op)
{
    if (OP_MAS == op)   return a + b;
    if (OP_MENOS == op) return a - b;
    if (OP_POR == op)   return a * b;
    return a / b;
}

/* Aplica el operador pendiente: acum := acum OP valor. */
static void ejecutar_pendiente(struct calc *c, const char *sop)
{
    float v, r;

    v = valor_actual(c);

    if (OP_NINGUNA == c->op)
    {
        c->acum = v;
    }
    else if (OP_DIV == c->op && 0.0f == v)
    {
        c->error = 1;
        mostrar(c);
        return;
    }
    else
    {
        r = binop(c->acum, v, c->op);
        if (mal(r))
        {
            c->error = 1;
            mostrar(c);
            return;
        }
        c->acum = r;
    }

    (void)sop;
}

static void aplicar_operador(struct calc *c, int op, const char *sop)
{
    if (c->error) return;

    /* Si ya hay un operador pendiente, encadena: 2+3*4 -> (2+3)*4 solo si se
     * pulsa primero '+' y luego '*'; es una calculadora simple de izquierda a
     * derecha, sin precedencia. */
    ejecutar_pendiente(c, sop);
    if (c->error) return;

    c->op = op;
    c->nuevo = 1;
    c->neg = 0;
    c->entrada[0] = '\0';
    poner_expr(c, sop);
    mostrar(c);
}

static void tecla_igual(struct calc *c)
{
    if (c->error || OP_NINGUNA == c->op) return;

    ejecutar_pendiente(c, NULL);
    if (c->error) return;

    c->op = OP_NINGUNA;
    c->nuevo = 1;
    c->neg = 0;
    c->entrada[0] = '\0';
    c->ebuf[0] = '\0';
    mostrar(c);
}

/* ---- teclas -------------------------------------------------------------- */

static void tecla_digito(struct calc *c, int d)
{
    size_t l;

    if (c->error) return;
    if (c->nuevo)
    {
        c->entrada[0] = '\0';
        c->neg = 0;
        c->nuevo = 0;
    }

    l = strlen(c->entrada);
    if (l >= sizeof(c->entrada) - 2) return;

    /* 0 inicial solo: se sustituye por el digito si no hay punto todavia */
    if (1 == l && '0' == c->entrada[0] && NULL == strchr(c->entrada, '.'))
    {
        c->entrada[0] = (char)('0' + d);
        c->entrada[1] = '\0';
    }
    else
    {
        c->entrada[l] = (char)('0' + d);
        c->entrada[l + 1] = '\0';
    }
    mostrar(c);
}

static void tecla_decimal(struct calc *c)
{
    if (c->error) return;
    if (c->nuevo)
    {
        c->entrada[0] = '0';
        c->entrada[1] = '\0';
        c->nuevo = 0;
    }
    if (NULL != strchr(c->entrada, '.')) return;
    if (strlen(c->entrada) >= sizeof(c->entrada) - 2) return;

    {
        size_t l = strlen(c->entrada);

        c->entrada[l] = '.';
        c->entrada[l + 1] = '\0';
    }
    mostrar(c);
}

static void tecla_signo(struct calc *c)
{
    if (c->error) return;
    if (c->nuevo)
    {
        c->acum = -c->acum;
    }
    else
    {
        c->neg = !c->neg;
    }
    mostrar(c);
}

static void tecla_borra(struct calc *c)
{
    size_t l;

    if (c->error || c->nuevo) return;
    l = strlen(c->entrada);
    if (0 == l) return;

    c->entrada[l - 1] = '\0';
    /* Si borra todo, la entrada queda limpia y el signo se olvida */
    if ('\0' == c->entrada[0]) { c->neg = 0; }
    mostrar(c);
}

static void ir_fondo(struct calc *c, int *fuera);

static void procesar(struct calc *c, ui_event_t *ev, int *fuera)
{
#ifdef CALC_DEBUG
    /* Diagnostico temporal: cada tecla deja huella en la consola */
    if (UI_EVENT_BUTTON == ev->tipo) {
        printf("calc: clave id=%d\r\n", ev->id);
    } else if (UI_EVENT_SWIPE == ev->tipo) {
        printf("calc: swipe dir=%d borde=%d\r\n", ev->dir, ev->edge);
    } else if (UI_EVENT_TAP == ev->tipo) {
        printf("calc: tap en (%d,%d)\r\n", ev->x1, ev->y1);
    }
#endif

    if (UI_EVENT_SWIPE == ev->tipo)
    {
        if (UI_EDGE_BOTTOM == ev->edge && UI_DIR_UP == ev->dir)
        {
            ir_fondo(c, fuera);
        }
        return;
    }
    if (UI_EVENT_BUTTON != ev->tipo)
    {
        return;
    }

    switch (ev->id)
    {
    case T_0: case T_1: case T_2: case T_3: case T_4:
    case T_5: case T_6: case T_7: case T_8: case T_9:
        tecla_digito(c, ev->id); break;
    case T_MAS:   aplicar_operador(c, OP_MAS,   "+"); break;
    case T_MENOS: aplicar_operador(c, OP_MENOS, "-"); break;
    case T_POR:   aplicar_operador(c, OP_POR,   "x"); break;
    case T_DIV:   aplicar_operador(c, OP_DIV,   "/"); break;
    case T_IGUAL: tecla_igual(c); break;
    case T_CLR:   limpiar_todo(c); mostrar(c); break;
    case T_SIGNO: tecla_signo(c); break;
    case T_BORRA: tecla_borra(c); break;
    case T_DP:    tecla_decimal(c); break;
    case T_SALIR: *fuera = 1; break;
    default: break;
    }
}

/* ---- interfaz ------------------------------------------------------------ */

static void crear_ui(struct calc *c)
{
    int i;

    /* titulo */
    lvgl_lock();
    {
        lv_obj_t *tit = lv_label_create(ui_root(c->g));
        lv_obj_set_style_text_color(tit, lv_color_hex(C_TEXTO), 0);
        lv_obj_align(tit, LV_ALIGN_TOP_LEFT, 10, 6);
        lv_label_set_text(tit, "CALC");
    }
    /* fondo de la pantalla de numeros (se crea antes que los rotulos para que
     * los rotulos queden encima al dibujarse). */
    {
        lv_obj_t *box = lv_obj_create(ui_root(c->g));
        lv_obj_set_size(box, 304, 64);
        lv_obj_align(box, LV_ALIGN_TOP_LEFT, 8, 38);
        lv_obj_set_style_bg_color(box, lv_color_hex(C_PANT), 0);
        lv_obj_set_style_bg_grad_dir(box, LV_GRAD_DIR_NONE, 0);
        lv_obj_set_style_radius(box, 8, 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    }
    c->pantalla = lv_label_create(ui_root(c->g));
    lv_label_set_text(c->pantalla, "0");
    lv_obj_set_style_text_color(c->pantalla, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(c->pantalla, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(c->pantalla, 296);
    lv_obj_align(c->pantalla, LV_ALIGN_TOP_RIGHT, -12, 44);

    c->expr = lv_label_create(ui_root(c->g));
    lv_label_set_text(c->expr, "");
    lv_obj_set_style_text_color(c->expr, lv_color_hex(C_OP), 0);
    lv_obj_set_style_text_align(c->expr, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(c->expr, 296);
    lv_obj_align(c->expr, LV_ALIGN_TOP_RIGHT, -12, 22);
    lvgl_unlock();

    {
        const char *t;
        unsigned col;
        int id, r, k, x, y, w;
        /* 4 columnas, 5 filas */
        int bw = 70, bh = 58, gap = 8, x0 = 8, y0 = 130;
        struct { const char *t; int id; unsigned c; } keys[20] = {
            {"C",T_CLR,C_CLR}, {"+/-",T_SIGNO,C_OP}, {".",T_DP,C_NUM}, {"/",T_DIV,C_OP},
            {"7",T_7,C_NUM},   {"8",T_8,C_NUM},      {"9",T_9,C_NUM},    {"x",T_POR,C_OP},
            {"4",T_4,C_NUM},   {"5",T_5,C_NUM},      {"6",T_6,C_NUM},    {"-",T_MENOS,C_OP},
            {"1",T_1,C_NUM},   {"2",T_2,C_NUM},      {"3",T_3,C_NUM},    {"+",T_MAS,C_OP},
            {"0",T_0,C_NUM},   {"0",T_0,C_NUM},      {"<",T_BORRA,C_OP}, {"=",T_IGUAL,C_ACC}
        };

        for (r = 0; r < 5; r++)
        {
            for (k = 0; k < 4; k++)
            {
                i = r * 4 + k;
                /* ultima fila: 0 doble (cols 0-1), col2 backspace, col3 = */
                if (4 == r && 1 == k) continue;
                t = keys[i].t; id = keys[i].id; col = keys[i].c;
                x = x0 + k * (bw + gap);
                y = y0 + r * (bh + gap);
                w = bw;
                if (4 == r && 0 == k) { w = bw * 2 + gap; }
                if (4 == r && 2 == k) { t = "<"; id = T_BORRA; col = C_OP; }
                ui_button(c->g, id, t, x, y, w, bh, col);
            }
        }
    }

    /* X para salir */
    ui_button(c->g, T_SALIR, "X", 276, 6, 36, 32, C_CLR);

    limpiar_todo(c);
    mostrar(c);
}

/* ---- arranque ------------------------------------------------------------ */

static void salir(struct calc *c)
{
    if (c->slot >= 0)
    {
        appmgr_unregister(c->slot);
    }
    ui_destroy(c->g);
    psram_free(c);
}

static void ir_fondo(struct calc *c, int *fuera)
{
    ui_event_t ev;
    int s;

    if (c->slot < 0)
    {
        return;
    }
    ui_show(c->g, 0);
    appmgr_set_state(c->slot, APP_BACKGROUND);
    while (!*fuera)
    {
        s = appmgr_get_state(c->slot);
        if (APP_RUNNING == s)
        {
            ui_show(c->g, 1);
            return;
        }
        if (APP_CLOSING == s || APP_FREE == s)
        {
            *fuera = 1;
            return;
        }
        while (ui_poll(c->g, &ev))
        {
            ;
        }
        sleep(50);
    }
}

int main(int argc, char *argv[])
{
    struct calc *c;
    ui_event_t ev;
    int fuera = 0;

    (void)argc; (void)argv;
    printf("calculadora: arranca (tid=%d)\r\n", gettid());

    c = (struct calc *)psram_malloc(sizeof(struct calc));
    if (NULL == c) { printf("calculadora: sin PSRAM\r\n"); exit(1); }
    memset(c, 0, sizeof(struct calc));
    c->slot = SYSERR;

    c->g = ui_create_ex(UI_CHROME_NONE);
    if (NULL == c->g) { psram_free(c); exit(1); }

    c->slot = appmgr_register("Calculadora", "/apps/calculadora/app.elf", 0x5DADE2, gettid());

    crear_ui(c);

    while (!fuera)
    {
        while (ui_poll(c->g, &ev))
        {
            procesar(c, &ev, &fuera);
            if (fuera) break;
        }
        if (!fuera && c->slot >= 0 && APP_CLOSING == appmgr_get_state(c->slot))
        {
            fuera = 1;
        }
        if (!fuera) sleep(20);
    }

    salir(c);
    exit(0);
    return 0;
}
