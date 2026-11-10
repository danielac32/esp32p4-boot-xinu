/**
 * @file tetris.c
 *
 * Tetris para el pad LCD del P4, sobre la libreria de gestos de la app.
 *
 *   elf /apps/tetris/app.elf
 *
 * Tablero de 10x20 celdas de 24 px: ocupa de arriba abajo los 480 px (CELL*
 * ROWS) y deja la columna derecha para el marcador, la siguiente ficha y los
 * mandos.  El pintado va a un lv_canvas y lo bombea la hebra residente
 * `lvglui`, asi que la app solo pinta cuando cambia algo.
 *
 * Mandos:
 *   - arrastre horizontal ... mover la ficha una columna
 *   - arrastre hacia abajo ... bajar una fila (soft drop, suma 1 punto)
 *   - arrastre hacia arriba . "hard drop": cae al fondo (2 puntos por fila)
 *   - boton flecha ABAJO (azul, bajo "SIGUIENTE"): hard drop
 *   - boton flecha de GIRO (morado, debajo del anterior): girar 90 grados
 *   - X ..................... salir
 *   - arrastre arriba desde el borde inferior: devuelve al launcher
 *     (en un juego el arriba normal tiene otro significado, por eso el gesto
 *     de "volver" exige UI_EDGE_BOTTOM, como en las demas apps)
 *
 * Reglas: las filas completas se borran y bajan las de arriba; 100 x nivel por
 * fila, 1 por fila de soft drop y 2 por hard drop; cada 10 filas sube el nivel
 * y la gravedad acelera (700 ms, menos 60 por nivel, suelo 80 ms).  La
 * puntuacion es entera: nada de coma flotante.
 *
 * Detalles: hay pieza "fantasma" (el aterrizaje, en gris translucido), una
 * vista previa de la ficha siguiente y una rejilla de fondo.
 *
 * Estado: todo en un bloque de PSRAM colgado del handle.  Nada de estaticas
 * escribibles (el cargador de ELF no las soporta).  El bucle de main saca
 * eventos con ui_poll() y solo el llama a exit(); los callbacks de LVGL corren
 * en lvglui y solo apilan.
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

LV_FONT_DECLARE(lv_font_unscii_8);

/* ---- medidas ------------------------------------------------------------- */

#define COLS 10
#define ROWS 20
#define CELL 24
#define ANCHO (COLS * CELL)      /* 240 px */
#define ALTO  (ROWS * CELL)      /* 480 px: de arriba a abajo           */
#define X_TAB 4
#define Y_TAB 0

/* Vista previa de la siguiente ficha: 4x3 celdas de 16 px */
#define PXW 64
#define PXH 48
#define PSZ 16

/* ---- ids de mando -------------------------------------------------------- */

#define ID_CAE   11
#define ID_ROT   13
#define ID_SALIR 12

/* ---- estados ------------------------------------------------------------- */

#define ESTADO_JUEGA 0
#define ESTADO_FIN   1

/* ---- colores (#RRGGBB; LVGL los pasa a RGB565) -------------------------- */

#define C_FONDO_TOP 0x152238
#define C_FONDO_BOT 0x060C16
#define C_TABLERO   0x0B1622
#define C_REJILLA   0x182C44
#define C_BORDE     0x2F6ECE
#define C_TEXTO     0xE8D8B0
#define C_AMBER     0xFFB302
#define C_ROJO      0xE74C3C
#define C_GRIS      0x5D6D7E
#define C_VERDE     0x2ECC71

/* Un color por tetromino (1..7, ver ficha_base()). */
#define C_I 0x2E86DE
#define C_O 0xE67E22
#define C_T 0x9B59B6
#define C_S 0x16A085
#define C_Z 0xC0392B
#define C_J 0x2471A3
#define C_L 0xE1A100

