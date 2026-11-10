/**
 * @file flappy.c
 *
 * Flappy Bird para el pad LCD del P4, sobre la libreria de gestos de la app.
 *
 *   elf /elf/flappy.elf
 *
 * Tres estados sobre un pad de 320x480:
 *
 *   - LISTO: el pájaro en la cola, el suelo pintado, el primer tramo y la nota
 *     de instruccion.  El primer toque arranca la partida.
 *   - CORRIENDO: gravedad + impulso (enteros, sin coma flotante), los tramos se
 *     mueven a la izquierda y se reciclan al salir por la izquierda, y la
 *     puntuacion se anota al pasar el pájaro por un tramo.
 *   - FIN: game over, con el puntuador.
 *
 * Entrada: toque o pulsacion larga = flap / arrancar / reiniciar; boton
 * ID_REINICIAR = arrancar; boton ID_SALIR = salir (mata la app).
 *
 * Disciplina, misma que la app de musica: el estado vive en PSRAM (struct +
 * objetos LVGL), todo lo que pinta va con lvgl_lock()/lvgl_unlock(), y el bucle
 * solo avanza la fisica cuando el estado es CORRIENDO.
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

#define ESTADO_LISTO     0
#define ESTADO_CORRIENDO 1
#define ESTADO_FIN       2

#define ANCHO      320
#define PISO_Y     440
#define PISO_H     40
#define AN_BIRD    14
#define Y_INICIO   200
/* Sensibilidad del vuelo: el impulso manda en lo alto que sube cada toque
 * (altura = V_FLAP^2 / (2 * V_GRAV)).  Con 380/1500 el salto era de ~48 px.
 * Se bajo a 300/1000 (~45 px) y aun parecia demasiado alto al jugar, asi que
 * el impulso queda en 260 con la misma gravedad: salto de ~34 px (25 % menos),
 * tiempo hasta el apice 0,26 s.  La caida topa en VY_MAX para que no caiga
 * como una piedra. */
#define V_FLAP     260
#define V_GRAV     1000
#define VY_MAX     420
/* Los tramos van mas separados y algo mas lentos: a 130 px/s y 70 px de paso
 * salia un tubo cada 0,54 s, imposible.  Ahora uno cada ~1,24 s. */
#define VELO_PIPE  105
#define PASO_PIPE  130
#define AN_PIPE    30
#define GAP        150
#define MIN_GAP    70
#define MAX_GAP    190      /* el pajaro (y=200) siempre cae dentro del 1er hueco */
#define BIRD_X     160
#define MAX_PIPE   3
#define ID_REINICIAR 1
#define ID_SALIR     2
#define X_SALIR     272
#define Y_SALIR     8
#define AN_SALIR    40
#define AL_SALIR    40
#define X_JUGAR     60
#define Y_JUGAR     330
#define AN_JUGAR    200
#define AL_JUGAR    50

/* colores, en hex #RRGGBB sobre RGB565 */
#define C_CIELO  0x4A90C4
#define C_PIPE   0x9BD14F
#define C_BIRD   0xFFC93C
#define C_ALA    0xD9990A
#define C_PICO   0xFF6B1A
#define C_PISO   0x8B5A2B
#define C_TEXTO  0xE8D8B0
#define C_ROJO   0xC0392B
#define C_AMBER  0xFFB302
#define C_CAJA   0x0E1A26

/* un tramo en vuelo: su posicion, el hueco y sus dos trozos de tubo */
struct flit
{
    int x;
    int gap_top;
    lv_obj_t *arriba;
    lv_obj_t *abajo;
    int anotado;
};

struct flappy
{
    gestos_t *g;
    int estado;
    int bird_y;
    int bird_vy;
    int puntos;
    struct flit f[MAX_PIPE];
    unsigned lcg;
    unsigned ultimo_tick;
    lv_obj_t *ojo;
    lv_obj_t *piso;
    lv_obj_t *score;
    lv_obj_t *fin;
    lv_obj_t *t_reiniciar;
    lv_obj_t *t_salir;
};

/* ---- objetos planos ------------------------------------------------------- */

