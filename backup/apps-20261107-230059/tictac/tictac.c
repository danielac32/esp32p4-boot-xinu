/**
 * @file tictac.c
 *
 * Tres en raya (tic tac toe) para el pad LCD del P4, sobre la libreria de
 * gestos de la app.
 *
 *   elf /elf/tictac.elf
 *
 * El tablero son nueve botones de gestos (ids 1..9) con el fondo claro de la
 * casilla.  Las fichas NO son texto: la X se dibuja con dos lv_line y la O con
 * un objeto de borde circular.  La fuente por defecto del arbol (unscii 16) es
 * de 16x16 pixeles y en una casilla de 84 px el glifo quedaria diminuto; las
 * lineas y el circulo ocupan ~40 px y se ven de un vistazo.  Ademas esos
 * objetos van SIN CLICKABLE, asi el toque atraviesa el dibujo y llega al boton
 * de la casilla.
 *
 * Dos jugadores en el mismo pad: X empieza y cada toque en una casilla libre
 * pone la ficha del turno.  Gana quien alinee tres; si se llena el tablero sin
 * tres en raya, empate.  Al ganar, una raya cruza las tres fichas de la linea.
 * Esta escrito con enteros y sin coma flotante.
 *
 * Estado: todo en un bloque de PSRAM colgado del handle (tablero, marcador y
 * punteros a los objetos).  Nada de estaticas escribibles, que el cargador de
 * ELF no las soporta.  El bucle de main saca eventos con gestos_leer() y solo
 * el llama a exit(); los callbacks de LVGL corren en lvglui y solo apilan.
 */

#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <timer.h>
#include <memory.h>
#include <device.h>
#include <lvgl.h>
#include "gestos.h"

void exit(int codigo);

/* ---- constantes del juego ------------------------------------------------ */

#define FX 1                     /* ficha X, jugador 1 (empieza)                 */
#define FO 2                     /* ficha O, jugador 2                           */

#define ANCHO 320
#define CELDA 84                 /* lado de cada casilla                         */
#define PASO  92                 /* casilla + 8 px de hueco                      */
#define X0    26                 /* (320 - 3*92 + 8) / 2                        */
#define Y0    64

#define ID_CELDA 1               /* ids 1..9: las nueve casillas                 */
#define ID_NUEVA 10
#define ID_SALIR 11

#define X_SALIR   272
#define Y_SALIR   6
#define AN_SALIR  40
#define AL_SALIR  40

#define X_NUEVA   50
#define Y_NUEVA   344
#define AN_NUEVA  220
#define AL_NUEVA  44

/* colores en #RRGGBB (LVGL los pasa a RGB565) */
#define C_TEXTO  0xE8D8B0
#define C_AMBER  0xFFB302
#define C_CELDA  0xD7E3EE
#define C_X      0x2E86DE
#define C_O      0xE67E22
#define C_VERDE  0x2ECC71
#define C_ROJO   0xE74C3C
#define C_FONDO  0x0B1622

struct juego
{
    gestos_t *g;
    int b[9];                    /* 0 vacia, FX = X, FO = O                     */
    int turno;                   /* FX o FO                                     */
    int fin;                     /* 0 jugando; FX/FO ganador; 3 tablas          */
    int vx, vo, ve;              /* marcador acumulado de la sesion             */
    lv_obj_t *celda[9];          /* boton de cada casilla                       */
    lv_obj_t *circulo[9];        /* anillo de la O                              */
    lv_obj_t *linea[9][2];       /* las dos lineas de la X                      */
    lv_point_t pts[9][4];        /* puntos de esas lineas (los lee lv_line)     */
    lv_obj_t *raya;              /* raya que cruza la linea ganadora            */
    lv_point_t raya_pts[2];      /* extremos de esa raya (los lee lv_line)      */
    lv_obj_t *estado;            /* rotulo de turno / resultado                 */
    lv_obj_t *marcador;          /* X n  O n  EMPATES n                         */
};

/* ---- objetos planos ------------------------------------------------------- */

/* Deja un objeto sin fondo ni adornos del tema: lo usan las lineas y el
 * circulo de O, que solo deben pintar su trazo.  Sin quitar CLICKABLE el
 * dibujo se comeria el toque y la casilla de debajo no recibiria nada. */