struct juego
{
    ui_input_t *g;
    int b[COLS * ROWS];          /* 0 vacia; 1..7 = color del tetromino        */
    int ptipo;                   /* tetromino en curso                          */
    int sigi;                    /* siguiente tetromino (vista previa)          */
    int prot;                    /* rotacion actual 0..3                       */
    int pcells[4][2];            /* celdas de la ficha, relativas a su origen   */
    int pcol, prow;              /* origen de la ficha en el tablero            */
    int score, nivel, lineas;
    int estado;                  /* ESTADO_*                                   */
    int bajada;                  /* ms acumulados para la gravedad             */
    unsigned rnd;                /* generador entero de piezas                 */
    lv_obj_t *tab;               /* canvas del tablero                         */
    lv_obj_t *prev;              /* canvas de la siguiente ficha               */
    lv_obj_t *estado_lbl;        /* rotulo de estado (JUEGA / FIN)             */
    lv_obj_t *marcador;          /* puntos, nivel y lineas                     */
    void *buf;                   /* buffer del canvas del tablero (PSRAM)      */
    void *pbuf;                  /* buffer del canvas de la siguiente (PSRAM)  */
    void *dbuf;                  /* buffer de la flecha CAE (PSRAM)            */
    void *rbuf;                  /* buffer de la flecha GIRA (PSRAM)           */
    int slot;
};

/* ---- piezas -------------------------------------------------------------- */

static unsigned color_ficha(int tipo)
{
    switch (tipo)
    {
    case 1: return C_I;
    case 2: return C_O;
    case 3: return C_T;
    case 4: return C_S;
    case 5: return C_Z;
    case 6: return C_J;
    default: return C_L;
    }
}

/* Celdas de la orientacion base de cada pieza, en una caja de 4x4; las demas
 * salen de girar esta (rotar()). */
static void ficha_base(int tipo, int c[4][2])
{
    switch (tipo)
    {
    case 1:                      /* I: barra horizontal */
        c[0][0] = 0; c[0][1] = 0; c[1][0] = 1; c[1][1] = 0;
        c[2][0] = 2; c[2][1] = 0; c[3][0] = 3; c[3][1] = 0;
        break;
    case 2:                      /* O: cuadrado */
        c[0][0] = 0; c[0][1] = 0; c[1][0] = 1; c[1][1] = 0;
        c[2][0] = 0; c[2][1] = 1; c[3][0] = 1; c[3][1] = 1;
        break;
    case 3:                      /* T */
        c[0][0] = 0; c[0][1] = 1; c[1][0] = 1; c[1][1] = 1;
        c[2][0] = 2; c[2][1] = 1; c[3][0] = 1; c[3][1] = 0;
        break;
    case 4:                      /* S */
        c[0][0] = 1; c[0][1] = 0; c[1][0] = 2; c[1][1] = 0;
        c[2][0] = 0; c[2][1] = 1; c[3][0] = 1; c[3][1] = 1;
        break;
    case 5:                      /* Z */
        c[0][0] = 0; c[0][1] = 0; c[1][0] = 1; c[1][1] = 0;
        c[2][0] = 1; c[2][1] = 1; c[3][0] = 2; c[3][1] = 1;
        break;
    case 6:                      /* J */
        c[0][0] = 0; c[0][1] = 0; c[1][0] = 0; c[1][1] = 1;
        c[2][0] = 0; c[2][1] = 2; c[3][0] = 1; c[3][1] = 2;
        break;
    default:                     /* 7 = L */
        c[0][0] = 1; c[0][1] = 0; c[1][0] = 1; c[1][1] = 1;
        c[2][0] = 0; c[2][1] = 2; c[3][0] = 1; c[3][1] = 2;
        break;
    }
}

/* Gira 90 grados en sentido horario las celdas relativas: (x,y) -> (y,-x).
 * Despues se normaliza a la esquina (minimo 0).  Todo con enteros. */
static void rotar(int c[4][2])
{
    int nc[4][2];
    int k, minx = 99, miny = 99;

    for (k = 0; k < 4; k++)
    {
        int x = c[k][0];
        int y = c[k][1];

        nc[k][0] = y;
        nc[k][1] = -x;
        if (nc[k][0] < minx) minx = nc[k][0];
        if (nc[k][1] < miny) miny = nc[k][1];
    }
    for (k = 0; k < 4; k++)
    {
        c[k][0] = nc[k][0] - minx;
        c[k][1] = nc[k][1] - miny;
    }
}