/* Objeto sin borde, sin contorno, sin sombra, sin scroll y sin clickable: el
 * juego pinta su propia geometria y un objeto clicable se comeria el toque del
 * fondo, asi que aqui todo se despoja. */
static void candado(lv_obj_t *o)
{
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_outline_width(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
}

static void candado_color(lv_obj_t *o, unsigned int rgb)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(rgb), 0);
}

/* Pieza suelta dentro de un contenedor: rectangulo redondeado, ya despojado de
 * borde/sombra/scroll y no clicable.  Con esto se compone el pajaro sin
 * imagenes ni decodificador. */
static lv_obj_t *pieza(lv_obj_t *padre, int x, int y, int w, int h,
                       unsigned int rgb, int radio)
{
    lv_obj_t *o = lv_obj_create(padre);

    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    candado(o);
    lv_obj_set_style_radius(o, radio, 0);
    candado_color(o, rgb);
    return o;
}

/* ---- los tramos ----------------------------------------------------------- */

/* Reposiciona un tramo reciclado: nuevo hueco, posicion de salida a la derecha. */
static void spawn(struct flappy *m, int slot)
{
    struct flit *p = &m->f[slot];
    unsigned r = m->lcg = m->lcg * 1103515245u + 12345u;
    int top = MIN_GAP + (int)(r % (unsigned)(MAX_GAP - MIN_GAP));

    p->gap_top = top;
    p->anotado = 0;
}

/* ---- pintar ---------------------------------------------------------------- */