static void plano(lv_obj_t *o)
{
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_outline_width(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
}

/* ---- construccion de la pantalla ------------------------------------------ */

static void crear(struct juego *m)
{
    lv_obj_t *raiz = gestos_raiz(m->g);
    int i, fila, col;

    /* Los botones primero: gestos_boton() toma el candado por dentro, asi que
     * no pueden crearse dentro del bloque con lock de mas abajo. */
    for (i = 0; i < 9; i++)
    {
        fila = i / 3;
        col = i % 3;
        m->celda[i] = gestos_boton(m->g, ID_CELDA + i, "",
                                   X0 + col * PASO, Y0 + fila * PASO,
                                   CELDA, CELDA, C_CELDA);
    }

#ifdef PRUEBA_AUTO
    printf("crear: botones\r\n");
    for (i = 0; i < 9; i++)
    {
        if (NULL == m->celda[i])
        {
            printf("crear: celda %d NULL\r\n", i);
        }
    }
#endif

    lvgl_lock();

    lv_obj_set_style_bg_color(raiz, lv_color_hex(C_FONDO), 0);
    lv_obj_set_style_bg_grad_dir(raiz, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(raiz, LV_OPA_COVER, 0);

    lv_obj_t *tit = lv_label_create(raiz);
    lv_obj_clear_flag(tit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(tit, lv_color_hex(C_AMBER), 0);
    lv_obj_align(tit, LV_ALIGN_TOP_MID, 0, 8);
    lv_label_set_text(tit, "TRES EN RAYA");

    m->estado = lv_label_create(raiz);
    lv_obj_clear_flag(m->estado, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(m->estado, lv_color_hex(C_TEXTO), 0);
    lv_obj_align(m->estado, LV_ALIGN_TOP_MID, 0, 30);
    lv_label_set_text(m->estado, "TURNO X");

    m->marcador = lv_label_create(raiz);
    lv_obj_clear_flag(m->marcador, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(m->marcador, lv_color_hex(C_TEXTO), 0);
    lv_obj_align(m->marcador, LV_ALIGN_TOP_MID, 0, 396);
    lv_label_set_text(m->marcador, "X 0  O 0  EMPATES 0");

#ifdef PRUEBA_AUTO
    printf("crear: rotulos\r\n");
#endif

    /* Fichas: DESPUES de los botones para quedar por encima, y no clicables
     * para que el toque siga llegando al boton de la casilla.  Los puntos de
     * las lineas viven en el struct (PSRAM): lv_line guarda el puntero, no una
     * copia, asi que no pueden ser una local de crear(). */
    for (i = 0; i < 9; i++)
    {
        fila = i / 3;
        col = i % 3;
        int cx = X0 + col * PASO;
        int cy = Y0 + fila * PASO;

        m->pts[i][0].x = 22;
        m->pts[i][0].y = 22;
        m->pts[i][1].x = 62;
        m->pts[i][1].y = 62;
        m->pts[i][2].x = 62;
        m->pts[i][2].y = 22;
        m->pts[i][3].x = 22;
        m->pts[i][3].y = 62;

        m->linea[i][0] = lv_line_create(raiz);
        lv_obj_set_size(m->linea[i][0], CELDA, CELDA);
        lv_obj_align(m->linea[i][0], LV_ALIGN_TOP_LEFT, cx, cy);
        plano(m->linea[i][0]);
        lv_obj_set_style_line_width(m->linea[i][0], 7, 0);
        lv_obj_set_style_line_rounded(m->linea[i][0], true, 0);
        lv_obj_set_style_line_color(m->linea[i][0], lv_color_hex(C_X), 0);
        lv_line_set_points(m->linea[i][0], &m->pts[i][0], 2);
        lv_obj_add_flag(m->linea[i][0], LV_OBJ_FLAG_HIDDEN);

        m->linea[i][1] = lv_line_create(raiz);
        lv_obj_set_size(m->linea[i][1], CELDA, CELDA);
        lv_obj_align(m->linea[i][1], LV_ALIGN_TOP_LEFT, cx, cy);
        plano(m->linea[i][1]);
        lv_obj_set_style_line_width(m->linea[i][1], 7, 0);
        lv_obj_set_style_line_rounded(m->linea[i][1], true, 0);
        lv_obj_set_style_line_color(m->linea[i][1], lv_color_hex(C_X), 0);
        lv_line_set_points(m->linea[i][1], &m->pts[i][2], 2);
        lv_obj_add_flag(m->linea[i][1], LV_OBJ_FLAG_HIDDEN);

        /* La O es un aro: caja transparente con borde grueso y radio circulo.
         * 44 px centrados en la casilla de 84 (20 px de margen por lado). */
        m->circulo[i] = lv_obj_create(raiz);
        lv_obj_set_size(m->circulo[i], 44, 44);
        lv_obj_align(m->circulo[i], LV_ALIGN_TOP_LEFT, cx + 20, cy + 20);
        plano(m->circulo[i]);
        lv_obj_set_style_radius(m->circulo[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(m->circulo[i], 7, 0);
        lv_obj_set_style_border_color(m->circulo[i], lv_color_hex(C_O), 0);
        lv_obj_add_flag(m->circulo[i], LV_OBJ_FLAG_HIDDEN);
#ifdef PRUEBA_AUTO
        if (NULL == m->linea[i][0] || NULL == m->linea[i][1]
            || NULL == m->circulo[i])
        {
            printf("crear: dibujo %d NULL\r\n", i);
        }
#endif
    }

    /* Raya de la victoria: se crea al final para quedar por encima de las
     * fichas.  Abarca toda la pantalla porque sus puntos se dan en coordenadas
     * absolutas; plano() la deja sin fondo ni borde y sin CLICKABLE, asi que
     * no estorba a los toques de las casillas. */
    m->raya = lv_line_create(raiz);
    lv_obj_set_size(m->raya, ANCHO, 480);
    lv_obj_align(m->raya, LV_ALIGN_TOP_LEFT, 0, 0);
    plano(m->raya);
    lv_obj_set_style_line_width(m->raya, 10, 0);
    lv_obj_set_style_line_rounded(m->raya, true, 0);
    lv_obj_set_style_line_color(m->raya, lv_color_hex(C_TEXTO), 0);
    lv_obj_add_flag(m->raya, LV_OBJ_FLAG_HIDDEN);

#ifdef PRUEBA_AUTO
    printf("crear: dibujos ok\r\n");
    if (NULL == m->raya)
    {
        printf("crear: raya NULL\r\n");
    }
#endif

    lvgl_unlock();

    gestos_boton(m->g, ID_NUEVA, "OTRA PARTIDA", X_NUEVA, Y_NUEVA,
                 AN_NUEVA, AL_NUEVA, C_AMBER);
    gestos_boton(m->g, ID_SALIR, "X", X_SALIR, Y_SALIR,
                 AN_SALIR, AL_SALIR, C_AMBER);
}

/* ---- reglas --------------------------------------------------------------- */

/* Busca tres en raya y deja en idx las tres casillas de la linea.  Sin array
 * de lineas: tres filas, tres columnas y dos diagonales se comprueban a mano y
 * el binario no depende de una tabla en .rodata.  Devuelve 1 si hay linea. */
static int linea_ganadora(const int *b, int idx[3])
{
    int i;

    for (i = 0; i < 3; i++)
    {
        if (b[3 * i] && b[3 * i] == b[3 * i + 1] && b[3 * i] == b[3 * i + 2])
        {
            idx[0] = 3 * i;
            idx[1] = 3 * i + 1;
            idx[2] = 3 * i + 2;
            return 1;
        }
        if (b[i] && b[i] == b[i + 3] && b[i] == b[i + 6])
        {
            idx[0] = i;
            idx[1] = i + 3;
            idx[2] = i + 6;
            return 1;
        }
    }
    if (b[0] && b[0] == b[4] && b[0] == b[8])
    {
        idx[0] = 0;
        idx[1] = 4;
        idx[2] = 8;
        return 1;
    }
    if (b[2] && b[2] == b[4] && b[2] == b[6])
    {
        idx[0] = 2;
        idx[1] = 4;
        idx[2] = 6;
        return 1;
    }
    return 0;
}

static int lleno(const int *b)
{
    int i;

    for (i = 0; i < 9; i++)
    {
        if (0 == b[i])
        {
            return 0;
        }
    }
    return 1;
}

/* ---- minimax (apunte para el futuro, NO se compila) ----------------------
 *
 * La primera version era de un jugador contra la maquina (O).  Su turno usaba
 * minimax, y se deja aqui documentado por si algun dia se recupera el modo:
 *
 *   - Puntuacion de una posicion terminada, vista por la maquina:
 *       gana O .......... 10 - profundidad   (cuanto antes gane, mejor)
 *       gana X .......... profundidad - 10   (cuanto mas tarde pierda, mejor)
 *       tablas ........... 0
 *   - Con turno de O se MAXIMIZA la puntuacion; con turno de X se MINIMIZA.
 *     Esa pareja max/min es la que da nombre al algoritmo.
 *   - Se prueba cada casilla libre, se recursiona con el turno contrario y se
 *     deshace la jugada al volver.  Con 9 casillas hay 9! hojas como mucho, y
 *     en el P4 se recorren en decimas de segundo: no hace falta poda alfa-beta.
 *   - mejor_jugada() probaba las 9 respuestas de la maquina, se quedaba con la
 *     de mayor puntuacion y, entre las empatadas, elegia al azar con un LCG
 *     (multiplicador 1103515245, suma 12345).  Sin ese desempate la maquina
 *     contestaba siempre lo mismo y la partida era una foto fija.
 *
 * Para volver a activarlo haria falta: el estado `lcg`, las funciones
 * minimax()/mejor_jugada() y, en jugar(), el turno automatico de O.  Nada de
 * eso esta compilado ahora: juegan dos personas.
 * ------------------------------------------------------------------------- */

/* ---- pintura del estado --------------------------------------------------- */

static void poner_estado(struct juego *m, const char *txt, unsigned int rgb)
{
    lvgl_lock();
    lv_label_set_text(m->estado, txt);
    lv_obj_set_style_text_color(m->estado, lv_color_hex(rgb), 0);
    lvgl_unlock();
}

static void poner_marcador(struct juego *m)
{
    char buf[48];

    snprintf(buf, sizeof(buf), "X %d  O %d  EMPATES %d", m->vx, m->vo, m->ve);
    lvgl_lock();
    lv_label_set_text(m->marcador, buf);
    lvgl_unlock();
}

/* Centro de la casilla i en coordenadas de pantalla.  La raya es un objeto a
 * pantalla completa colocado en 0,0, asi que sus puntos son absolutos. */
static void centro_casilla(int i, int *x, int *y)
{
    *x = X0 + (i % 3) * PASO + CELDA / 2;
    *y = Y0 + (i / 3) * PASO + CELDA / 2;
}

/* Cruza con una raya las tres fichas de la linea ganadora.  Los extremos se
 * alargan 16 px (11 en cada eje si es diagonal, 16/sqrt(2) redondeado) para
 * que la raya rebase las fichas exteriores y no acabe en sus centros. */
static void pintar_raya(struct juego *m, const int *idx)
{
    int x1, y1, x2, y2;

    centro_casilla(idx[0], &x1, &y1);
    centro_casilla(idx[2], &x2, &y2);

    if (x1 == x2)
    {
        y1 -= 16;
        y2 += 16;
    }
    else if (y1 == y2)
    {
        x1 -= 16;
        x2 += 16;
    }
    else
    {
        x1 -= 11;
        y1 -= 11;
        x2 += 11;
        y2 += 11;
    }

    m->raya_pts[0].x = x1;
    m->raya_pts[0].y = y1;
    m->raya_pts[1].x = x2;
    m->raya_pts[1].y = y2;

    lvgl_lock();
    lv_line_set_points(m->raya, m->raya_pts, 2);
    lv_obj_clear_flag(m->raya, LV_OBJ_FLAG_HIDDEN);
    lvgl_unlock();
}

/* Descubre la ficha que acaba de poner `m->b[i]`. */
static void mostrar_ficha(struct juego *m, int i)
{
    lvgl_lock();
    if (FX == m->b[i])
    {
        lv_obj_clear_flag(m->linea[i][0], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(m->linea[i][1], LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_clear_flag(m->circulo[i], LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_unlock();
}

static void esconder_fichas(struct juego *m)
{
    int i;

    lvgl_lock();
    for (i = 0; i < 9; i++)
    {
        lv_obj_add_flag(m->linea[i][0], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(m->linea[i][1], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(m->circulo[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(m->raya, LV_OBJ_FLAG_HIDDEN);
    lvgl_unlock();
}

/* Cierra la partida si ya hay tres en raya o tablero lleno.  1 = terminada. */
static int cerrar(struct juego *m)
{
    int idx[3];

    if (linea_ganadora(m->b, idx))
    {
        int g = m->b[idx[0]];

        m->fin = g;
        pintar_raya(m, idx);
        if (FX == g)
        {
            m->vx = m->vx + 1;
            poner_estado(m, "GANA X", C_VERDE);
        }
        else
        {
            m->vo = m->vo + 1;
            poner_estado(m, "GANA O", C_VERDE);
        }
        poner_marcador(m);
        return 1;
    }
    if (lleno(m->b))
    {
        m->fin = 3;
        m->ve = m->ve + 1;
        poner_estado(m, "EMPATE", C_AMBER);
        poner_marcador(m);
        return 1;
    }
    return 0;
}

static void reiniciar(struct juego *m)
{
    memset(m->b, 0, sizeof(m->b));
    m->turno = FX;
    m->fin = 0;

    esconder_fichas(m);
    poner_estado(m, "TURNO X", C_X);
    poner_marcador(m);
}

/* ---- eventos -------------------------------------------------------------- */

static void jugar(struct juego *m, int i)
{
    if (m->fin || m->b[i])
    {
        return;
    }

    m->b[i] = m->turno;
    mostrar_ficha(m, i);
    if (cerrar(m))
    {
        return;
    }

    /* Dos jugadores en el mismo pad: el turno pasa al otro.  El rotulo lleva
     * el color de la ficha que toca para no tener que mirar el tablero. */
    if (FX == m->turno)
    {
        m->turno = FO;
        poner_estado(m, "TURNO O", C_O);
    }
    else
    {
        m->turno = FX;
        poner_estado(m, "TURNO X", C_X);
    }
}

#ifdef PRUEBA_AUTO
/* Volcado compacto del tablero para el ensayo por consola: filas separadas por
 * '/', 1=X, 2=O, 0=vacia.  No va en el binario de la app. */
static void volcar(struct juego *m)
{
    printf("auto: %d%d%d/%d%d%d/%d%d%d turno=%d fin=%d X=%d O=%d E=%d\r\n",
           m->b[0], m->b[1], m->b[2], m->b[3], m->b[4], m->b[5],
           m->b[6], m->b[7], m->b[8], m->turno, m->fin, m->vx, m->vo, m->ve);
}
#endif

static void procesar(struct juego *m, gev_t *ev, int *fuera)
{
    /* Solo interesan los botones: un toque en el fondo o un arrastre no mueven
     * ficha (el arrastre se sigue midiendo para no romper el gesto comun). */
    if (GE_BOTON != ev->tipo)
    {
        return;
    }

    if (ev->id >= ID_CELDA && ev->id < ID_CELDA + 9)
    {
        jugar(m, ev->id - ID_CELDA);
    }
    else if (ID_NUEVA == ev->id)
    {
        reiniciar(m);
    }
    else if (ID_SALIR == ev->id)
    {
        *fuera = 1;
    }

#ifdef PRUEBA_AUTO
    volcar(m);
#endif
}

/* ---- arranque ------------------------------------------------------------- */

static void salir(struct juego *m)
{
    gestos_cerrar(m->g);
    psram_free(m);
}

int main(int argc, char *argv[])
{
    struct juego *m;
    gev_t ev;
    int fuera = 0;
#ifdef PRUEBA_AUTO
    unsigned t_auto = millis();
#endif

    (void)argc;
    (void)argv;

    printf("tictac: arranca (tid=%d)\r\n", gettid());

    m = (struct juego *)psram_malloc(sizeof(struct juego));
    if (NULL == m)
    {
        printf("tictac: no hay PSRAM para el estado\r\n");
        exit(1);
    }
    memset(m, 0, sizeof(struct juego));

    /* La app pinta su propia cabecera: solo fondo, anillo y botones. */
    m->g = gestos_nueva_partes(0);
    if (NULL == m->g)
    {
        psram_free(m);
        exit(1);
    }
#ifdef PRUEBA_AUTO
    printf("tictac: gestos=%x estado=%x\r\n", (unsigned int)m->g,
           (unsigned int)m);
#endif

    crear(m);
    reiniciar(m);

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
#ifdef PRUEBA_AUTO
        /* Solo para verificar en placa sin dedo: cada 500 ms juega una casilla
         * libre al azar por el jugador que tenga el turno y reinicia al acabar.
         * Este bloque NO va en el binario de la app; se compila solo con
         * -DPRUEBA_AUTO. */
        if (!fuera && millis() - t_auto >= 500u)
        {
            t_auto = millis();
            if (m->fin)
            {
                /* Si gano alguien, dejar la raya en pantalla; si fue empate,
                 * empezar otra para seguir probando. */
                if (3 == m->fin)
                {
                    reiniciar(m);
                }
            }
            else
            {
                int libres[9], n = 0, k;

                for (k = 0; k < 9; k++)
                {
                    if (0 == m->b[k])
                    {
                        libres[n++] = k;
                    }
                }
                if (n > 0)
                {
                    gev_t falso;

                    /* Sin estado de azar: el reloj elige la casilla. */
                    memset(&falso, 0, sizeof(falso));
                    falso.tipo = GE_BOTON;
                    falso.id = ID_CELDA
                               + libres[(millis() >> 3) % (unsigned)n];
                    procesar(m, &falso, &fuera);
                }
            }
        }
#endif

        if (!fuera)
        {
            sleep(20);
        }
    }

    salir(m);               /* gestos_cerrar() y psram_free(): una sola vez */
    exit(0);
    return 0;
}