/* ---- tablero ------------------------------------------------------------- */

/* 1 si la ficha chocaria al desplazarse (dx,dy): pared, suelo o celda ocupada.
 * Las filas por encima del tablero (y < 0) se dejan pasar. */
static int choca(const struct juego *m, int dx, int dy)
{
    int k;

    for (k = 0; k < 4; k++)
    {
        int x = m->pcol + m->pcells[k][0] + dx;
        int y = m->prow + m->pcells[k][1] + dy;

        if (x < 0 || x >= COLS || y >= ROWS) return 1;
        if (y >= 0 && m->b[y * COLS + x]) return 1;
    }
    return 0;
}

/* Borra las filas completas y devuelve cuantas cayeron. */
static int limpiar_lineas(struct juego *m)
{
    int r = ROWS - 1;
    int n = 0;

    while (r >= 0)
    {
        int c, llena = 1, rr;

        for (c = 0; c < COLS; c++)
        {
            if (0 == m->b[r * COLS + c]) { llena = 0; break; }
        }
        if (!llena) { r--; continue; }

        for (rr = r; rr > 0; rr--)
        {
            memcpy(&m->b[rr * COLS], &m->b[(rr - 1) * COLS],
                   COLS * sizeof(int));
        }
        for (c = 0; c < COLS; c++) m->b[c] = 0;
        n++;
    }
    m->lineas += n;
    return n;
}

static void dibujar(struct juego *m);
static void dibujar_sig(struct juego *m);

/* Saca una pieza nueva.  El generador es el LCG clasico de 32 bits: entero,
 * sin tablas y sin coma flotante. */
static void nueva_ficha(struct juego *m)
{
    m->rnd = m->rnd * 1103515245u + 12345u;
    m->ptipo = m->sigi;
    m->sigi = (int)((m->rnd >> 16) % 7u) + 1;
    m->prot = 0;
    ficha_base(m->ptipo, m->pcells);
    m->pcol = 3;
    m->prow = 0;

    if (choca(m, 0, 0))
    {
        m->estado = ESTADO_FIN;
    }
}

/* Fija la ficha en el tablero, limpia filas y saca la siguiente. */
static void fijar(struct juego *m)
{
    int k, n;

    for (k = 0; k < 4; k++)
    {
        int x = m->pcol + m->pcells[k][0];
        int y = m->prow + m->pcells[k][1];

        if (x >= 0 && x < COLS && y >= 0 && y < ROWS)
        {
            m->b[y * COLS + x] = m->ptipo;
        }
    }

    n = limpiar_lineas(m);
    if (n > 0)
    {
        m->score += 100 * n * m->nivel;
    }
    m->nivel = m->lineas / 10 + 1;

    nueva_ficha(m);
    dibujar(m);
    dibujar_sig(m);
}

static void paso_gravedad(struct juego *m)
{
    if (choca(m, 0, 1))
    {
        fijar(m);
    }
    else
    {
        m->prow++;
        dibujar(m);
    }
}

static void bajar(struct juego *m)
{
    if (choca(m, 0, 1))
    {
        fijar(m);
    }
    else
    {
        m->prow++;
        m->score += 1;
        dibujar(m);
    }
}

/* Hard drop: hasta el fondo, dos puntos por fila y fija. */
static void caer(struct juego *m)
{
    int d = 0;

    while (!choca(m, 0, 1))
    {
        m->prow++;
        d++;
    }
    m->score += 2 * d;
    fijar(m);
}

static void mover(struct juego *m, int dx)
{
    if (!choca(m, dx, 0))
    {
        m->pcol += dx;
        dibujar(m);
    }
}

static void girar(struct juego *m)
{
    int guard[4][2];

    memcpy(guard, m->pcells, sizeof(guard));
    rotar(m->pcells);
    if (choca(m, 0, 0))
    {
        /* Sin hueco para girar junto a la pared: se deshace. */
        memcpy(m->pcells, guard, sizeof(guard));
        return;
    }
    m->prot = (m->prot + 1) % 4;
    dibujar(m);
}