static void pintar(struct flappy *m)
{
    char txt[16];
    int i;

    snprintf(txt, sizeof(txt), "SCORE %d", m->puntos);
    lvgl_lock();
    lv_obj_set_pos(m->piso, 0, PISO_Y);
    lv_obj_set_pos(m->ojo, BIRD_X, m->bird_y);
    lv_label_set_text(m->score, txt);
    for (i = 0; i < MAX_PIPE; i++)
    {
        struct flit *p = &m->f[i];

        if (p->x + AN_PIPE < -4)
        {
            continue;
        }
        lv_obj_set_pos(p->arriba, p->x, 0);
        lv_obj_set_size(p->arriba, AN_PIPE, p->gap_top);
        lv_obj_set_pos(p->abajo, p->x, p->gap_top + GAP);
        lv_obj_set_size(p->abajo, AN_PIPE, PISO_Y - (p->gap_top + GAP));
    }
    /* La tecla JUGAR solo tiene sentido parado: en cuanto la partida corre se
     * esconde para que se vea que el juego arranco, y vuelve al hacer game over
     * o al reiniciar.  El rotulo GAME OVER se gobierna igual, porque si no se
     * queda encendido encima del juego tras el primer reinicio.  Se hace aqui,
     * dentro del candado que ya tenemos, para no anidar lvgl_lock() (no es
     * recursivo). */
    if (NULL != m->t_reiniciar)
    {
        if (ESTADO_CORRIENDO == m->estado)
        {
            lv_obj_add_flag(m->t_reiniciar, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_clear_flag(m->t_reiniciar, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (NULL != m->fin)
    {
        if (ESTADO_FIN == m->estado)
        {
            lv_obj_clear_flag(m->fin, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(m->fin, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lvgl_unlock();
}

/* ---- game over ------------------------------------------------------------ */

static void fin(struct flappy *m)
{
#ifdef PRUEBA_AUTO
    printf("auto: FIN puntos=%d y=%d\r\n", m->puntos, m->bird_y);
#endif
    m->estado = ESTADO_FIN;
    /* La nota de la libreria vive en y=250, justo donde cae GAME OVER: se quita
     * para que los dos rotulos no se pisen.  pintar() enseña el GAME OVER en
     * cuanto ve el estado FIN (no se toca aqui para no anidar lvgl_lock). */
    gestos_nota(m->g, "");
}

/* ---- reiniciar / flap ----------------------------------------------------- */

static void reiniciar(struct flappy *m)
{
    int i;

    m->estado = ESTADO_LISTO;
    m->bird_y = Y_INICIO;
    m->bird_vy = 0;
    m->puntos = 0;
    m->lcg = 1;
    for (i = 0; i < MAX_PIPE; i++)
    {
        struct flit *p = &m->f[i];

        p->x = ANCHO + 30 + i * PASO_PIPE;
        spawn(m, i);
    }
    gestos_nota(m->g, "TOQUE PARA VOLAR");
}

static void dar_flap(struct flappy *m)
{
    if (ESTADO_FIN == m->estado)
    {
        /* Tras el game over, JUGAR/tocar rearranca de una vez: reiniciar deja
         * el pajaro en la cola y acto seguido se le da el primer flap. */
        reiniciar(m);
    }
    /* Al arrancar se retira la nota de instruccion para que no quede encima
     * del juego; reiniciar() la vuelve a poner. */
    if (ESTADO_LISTO == m->estado)
    {
        gestos_nota(m->g, "");
    }
    m->estado = ESTADO_CORRIENDO;
    m->bird_vy = -V_FLAP;
}

/* ---- fisica y repintado -------------------------------------------------- */

static void tick(struct flappy *m)
{
    unsigned ahora = millis();
    unsigned dt = ahora - m->ultimo_tick;
    int i;

    m->ultimo_tick = ahora;
    if (dt > 40)
    {
        dt = 40;
    }

    if (ESTADO_CORRIENDO == m->estado)
    {
        m->bird_vy = m->bird_vy + (V_GRAV * (int)dt) / 1000;
        if (m->bird_vy > VY_MAX)
        {
            m->bird_vy = VY_MAX;
        }
        m->bird_y = m->bird_y + (m->bird_vy * (int)dt) / 1000;
        if (m->bird_y < 0)
        {
            m->bird_y = 0;
        }

        for (i = 0; i < MAX_PIPE; i++)
        {
            struct flit *p = &m->f[i];

            if (p->x + AN_PIPE < -4)
            {
                p->x = ANCHO + 30 + i * PASO_PIPE;
                spawn(m, i);
                continue;
            }
            p->x = p->x - ((VELO_PIPE * (int)dt) / 1000);
            if (!p->anotado && (BIRD_X + AN_BIRD) > (p->x + AN_PIPE))
            {
                p->anotado = 1;
                m->puntos = m->puntos + 1;
            }
        }

        if (m->bird_y + AN_BIRD >= PISO_Y)
        {
            fin(m);
            return;
        }
        for (i = 0; i < MAX_PIPE; i++)
        {
            struct flit *p = &m->f[i];

            if (p->x + AN_PIPE < -4)
            {
                continue;
            }
            if ((BIRD_X + AN_BIRD) > p->x && (BIRD_X) < (p->x + AN_PIPE))
            {
                if ((m->bird_y) < (p->gap_top) || ((m->bird_y) + AN_BIRD) > (p->gap_top + GAP))
                {
                    fin(m);
                    return;
                }
            }
        }
    }

    pintar(m);
}

/* ---- los objetos ---------------------------------------------------------- */

static void crear(struct flappy *m)
{
    lv_obj_t *raiz = gestos_raiz(m->g);
    int i;

    lvgl_lock();

    lv_obj_set_style_bg_color(raiz, lv_color_hex(C_CIELO), 0);
    lv_obj_set_style_bg_grad_dir(raiz, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(raiz, LV_OPA_COVER, 0);

    /* Los tubos primero: todo lo que se cree despues queda por encima.  Antes
     * el marcador y el pajaro se creaban antes y un tubo que pasaba por la
     * esquina tapaba el SCORE. */
    for (i = 0; i < MAX_PIPE; i++)
    {
        struct flit *p = &m->f[i];

        p->x = ANCHO + 30 + i * PASO_PIPE;
        p->gap_top = MIN_GAP;
        p->anotado = 0;
        p->arriba = lv_obj_create(raiz);
        lv_obj_set_size(p->arriba, AN_PIPE, p->gap_top);
        lv_obj_align(p->arriba, LV_ALIGN_TOP_LEFT, p->x, 0);
        candado(p->arriba);
        candado_color(p->arriba, C_PIPE);
        p->abajo = lv_obj_create(raiz);
        lv_obj_set_size(p->abajo, AN_PIPE, PISO_Y - (p->gap_top + GAP));
        lv_obj_align(p->abajo, LV_ALIGN_TOP_LEFT, p->x, p->gap_top + GAP);
        candado(p->abajo);
        candado_color(p->abajo, C_PIPE);
        spawn(m, i);
    }

    m->piso = lv_obj_create(raiz);
    lv_obj_set_size(m->piso, ANCHO, PISO_H);
    lv_obj_align(m->piso, LV_ALIGN_TOP_LEFT, 0, PISO_Y);
    candado(m->piso);
    candado_color(m->piso, C_PISO);

    /* El pajaro es un contenedor transparente que se mueve entero; dentro van
     * cuerpo, ala, pico, ojo y pupila, rectangulos redondeados.  El contenedor
     * no lleva fondo ni relleno para que las piezas caigan en sus coordenadas. */
    m->ojo = lv_obj_create(raiz);
    lv_obj_set_size(m->ojo, 25, 16);
    lv_obj_align(m->ojo, LV_ALIGN_TOP_LEFT, BIRD_X, m->bird_y);
    lv_obj_set_style_bg_opa(m->ojo, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m->ojo, 0, 0);
    lv_obj_set_style_radius(m->ojo, 0, 0);
    lv_obj_set_style_outline_width(m->ojo, 0, 0);
    lv_obj_set_style_shadow_width(m->ojo, 0, 0);
    lv_obj_set_style_pad_all(m->ojo, 0, 0);
    lv_obj_clear_flag(m->ojo, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(m->ojo, LV_OBJ_FLAG_CLICKABLE);
    pieza(m->ojo, 0, 2, 20, 13, C_BIRD, 6);       /* cuerpo */
    pieza(m->ojo, 3, 6, 8, 6, C_ALA, 3);          /* ala    */
    pieza(m->ojo, 19, 6, 5, 4, C_PICO, 2);        /* pico   */
    pieza(m->ojo, 12, 3, 5, 5, 0xFFFFFF, 3);      /* ojo    */
    pieza(m->ojo, 14, 4, 2, 2, 0x101820, 1);      /* pupila */

    /* El marcador lleva su propia caja: la etiqueta es de ancho automatico
     * (LV_SIZE_CONTENT) y LVGL suma el relleno al tamaño del texto, asi la caja
     * crece con el numero (SCORE 0, SCORE 12...) y el texto no se sale nunca.
     * Antes era una caja fija de 112 px y con dos cifras ya se tocaba el borde.
     * El fondo va aqui y no en un objeto aparte para que crezcan juntos. */
    m->score = lv_label_create(raiz);
    lv_obj_clear_flag(m->score, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(m->score, lv_color_hex(C_TEXTO), 0);
    lv_obj_set_style_bg_color(m->score, lv_color_hex(C_CAJA), 0);
    lv_obj_set_style_bg_opa(m->score, 170, 0);
    lv_obj_set_style_radius(m->score, 6, 0);
    lv_obj_set_style_pad_all(m->score, 5, 0);
    /* Mas aire a los lados que arriba/abajo: con el texto de 108 px y solo
     * 5 px por lado el "0" quedaba pegado al borde. 10 px por lado dejan la
     * caja en ~128 px sin subir la altura. */
    lv_obj_set_style_pad_left(m->score, 10, 0);
    lv_obj_set_style_pad_right(m->score, 10, 0);
    lv_obj_align(m->score, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_label_set_text(m->score, "SCORE 0");

    m->fin = lv_label_create(raiz);
    lv_obj_clear_flag(m->fin, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(m->fin, lv_color_hex(C_ROJO), 0);
    lv_obj_align(m->fin, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(m->fin, "GAME OVER");
    lv_obj_add_flag(m->fin, LV_OBJ_FLAG_HIDDEN);

    lvgl_unlock();
}

/* ---- los botones ---------------------------------------------------------- */

static void botones(struct flappy *m)
{
    m->t_reiniciar = gestos_boton(m->g, ID_REINICIAR, "JUGAR", X_JUGAR, Y_JUGAR,
                                 AN_JUGAR, AL_JUGAR, C_AMBER);
    m->t_salir = gestos_boton(m->g, ID_SALIR, "X", X_SALIR, Y_SALIR,
                              AN_SALIR, AL_SALIR, C_AMBER);
}

/* ---- salida --------------------------------------------------------------- */

static void salir(struct flappy *m)
{
    gestos_cerrar(m->g);
    psram_free(m);
}

/* ---- eventos -------------------------------------------------------------- */

static void procesar(struct flappy *m, gev_t *ev, int *fuera)
{
#ifdef PRUEBA_AUTO
    printf("ev: tipo=%d dir=%d id=%d x=%d y=%d\r\n", ev->tipo, ev->dir, ev->id,
           ev->x1, ev->y1);
#endif
    if (GE_ARRASTRE == ev->tipo)
    {
        return;
    }
    if (GE_TOQUE == ev->tipo || GE_LARGA == ev->tipo)
    {
        dar_flap(m);
        return;
    }
    if (GE_BOTON != ev->tipo)
    {
        return;
    }
    if (ID_SALIR == ev->id)
    {
        *fuera = 1;
        return;
    }
    if (ID_REINICIAR == ev->id)
    {
        /* El boton JUGAR arranca la partida (o la reinicia desde el game over):
         * en LISTO hace dar_flap, que arranca; en FIN, que reinicia. */
        dar_flap(m);
    }
}

int main(int argc, char *argv[])
{
    struct flappy *m;
    gev_t ev;
    int fuera = 0;
#ifdef PRUEBA_AUTO
    unsigned t_auto = millis();
#endif

    (void)argc;
    (void)argv;

    printf("flappy: arranca (tid=%d)\r\n", gettid());

    m = (struct flappy *)psram_malloc(sizeof(struct flappy));
    if (NULL == m)
    {
        printf("flappy: no hay PSRAM para el estado\r\n");
        exit(1);
    }
    memset(m, 0, sizeof(struct flappy));
    m->ultimo_tick = millis();

    m->g = gestos_nueva_partes(GE_NOTA);
    if (NULL == m->g)
    {
        psram_free(m);
        exit(1);
    }

    /* Estado de partida ANTES de crear los objetos: sin esto el pajaro nace en
     * y=0 (el memset de arriba) en vez de en Y_INICIO, y al pulsar JUGAR cae
     * desde el borde superior hasta el suelo en poco mas de un segundo: parece
     * que el juego "no hace nada" porque muere nada mas empezar.  reiniciar()
     * deja el pajaro en la cola, los tres tramos colocados y la semilla del
     * generador lista. */
    reiniciar(m);

    crear(m);
    botones(m);
    /* La nota es lo unico que usa este juego; la pastilla y la flecha sobran. */
    gestos_nota(m->g, "TOQUE PARA VOLAR");

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
        /* Solo para verificar en placa sin dedo: cada 600 ms pulsa JUGAR la
         * primera vez y despues da un flap, que es lo mismo que hace el toque.
         * Este bloque NO va en el binario de la app: se compila solo con
         * -DPRUEBA_AUTO para el ensayo. */
        if (millis() - t_auto >= 600u)
        {
            t_auto = millis();
            if (ESTADO_LISTO == m->estado)
            {
                gev_t falso;

                memset(&falso, 0, sizeof(falso));
                falso.tipo = GE_BOTON;
                falso.id = ID_REINICIAR;
                printf("auto: pulsar JUGAR\r\n");
                procesar(m, &falso, &fuera);
            }
            else
            {
                dar_flap(m);
            }
        }
#endif

        if (ESTADO_CORRIENDO == m->estado)
        {
            unsigned ahora = millis();

            if (ahora - m->ultimo_tick >= 30u)
            {
                /* NO tocar m->ultimo_tick aqui: tick() mide dt como
                 * millis() - m->ultimo_tick y lo actualiza el mismo.  Si el
                 * bucle lo pisaba antes de llamar, dt salia 0 en cada vuelta,
                 * la gravedad no movia al pajaro y los tubos no avanzaban:
                 * el juego arrancaba pero se quedaba congelado. */
                tick(m);
            }
        }
        else if (ESTADO_LISTO == m->estado || ESTADO_FIN == m->estado)
        {
            pintar(m);
        }

        sleep(30);
    }

    salir(m);           /* gestos_cerrar() y psram_free(m); no volver a liberar */
    exit(0);
    return 0;
}
