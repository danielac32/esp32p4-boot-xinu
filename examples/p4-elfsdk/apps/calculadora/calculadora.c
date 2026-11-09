/**
 * @file calculadora.c
 *
 * Calculadora basica para el pad LCD del P4.
 *
 *   elf /elf/calculadora.elf
 *
 * Botones de la libreria de gestos (un id por tecla) y un rotulo de LVGL para
 * la pantalla.  La aritmetica es ENTERA (32 bits con signo): suma, resta,
 * producto y division (truncada hacia cero).  Sin coma flotante, que el kernel
 * y las apps no la usan.  Division por cero y desbordamiento muestran "ERR".
 *
 * Estado: todo en un bloque de PSRAM colgado del handle.  Nada de estaticas
 * escribibles: el cargador de ELF no las soporta.  El bucle de main saca
 * eventos con gestos_leer() y solo el llama a exit(); los callbacks de LVGL
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

void exit(int codigo);

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
#define T_SALIR 18

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
    gestos_t *g;
    lv_obj_t *pantalla;         /* rotulo donde se ve el numero               */
    lv_obj_t *expr;             /* linea chica con la operacion pendiente     */
    long acum;                  /* operando izquierdo / resultado             */
    long valor;                 /* numero que se esta tecleando               */
    int op;                     /* OP_* pendiente                             */
    int nuevo;                  /* el proximo digito empieza un numero nuevo  */
    int error;                  /* 1 = mostrar ERR hasta limpiar              */
    int neg;                    /* el numero en curso es negativo             */
    char buf[24];
    char ebuf[40];
    int slot;                   /* ranura en el gestor de apps (-1 = solo)    */
};

/* ---- pantalla ------------------------------------------------------------ */

static void mostrar(struct calc *c)
{
    if (c->error)
    {
        lvgl_lock();
        lv_label_set_text(c->pantalla, "ERR");
        lv_label_set_text(c->expr, "");
        lvgl_unlock();
        return;
    }

    /* El numero en curso se muestra tal cual se teclea (con signo).  Si no
     * se esta tecleando, se muestra el acumulador. */
    if (!c->nuevo)
    {
        long v = c->neg ? -c->valor : c->valor;

        snprintf(c->buf, sizeof(c->buf), "%ld", v);
    }
    else
    {
        snprintf(c->buf, sizeof(c->buf), "%ld", c->acum);
    }

    lvgl_lock();
    lv_label_set_text(c->pantalla, c->buf);
    lv_label_set_text(c->expr, c->ebuf);
    lvgl_unlock();
}

static void poner_expr(struct calc *c, const char *sop)
{
    if ('\0' == sop[0])
    {
        c->ebuf[0] = '\0';
    }
    else
    {
        snprintf(c->ebuf, sizeof(c->ebuf), "%ld %s", c->acum, sop);
    }
}

static void limpiar_todo(struct calc *c)
{
    c->acum = 0;
    c->valor = 0;
    c->op = OP_NINGUNA;
    c->nuevo = 1;
    c->error = 0;
    c->neg = 0;
    c->ebuf[0] = '\0';
}

/* ---- aritmetica (entera, con deteccion basica) -------------------------- */

static int desborda_mul(long a, long b, long *r)
{
    /* Limite conservador para 32 bits con signo. */
    if (0 == a || 0 == b)
    {
        *r = 0;
        return 0;
    }
    if (a > 0 && b > 0 && a > 2147483647L / b) return 1;
    if (a > 0 && b < 0 && b < -2147483647L / a) return 1;
    if (a < 0 && b > 0 && a < -2147483647L / b) return 1;
    if (a < 0 && b < 0 && a < 2147483647L / b) return 1;
    *r = a * b;
    return 0;
}

static long calcular(long a, long b, int op, int *err)
{
    long r = 0;

    *err = 0;
    switch (op)
    {
    case OP_MAS:
        /* desbordamiento simple por signo */
        r = a + b;
        if ((b > 0 && r < a) || (b < 0 && r > a))
        {
            *err = 1;
        }
        return r;
    case OP_MENOS:
        r = a - b;
        if ((b < 0 && r < a) || (b > 0 && r > a))
        {
            *err = 1;
        }
        return r;
    case OP_POR:
        *err = desborda_mul(a, b, &r);
        return r;
    case OP_DIV:
        if (0 == b)
        {
            *err = 1;
            return 0;
        }
        return a / b;
    default:
        return b;
    }
}

static long valor_actual(struct calc *c)
{
    if (c->nuevo)
    {
        return c->acum;
    }
    return c->neg ? -c->valor : c->valor;
}

static void aplicar_operador(struct calc *c, int op, const char *sop)
{
    long v;
    int err;

    if (c->error)
    {
        return;
    }

    v = valor_actual(c);
    if (OP_NINGUNA == c->op)
    {
        c->acum = v;
    }
    else if (!c->nuevo)
    {
        c->acum = calcular(c->acum, v, c->op, &err);
        if (err)
        {
            c->error = 1;
            mostrar(c);
            return;
        }
    }

    c->op = op;
    c->nuevo = 1;
    c->valor = 0;
    c->neg = 0;
    poner_expr(c, sop);
    mostrar(c);
}

/* ---- teclas -------------------------------------------------------------- */

static void tecla_digito(struct calc *c, int d)
{
    if (c->error)
    {
        return;
    }
    if (c->nuevo)
    {
        c->valor = 0;
        c->neg = 0;
        c->nuevo = 0;
        c->ebuf[0] = '\0';
    }
    /* Tope para no pasar de 9 digitos y no desbordar al teclear. */
    if (c->valor < 100000000L)
    {
        c->valor = c->valor * 10 + d;
    }
    mostrar(c);
}