static int gravedad_ms(const struct juego *m)
{
    int ms = 700 - (m->nivel - 1) * 60;

    if (ms < 80) ms = 80;
    return ms;
}

/* ---- pintura ------------------------------------------------------------- */

/* Una celda rellena: rectangulo redondeado del color de la pieza. */
static void celda(lv_obj_t *tab, int x, int y, int size, unsigned color,
                  lv_opa_t opa)
{
    lv_draw_rect_dsc_t dsc;

    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = 3;
    dsc.border_width = 0;
    dsc.shadow_width = 0;
    dsc.bg_opa = opa;
    dsc.bg_color = lv_color_hex(color);
    lv_canvas_draw_rect(tab, x, y, size, size, &dsc);
}

/* Rejilla de fondo: lineas muy tenues entre celdas. */
static void rejilla(lv_obj_t *tab)
{
    lv_draw_rect_dsc_t dsc;
    int i;

    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = 0;
    dsc.border_width = 0;
    dsc.shadow_width = 0;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.bg_color = lv_color_hex(C_REJILLA);

    for (i = 1; i < COLS; i++)
    {
        lv_canvas_draw_rect(tab, i * CELL, 0, 1, ALTO, &dsc);
    }
    for (i = 1; i < ROWS; i++)
    {
        lv_canvas_draw_rect(tab, 0, i * CELL, ANCHO, 1, &dsc);
    }
}

/* Marco: dos rayas delgadas a todo el contorno del tablero. */
static void marco(lv_obj_t *tab)
{
    lv_draw_rect_dsc_t dsc;

    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = 0;
    dsc.border_width = 0;
    dsc.shadow_width = 0;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.bg_color = lv_color_hex(C_BORDE);

    lv_canvas_draw_rect(tab, 0, 0, ANCHO, 2, &dsc);
    lv_canvas_draw_rect(tab, 0, ALTO - 2, ANCHO, 2, &dsc);
    lv_canvas_draw_rect(tab, 0, 0, 2, ALTO, &dsc);
    lv_canvas_draw_rect(tab, ANCHO - 2, 0, 2, ALTO, &dsc);
}

/* Copia el tablero y las piezas (fantasma y actual) al canvas.  Solo se llama
 * cuando cambia algo: no hay animacion continua. */
static void dibujar(struct juego *m)
{
    int r, c, k;

    lvgl_lock();
    lv_canvas_fill_bg(m->tab, lv_color_hex(C_TABLERO), LV_OPA_COVER);
    rejilla(m->tab);
    marco(m->tab);

    /* Celdas del tablero */
    for (r = 0; r < ROWS; r++)
    {
        for (c = 0; c < COLS; c++)
        {
            int v = m->b[r * COLS + c];

            if (0 == v) continue;
            celda(m->tab, c * CELL + 1, r * CELL + 1, CELL - 2,
                  color_ficha(v), LV_OPA_COVER);
        }
    }

    if (ESTADO_JUEGA == m->estado)
    {
        /* Fantasma: el aterrizaje, translucido */
        int d = 0;

        while (!choca(m, 0, d + 1)) d++;
        if (d > 0)
        {
            for (k = 0; k < 4; k++)
            {
                int x = m->pcol + m->pcells[k][0];
                int y = m->prow + m->pcells[k][1] + d;

                if (y < 0) continue;
                celda(m->tab, x * CELL + 2, y * CELL + 2, CELL - 4,
                      color_ficha(m->ptipo), LV_OPA_20);
            }
        }

        /* Ficha actual */
        for (k = 0; k < 4; k++)
        {
            int x = m->pcol + m->pcells[k][0];
            int y = m->prow + m->pcells[k][1];

            if (y < 0) continue;
            celda(m->tab, x * CELL + 1, y * CELL + 1, CELL - 2,
                  color_ficha(m->ptipo), LV_OPA_COVER);
        }
    }
    lvgl_unlock();
}

/* La ficha que viene: centrada en su canvas. */
static void dibujar_sig(struct juego *m)
{
    int c[4][2];
    int k, minx = 99, maxx = -99, miny = 99, maxy = -99;
    int dx, dy, offx, offy;

    ficha_base(m->sigi, c);

    for (k = 0; k < 4; k++)
    {
        if (c[k][0] < minx) minx = c[k][0];
        if (c[k][0] > maxx) maxx = c[k][0];
        if (c[k][1] < miny) miny = c[k][1];
        if (c[k][1] > maxy) maxy = c[k][1];
    }
    dx = maxx - minx + 1;
    dy = maxy - miny + 1;
    offx = (PXW - dx * PSZ) / 2 - minx * PSZ;
    offy = (PXH - dy * PSZ) / 2 - miny * PSZ;

    lvgl_lock();
    lv_canvas_fill_bg(m->prev, lv_color_hex(C_TABLERO), LV_OPA_COVER);
    for (k = 0; k < 4; k++)
    {
        celda(m->prev, offx + c[k][0] * PSZ + 1, offy + c[k][1] * PSZ + 1,
              PSZ - 2, color_ficha(m->sigi), LV_OPA_COVER);
    }
    lvgl_unlock();
}

static void poner_estado(struct juego *m, const char *txt, unsigned rgb)
{
    lvgl_lock();
    lv_label_set_text(m->estado_lbl, txt);
    lv_obj_set_style_text_color(m->estado_lbl, lv_color_hex(rgb), 0);
    lvgl_unlock();
}

static void poner_marcador(struct juego *m)
{
    char buf[48];

    snprintf(buf, sizeof(buf), "PTS %d\nNIV %d\nLIN %d",
             m->score, m->nivel, m->lineas);
    lvgl_lock();
    lv_label_set_text(m->marcador, buf);
    lvgl_unlock();
}

/* Tras cualquier jugada: marcador al dia y, si la partida acabo, el rotulo. */
static void refrescar(struct juego *m)
{
    poner_marcador(m);
    if (ESTADO_FIN == m->estado)
    {
        poner_estado(m, "FIN", C_ROJO);
    }
}

/* ---- interfaz ------------------------------------------------------------ */

/* Flecha hacia abajo (botao CAE): el boton entero ES una flecha blanca
 * sobre el azul del boton.  El canvas es HIJO del boton (se crea despues y
 * queda garantizado encima del fondo del boton) y no es clicable, asi que
 * el toque le alcanza siempre al boton. */