static void tecla_igual(struct calc *c)
{
    long v;
    int err;

    if (c->error || OP_NINGUNA == c->op)
    {
        return;
    }
    v = valor_actual(c);
    c->acum = calcular(c->acum, v, c->op, &err);
    c->op = OP_NINGUNA;
    c->nuevo = 1;
    c->valor = 0;
    c->neg = 0;
    c->error = err;
    c->ebuf[0] = '\0';
    mostrar(c);
}

static void ir_fondo(struct calc *c, int *fuera);

static void procesar(struct calc *c, gev_t *ev, int *fuera)
{
    if (GE_ARRASTRE == ev->tipo)
    {
        if (GE_ARRIBA == ev->dir)
        {
            ir_fondo(c, fuera);
        }
        return;
    }
    if (GE_BOTON != ev->tipo)
    {
        return;
    }

    switch (ev->id)
    {
    case T_0: case T_1: case T_2: case T_3: case T_4:
    case T_5: case T_6: case T_7: case T_8: case T_9:
        tecla_digito(c, ev->id); break;
    case T_MAS:   aplicar_operador(c, OP_MAS, "+"); break;
    case T_MENOS: aplicar_operador(c, OP_MENOS, "-"); break;
    case T_POR:   aplicar_operador(c, OP_POR, "x"); break;
    case T_DIV:   aplicar_operador(c, OP_DIV, "/"); break;
    case T_IGUAL: tecla_igual(c); break;
    case T_CLR:   limpiar_todo(c); mostrar(c); break;
    case T_SIGNO:
        if (!c->error)
        {
            if (c->nuevo) { c->acum = -c->acum; }
            else { c->neg = !c->neg; }
            mostrar(c);
        }
        break;
    case T_BORRA:
        if (!c->error && !c->nuevo)
        {
            c->valor /= 10;
            if (0 == c->valor) { c->neg = 0; }
            mostrar(c);
        }
        break;
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
        lv_obj_t *tit = lv_label_create(gestos_raiz(c->g));
        lv_label_set_text(tit, "CALC");
        lv_obj_set_style_text_color(tit, lv_color_hex(C_TEXTO), 0);
        lv_obj_align(tit, LV_ALIGN_TOP_LEFT, 10, 6);
    }
    /* fondo de la pantalla de numeros (se crea antes que los rotulos para que
     * los rotulos queden encima al dibujarse). */
    {
        lv_obj_t *box = lv_obj_create(gestos_raiz(c->g));
        lv_obj_set_size(box, 304, 64);
        lv_obj_align(box, LV_ALIGN_TOP_LEFT, 8, 38);
        lv_obj_set_style_bg_color(box, lv_color_hex(C_PANT), 0);
        lv_obj_set_style_bg_grad_dir(box, LV_GRAD_DIR_NONE, 0);
        lv_obj_set_style_radius(box, 8, 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    }
    c->pantalla = lv_label_create(gestos_raiz(c->g));
    lv_label_set_text(c->pantalla, "0");
    lv_obj_set_style_text_color(c->pantalla, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(c->pantalla, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(c->pantalla, 296);
    lv_obj_align(c->pantalla, LV_ALIGN_TOP_RIGHT, -12, 44);

    c->expr = lv_label_create(gestos_raiz(c->g));
    lv_label_set_text(c->expr, "");
    lv_obj_set_style_text_color(c->expr, lv_color_hex(C_TEXTO), 0);
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
            {"C",T_CLR,C_CLR}, {"+/-",T_SIGNO,C_OP}, {"<",T_BORRA,C_OP}, {"/",T_DIV,C_OP},
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
                gestos_boton(c->g, id, t, x, y, w, bh, col);
            }
        }
    }

    /* X para salir */
    gestos_boton(c->g, T_SALIR, "X", 276, 6, 36, 32, C_CLR);

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
    gestos_cerrar(c->g);
    psram_free(c);
}

static void ir_fondo(struct calc *c, int *fuera)
{
    gev_t ev;
    int s;

    if (c->slot < 0)
    {
        return;
    }
    gestos_show(c->g, 0);
    appmgr_set_state(c->slot, APP_BACKGROUND);
    while (!*fuera)
    {
        s = appmgr_get_state(c->slot);
        if (APP_RUNNING == s)
        {
            gestos_show(c->g, 1);
            return;
        }
        if (APP_CLOSING == s || APP_FREE == s)
        {
            *fuera = 1;
            return;
        }
        while (gestos_leer(c->g, &ev))
        {
            ;
        }
        sleep(50);
    }
}

int main(int argc, char *argv[])
{
    struct calc *c;
    gev_t ev;
    int fuera = 0;

    (void)argc; (void)argv;
    printf("calculadora: arranca (tid=%d)\r\n", gettid());

    c = (struct calc *)psram_malloc(sizeof(struct calc));
    if (NULL == c) { printf("calculadora: sin PSRAM\r\n"); exit(1); }
    memset(c, 0, sizeof(struct calc));
    c->slot = SYSERR;

    c->g = gestos_nueva_partes(0);
    if (NULL == c->g) { psram_free(c); exit(1); }

    c->slot = appmgr_register("Calculadora", "/apps/calculadora/app.elf", 0x5DADE2, gettid());

    crear_ui(c);

    while (!fuera)
    {
        while (gestos_leer(c->g, &ev))
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