static void flecha_abajo(struct juego *m, lv_obj_t *btn, int w, int h)
{
    void *b = psram_malloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(w, h));
    lv_obj_t *cv;
    lv_draw_rect_dsc_t dsc;
    lv_point_t p[3];
    int cx = w / 2;

    if (NULL == b) { return; }

    lvgl_lock();
    /* el boton lleva padding propio: sin anularlo, pos(0,0) mandaria el
     * canvas al interior y la flecha quedaria descuadrada */
    lv_obj_set_style_pad_all(btn, 0, 0);
    cv = lv_canvas_create(btn);
    lv_canvas_set_buffer(cv, b, w, h, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(cv, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(cv, LV_OBJ_FLAG_CLICKABLE);
    lv_canvas_fill_bg(cv, lv_color_hex(C_I), LV_OPA_COVER);

    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = 0;
    dsc.border_width = 0;
    dsc.shadow_width = 0;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.bg_color = lv_color_hex(0xFFFFFF);
    /* asta gruesa */
    lv_canvas_draw_rect(cv, cx - 11, 4, 22, 15, &dsc);
    /* punta ancha: la mitad baja del boton */
    p[0].x = cx - 24; p[0].y = 20;
    p[1].x = cx + 24; p[1].y = 20;
    p[2].x = cx;      p[2].y = h - 2;
    lv_canvas_draw_polygon(cv, p, 3, &dsc);
    lvgl_unlock();

    if (NULL != m->dbuf) psram_free(m->dbuf);
    m->dbuf = b;
}

/* Flecha de giro (botao GIRA): el boton entero es un simbolo de refresco,
 * dos medias flechas blancas opuestas en sentido horario.  Hijo del boton,
 * garantizado encima de su fondo. */
static void flecha_gira(struct juego *m, lv_obj_t *btn, int w, int h)
{
    void *b = psram_malloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(w, h));
    lv_obj_t *cv;
    lv_draw_arc_dsc_t adsc;
    lv_draw_rect_dsc_t dsc;
    lv_point_t p[3];
    int cx = w / 2, cy = h / 2;
    float r;

    if (NULL == b) { return; }

    lvgl_lock();
    lv_obj_set_style_pad_all(btn, 0, 0);
    cv = lv_canvas_create(btn);
    lv_canvas_set_buffer(cv, b, w, h, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(cv, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(cv, LV_OBJ_FLAG_CLICKABLE);
    lv_canvas_fill_bg(cv, lv_color_hex(C_T), LV_OPA_COVER);

    r = (float)(h / 2) - 7.0f;
    lv_draw_arc_dsc_init(&adsc);
    adsc.width = 7;
    adsc.color = lv_color_hex(0xFFFFFF);
    adsc.opa = LV_OPA_COVER;
    /* dos medios arcos opuestos, sentido horario */
    lv_canvas_draw_arc(cv, cx, cy, (lv_coord_t)r, 250, 60, &adsc);
    lv_canvas_draw_arc(cv, cx, cy, (lv_coord_t)r, 70, 240, &adsc);

    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = 0;
    dsc.border_width = 0;
    dsc.shadow_width = 0;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.bg_color = lv_color_hex(0xFFFFFF);

    /* Punta arriba-derecha (60 grados), tangente hacia abajo-derecha */
    {
        float s = 0.866f, c = 0.5f;       /* sin60, cos60 */
        int tx = (int)(cx + r * s);
        int ty = (int)(cy - r * c);

        p[0].x = tx + 10; p[0].y = ty + 13;   /* apex, tangente        */
        p[1].x = tx + 9;  p[1].y = ty - 7;    /* base                  */
        p[2].x = tx - 10; p[2].y = ty + 0;    /* base                  */
        lv_canvas_draw_polygon(cv, p, 3, &dsc);
    }
    /* Punta abajo-izquierda (240 grados), tangente hacia arriba-izquierda */
    {
        float s = -0.866f, c = -0.5f;     /* sin240, cos240 */
        int tx = (int)(cx + r * s);
        int ty = (int)(cy - r * c);

        p[0].x = tx - 10; p[0].y = ty - 13;   /* apex, tangente        */
        p[1].x = tx - 9;  p[1].y = ty + 7;    /* base                  */
        p[2].x = tx + 10; p[2].y = ty + 0;    /* base                  */
        lv_canvas_draw_polygon(cv, p, 3, &dsc);
    }
    lvgl_unlock();

    if (NULL != m->rbuf) psram_free(m->rbuf);
    m->rbuf = b;
}

/* Una etiqueta pequena con fuente unscii_8: las etiquetas gordas de 16 px
 * empizan el pad. */
static lv_obj_t *rotulo(lv_obj_t *raiz, const char *txt, int x, int y,
                        unsigned rgb)
{
    lv_obj_t *l = lv_label_create(raiz);

    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(l, lv_color_hex(rgb), 0);
    lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, txt);
    return l;
}

static void crear(struct juego *m)
{
    lv_obj_t *raiz = ui_root(m->g);

    lvgl_lock();

    /* Fondo: degradado vertical */
    lv_obj_set_style_bg_color(raiz, lv_color_hex(C_FONDO_TOP), 0);
    lv_obj_set_style_bg_grad_color(raiz, lv_color_hex(C_FONDO_BOT), 0);
    lv_obj_set_style_bg_grad_dir(raiz, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(raiz, LV_OPA_COVER, 0);

    /* El canvas primero: debajo de rotulos y botones */
    m->tab = lv_canvas_create(raiz);
    lv_canvas_set_buffer(m->tab, m->buf, ANCHO, ALTO, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(m->tab, LV_ALIGN_TOP_LEFT, X_TAB, Y_TAB);
    /* Primera pintada: sin esto el panel mostraba PSRAM sin inicializar
     * durante un instante al abrir el app. */
    lv_canvas_fill_bg(m->tab, lv_color_hex(C_TABLERO), LV_OPA_COVER);

    /* La siguiente ficha */
    m->prev = lv_canvas_create(raiz);
    lv_canvas_set_buffer(m->prev, m->pbuf, PXW, PXH, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(m->prev, LV_ALIGN_TOP_LEFT, 252, 140);
    lv_canvas_fill_bg(m->prev, lv_color_hex(C_TABLERO), LV_OPA_COVER);

    rotulo(raiz, "TETRIS", 252, 6, C_AMBER);
    m->estado_lbl = rotulo(raiz, "JUEGA", 252, 22, C_TEXTO);
    m->marcador = rotulo(raiz, "PTS 0\nNIV 1\nLIN 0", 252, 40, C_TEXTO);
    rotulo(raiz, "SIGUIENTE", 252, 112, C_GRIS);
    rotulo(raiz, "BOT=GIRA", 252, 384, C_GRIS);

    lvgl_unlock();

    /* Mandos: ui_button() toma el candado por dentro.  CAE lleva una flecha
     * hacia abajo y GIRA una flecha de giro; el texto va vacio y las flechas
     * son canvases no clicables HIJOS de los botones (el toque le alcanza al
     * boton).  Ya no se gira tocando el tablero: solo con GIRA. */
    {
        lv_obj_t *b_cae = ui_button(m->g, ID_CAE, "", 252, 210, 64, 48, C_I);
        flecha_abajo(m, b_cae, 64, 48);
        lv_obj_t *b_rot = ui_button(m->g, ID_ROT, "", 252, 268, 64, 48, C_T);
        flecha_gira(m, b_rot, 64, 48);
    }
    ui_button(m->g, ID_SALIR, "X", 252, 326, 64, 48, C_ROJO);
}

/* ---- partida ------------------------------------------------------------- */

static void reiniciar(struct juego *m)
{
    memset(m->b, 0, sizeof(m->b));
    m->score = 0;
    m->nivel = 1;
    m->lineas = 0;
    m->bajada = 0;
    m->estado = ESTADO_JUEGA;
    m->rnd = 0x1234567u;

    /* La primera "siguiente" ficha se sortea para que la partida no arranque
     * siempre con la I. */
    m->rnd = m->rnd * 1103515245u + 12345u;
    m->sigi = (int)((m->rnd >> 16) % 7u) + 1;

    nueva_ficha(m);
    poner_estado(m, "JUEGA", C_TEXTO);
    poner_marcador(m);
    dibujar(m);
    dibujar_sig(m);
}

static void ir_fondo(struct juego *m, int *fuera);

static void procesar(struct juego *m, ui_event_t *ev, int *fuera)
{
    if (UI_EVENT_SWIPE == ev->tipo)
    {
        /* Volver al launcher: solo el arrastre hacia arriba que arranca en el
         * borde inferior.  El resto de direcciones son mandos del juego. */
        if (UI_EDGE_BOTTOM == ev->edge && UI_DIR_UP == ev->dir)
        {
            ir_fondo(m, fuera);
            return;
        }
        if (ESTADO_JUEGA != m->estado) return;

        if (UI_DIR_LEFT == ev->dir)       mover(m, -1);
        else if (UI_DIR_RIGHT == ev->dir) mover(m, 1);
        else if (UI_DIR_DOWN == ev->dir)  { bajar(m); m->bajada = 0; }
        else if (UI_DIR_UP == ev->dir)    caer(m);
        refrescar(m);
        return;
    }

    if (UI_EVENT_TAP == ev->tipo)
    {
        /* Ya no se gira tocando el tablero: el giro solo lo hace el boton
         * GIRA.  El toque suelto se ignora aqui (los botones son otros
         * eventos). */
        return;
    }

    if (UI_EVENT_BUTTON != ev->tipo) return;

    if (ID_SALIR == ev->id)
    {
        *fuera = 1;
    }
    else if (ID_CAE == ev->id)
    {
        if (ESTADO_JUEGA == m->estado) caer(m);
        refrescar(m);
    }
    else if (ID_ROT == ev->id)
    {
        if (ESTADO_JUEGA == m->estado) girar(m);
    }
}

/* ---- arranque ------------------------------------------------------------ */

static void salir(struct juego *m)
{
    if (m->slot >= 0)
    {
        appmgr_unregister(m->slot);
    }
    ui_destroy(m->g);
    if (m->rbuf != NULL) psram_free(m->rbuf);
    if (m->dbuf != NULL) psram_free(m->dbuf);
    if (m->pbuf != NULL) psram_free(m->pbuf);
    if (m->buf != NULL) psram_free(m->buf);
    psram_free(m);
}

static void ir_fondo(struct juego *m, int *fuera)
{
    ui_event_t ev;
    int s;

    if (m->slot < 0)
    {
        return;
    }
    ui_show(m->g, 0);
    appmgr_set_state(m->slot, APP_BACKGROUND);
    while (!*fuera)
    {
        s = appmgr_get_state(m->slot);
        if (APP_RUNNING == s)
        {
            ui_show(m->g, 1);
            return;
        }
        if (APP_CLOSING == s || APP_FREE == s)
        {
            *fuera = 1;
            return;
        }
        while (ui_poll(m->g, &ev))
        {
            ;
        }
        sleep(50);
    }
}

int main(int argc, char *argv[])
{
    struct juego *m;
    ui_event_t ev;
    int fuera = 0;
    int t_ant;

    (void)argc;
    (void)argv;

    printf("tetris: arranca (tid=%d)\r\n", gettid());

    m = (struct juego *)psram_malloc(sizeof(struct juego));
    if (NULL == m) { printf("tetris: sin PSRAM para el estado\r\n"); exit(1); }
    memset(m, 0, sizeof(struct juego));
    m->slot = SYSERR;

    m->buf = psram_malloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(ANCHO, ALTO));
    if (NULL == m->buf)
    {
        printf("tetris: sin PSRAM para el canvas\r\n");
        psram_free(m);
        exit(1);
    }
    /* PSRAM sin inicializar salio al panel al abrir el app ("ruido de
     * colores"): se deja el buffer a cero y se rellena con el color del
     * tablero antes de que LVGL lo pinte. */
    memset(m->buf, 0, LV_CANVAS_BUF_SIZE_TRUE_COLOR(ANCHO, ALTO));

    m->pbuf = psram_malloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(PXW, PXH));
    if (NULL == m->pbuf)
    {
        psram_free(m->buf);
        psram_free(m);
        exit(1);
    }
    memset(m->pbuf, 0, LV_CANVAS_BUF_SIZE_TRUE_COLOR(PXW, PXH));

    m->g = ui_create_ex(UI_CHROME_NONE);
    if (NULL == m->g)
    {
        psram_free(m->pbuf);
        psram_free(m->buf);
        psram_free(m);
        exit(1);
    }

    m->slot = appmgr_register("Tetris", "/apps/tetris/app.elf", 0x8E44AD,
                              gettid());

    crear(m);
    reiniciar(m);

    t_ant = millis();
    while (!fuera)
    {
        while (ui_poll(m->g, &ev))
        {
            procesar(m, &ev, &fuera);
            if (fuera) break;
        }

        if (!fuera && ESTADO_JUEGA == m->estado)
        {
            int ahora = millis();
            int dt = ahora - t_ant;

            /* Un salto grande (p. ej. al volver del fondo) no topea filas */
            if (dt > 200) dt = 200;
            t_ant = ahora;
            m->bajada += dt;
            if (m->bajada >= gravedad_ms(m))
            {
                m->bajada = 0;
                paso_gravedad(m);
                refrescar(m);
            }
        }
        else
        {
            t_ant = millis();
        }

        if (!fuera && m->slot >= 0 &&
            APP_CLOSING == appmgr_get_state(m->slot))
        {
            fuera = 1;
        }
        if (!fuera) sleep(15);
    }

    salir(m);
    exit(0);
    return 0;
}
