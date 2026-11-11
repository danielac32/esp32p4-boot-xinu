/**
 * @file grabador.c
 *
 * Grabadora de voz vintage para el P4 (puerto LVGL del vintage de rec.c).
 *
 *   elf /apps/grabador/app.elf
 *
 *   - GRABAR ... arranca la captura; la tecla pasa a PARAR (roja).
 *   - PARAR ... corta la grabacion y reproduce lo tomado automaticamente.
 *     Durante la reproduccion la misma tecla vuelve a decir PARAR y corta.
 *   - REPETIR ... reproduce la ultima toma (si la hay).
 *   - swipe hacia arriba desde el borde inferior: corta lo que este grabando
 *     o sonando; si esta en reposo, devuelve al launcher.
 *     (Prohibido el boton X; la salida se hace con el gesto.)
 *
 * La captura arranca en cuanto se pulsa GRABAR y guarda desde el primer
 * momento, como rec.c y p4demoTask: no se descarta un tramo inicial para no
 * perder el comienzo de la voz.  El ADC del ES8311 puede tardar unos ms en
 * asentar su ganancia (fundido), pero comer audio del usuario es peor.
 *
 * Interfaz: panel vintage de cinta de cassette con pantalla de osciloscopio
 * (canvas) y carretes girando mientras hay señal.  Se repinta cada ~70 ms y
 * solo el canvas/objetos que cambian.
 *
 * Tres hebras.  La de main solo pinta y reparte eventos; "grabcap" captura y
 * "grabrep" reproduce.  Las dos operaciones bloquean contra el reloj del I2S
 * --write() espera hueco en el anillo y AUDIO_CTRL_STREAM_WAIT espera al
 * drenaje--, asi que hechas en main dejaban el boton muerto mientras duraran.
 * Ninguna de las dos toca LVGL: solo publican len/pos y un aviso de fin, y main
 * es el unico que pinta y el unico que llama a exit().
 *
 * La duplicacion del mono ya no se hace sobre la toma entera.  El micro entra
 * por el slot izquierdo de cada trama estereo; el pase que copiaba el slot
 * derecho byte a byte sobre la ventana directa de PSRAM (0x88......, sin
 * burst) iba a ~152.000 B/s frente a los 192.000 que consume el I2S: para una
 * toma de 20 s eran mas de doce segundos de boton PARAR congelado.  Ahora la
 * toma se guarda tal cual y el slot derecho se rellena por palabras, de un
 * chunk cada vez, sobre un marco en la pila mientras se reproduce.  PARAR no
 * deja ningun trabajo pendiente.
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
#include <audio.h>
#include <math.h>
#include "gestos.h"

void exit(int codigo);

LV_FONT_DECLARE(lv_font_unscii_8);

/* ---- constantes ---------------------------------------------------------- */

#define ID_GRABAR  10
#define ID_REPETIR 11

#define AUDIO_DEV   "/dev/audio0"
#define REC_CHUNK   4096U
#define BYTES_MS    192U
#define MAX_SEC     20U
#define CAPACITY    (BYTES_MS * 1000U * MAX_SEC)
/* (ya no se descarta un tramo inicial: se graba desde el primer instante,
 * como en rec.c/p4demoTask, para no perder el comienzo de la voz). */
#define VA_VOLUMEN 75U
#define VA_TEST_MS 1500U

/* Pila de las trabajadoras: la de reproduccion lleva el marco de 4 KB del
 * chunk encima, y las dos llaman a printf/control.  Salen de L2MEM (SRAM), no
 * de PSRAM, que es lo que hace barato el marco. */
#define CAP_STK 12288
#define REP_STK 16384

#define SC_W 296
#define SC_H 96
#define RE_W 272
#define RE_H 64
#define SCOPE_N 296                    /* un punto por pixel               */
#define SCOPE_STEP 16                  /* 16 B = 4 tramas, slot izquierdo  */

#define C_FONDO   0x1B120B
#define C_FONDO2  0x0D0805
#define C_PANEL   0x3A2C20
#define C_PANEL2  0x171009
#define C_PAPER   0xD8C8A8
#define C_INK     0x100A06
#define C_CREAM   0xE8D8B0
#define C_AMBER   0xFFB302
#define C_ROJO    0xE74C3C
#define C_VERDE   0x2ECC71
#define C_GRIS    0x8B7E6A
#define C_CYAN    0x2EC0E4
#define C_SCOPEBG 0x030F07
#define C_SCOPE_GRID 0x0E2B18
#define C_SCOPE_DIM  0x0F5A28
#define C_SCOPE_HI   0x3DFF70

/* ST_PARANDO y ST_CORTANDO son los estados en los que main ya pidio el corte
 * y espera el aviso de la trabajadora: el boton ya volvio a GRABAR y verde, y
 * las pulsaciones se ignoran hasta que la hebra cierre el device. */
enum estado
{
    ST_LISTO,
    ST_GRABANDO,
    ST_PARANDO,
    ST_REPRODUCIENDO,
    ST_CORTANDO
};

struct grabadora
{
    ui_input_t *g;
    int audio;
    uint8_t *buf;                      /* toma PCM por la ventana directa 0x88 */
    uint8_t *buf_base;                 /* base cacheada, solo para liberar  */
    uint8_t *sbuf;                     /* buffer del canvas del osciloscopio */
    uint8_t *rbuf;                     /* buffer del canvas de los carretes  */
    lv_obj_t *estado_lbl;
    lv_obj_t *tiempo_lbl;
    lv_obj_t *rec_dot;
    lv_obj_t *scope;
    lv_obj_t *reels;
    lv_obj_t *pista;
    lv_obj_t *progreso;
    lv_obj_t *btn_grabar;
    lv_obj_t *btn_repetir;
    int estado;                        /* solo lo escribe main             */
    int autoplay;                      /* solo lo escribe main             */
    int slot;
    uint32_t t_ui;                     /* ultima vez que se pinto          */
    unsigned reel_ang;                 /* angulo actual de los carretes    */
    tid_typ main_tid;
    tid_typ cap_tid;
    tid_typ rep_tid;
    /* Puente entre hebras.  main pide con cap_cmd/rep_cmd y corta con
     * cap_stop/rep_stop; las trabajadoras contestan con cap_done/rep_done y
     * publican len/pos.  cap_fin/rep_fin solo se ponen al morir la hebra: es
     * lo que main espera antes de liberar nada, porque el codigo que esas
     * hebras ejecutan vive dentro de esta misma imagen ELF. */
    volatile int quit;
    volatile int cap_cmd;
    volatile int cap_stop;
    volatile int rep_cmd;
    volatile int rep_stop;
    volatile int cap_fin;
    volatile int rep_fin;
    volatile int cap_done;
    volatile int rep_done;
    volatile int fallo;
    /* len y pos los deja a cero main al pedir la operacion, y a partir de ahi
     * solo los escribe la trabajadora: main no los toca con la hebra viva. */
    volatile uint32_t len;             /* bytes de toma: los crece grabcap */
    volatile uint32_t pos;             /* bytes sonados: los crece grabrep */
    volatile uint32_t cap_max;         /* CAPACITY, o lo que pida el test  */
};

/* ---- formatos ------------------------------------------------------------ */

static void mm_ss(char *out, size_t cap, uint32_t ms)
{
    uint32_t s = ms / 1000U;

    snprintf(out, cap, "%02u:%02u", (unsigned)(s / 60U), (unsigned)(s % 60U));
}

static lv_obj_t *rotulo(lv_obj_t *raiz, const char *txt, int x, int y,
                        unsigned rgb)
{
    lv_obj_t *l = lv_label_create(raiz);

    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(l, lv_color_hex(rgb), 0);
    lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, y);
    lv_label_set_text(l, txt);
    return l;
}

/* ---- osciloscopio -------------------------------------------------------- */

/* Dibuja la traza del osciloscopio con los ultimos SCOPE_N*SCOPE_STEP bytes
 * anteriores a @p fin: cada punto es el maximo absoluto de las 4 muestras del
 * slot izquierdo, sobre un fondo verde oscuro con rejilla.  Brillo doble:
 * traza gruesa y oscura debajo, traza fina y brillante encima.
 *
 * @p fin es g->len al grabar y g->pos al reproducir, y lo pasa main: la toma
 * ya no lleva el slot derecho relleno, asi que aqui se salta de 4 en 4 para
 * no pintar como señal lo que es una copia del canal izquierdo. */
static void scope_pintar(struct grabadora *g, uint32_t fin)
{
    lv_draw_line_dsc_t dsc;
    lv_point_t pts[SCOPE_N];
    uint32_t base = 0;
    int i;
    int y;

    if (NULL == g->scope)
    {
        return;
    }
    if (fin > (uint32_t)(SCOPE_N * SCOPE_STEP))
    {
        base = fin - (uint32_t)(SCOPE_N * SCOPE_STEP);
    }

    lvgl_lock();
    lv_canvas_fill_bg(g->scope, lv_color_hex(C_SCOPEBG), LV_OPA_COVER);

    /* rejilla: 3 horizontales, 3 verticales */
    {
        lv_draw_rect_dsc_t rd;

        lv_draw_rect_dsc_init(&rd);
        rd.radius = 0;
        rd.border_width = 0;
        rd.shadow_width = 0;
        rd.bg_opa = LV_OPA_COVER;
        rd.bg_color = lv_color_hex(C_SCOPE_GRID);
        lv_canvas_draw_rect(g->scope, 0, SC_H / 4,     SC_W, 1, &rd);
        lv_canvas_draw_rect(g->scope, 0, SC_H / 2,     SC_W, 1, &rd);
        lv_canvas_draw_rect(g->scope, 0, (3 * SC_H) / 4, SC_W, 1, &rd);
        lv_canvas_draw_rect(g->scope, SC_W / 4,     0, 1, SC_H, &rd);
        lv_canvas_draw_rect(g->scope, SC_W / 2,     0, 1, SC_H, &rd);
        lv_canvas_draw_rect(g->scope, (3 * SC_W) / 4, 0, 1, SC_H, &rd);
    }

    /* puntos de la traza */
    for (i = 0; i < SCOPE_N; i++)
    {
        uint32_t off = base + (uint32_t)i * SCOPE_STEP;
        uint32_t pico = 0;
        int k;

        for (k = 0; k < SCOPE_STEP - 1 && off + (uint32_t)k + 1U < fin; k += 4)
        {
            int16_t s = (int16_t)((uint16_t)g->buf[off + (uint32_t)k] |
                         ((uint16_t)g->buf[off + (uint32_t)k + 1U] << 8));
            uint32_t a = (s < 0) ? (uint32_t)(-s) : (uint32_t)s;

            if (a > pico) pico = a;
        }
        if (off >= fin)
        {
            pico = 0;                  /* fuera del buffer: linea plana   */
        }

        pts[i].x = (lv_coord_t)i;
        y = (SC_H / 2) - (int)((pico * (SC_H / 2)) / 32768U);
        if (y < 2) y = 2;
        if (y > SC_H - 3) y = SC_H - 3;
        pts[i].y = (lv_coord_t)y;
    }

    memset(&dsc, 0, sizeof(dsc));
    dsc.opa = LV_OPA_COVER;

    /* brillo tenue horizontal para el efecto fosforo */
    dsc.color = lv_color_hex(C_SCOPE_DIM);
    dsc.width = 3;
    lv_canvas_draw_line(g->scope, pts, SCOPE_N, &dsc);

    dsc.color = lv_color_hex(C_SCOPE_HI);
    dsc.width = 1;
    lv_canvas_draw_line(g->scope, pts, SCOPE_N, &dsc);
    lvgl_unlock();
}

/* Circulo relleno como scanline: una fila de rects por fila.  La ruta de
 * poligonos de LVGL cae en lv_draw_mask_apply y revienta en este port; los
 * rects nunca pasan por ahi (probados: el tablero del Tetris se pinta asi).
 * La misma tecnica para el aro, el papel y el hub. */
static void circulo(lv_obj_t *cv, int cx, int cy, int r, int npts,
                    unsigned rgb, lv_opa_t opa)
{
    lv_draw_rect_dsc_t dsc;
    int dy;

    (void)npts;
    if (r <= 0)
    {
        return;
    }

    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = 0;
    dsc.border_width = 0;
    dsc.shadow_width = 0;
    dsc.bg_opa = opa;
    dsc.bg_color = lv_color_hex(rgb);

    /* Paso 2: la mitad de rects y se ve igual a este tamaño; reduce el
     * tiempo bajo lvgl_lock para no congelar la atencion de los botones. */
    for (dy = -r; dy <= r; dy += 2)
    {
        int dx = (int)sqrtf((float)(r * r - dy * dy));

        lv_canvas_draw_rect(cv, cx - dx, cy + dy, 2 * dx + 1, 2, &dsc);
    }
}

/* Dos carretes girando con la cinta entre ellos.  El angulo avanza por el
 * tiempo, como rotaria el eje de una grabadora de cinta de verdad. */
static void reels_pintar(struct grabadora *g, int girando)
{
    lv_draw_rect_dsc_t rd;
    lv_draw_line_dsc_t dsc;
    lv_point_t p[2];
    int cx1 = 56, cx2 = 216, cy = RE_H / 2;
    int r_out = 22, r_in = 14, r_hub = 4;
    unsigned a = g->reel_ang;
    int i, k;

    if (girando)
    {
        /* ~0,35 rev/s: avanza un paso fijo por pintado */
        a = a + 13U;
        g->reel_ang = a;
    }

    lvgl_lock();
    lv_canvas_fill_bg(g->reels, lv_color_hex(C_PANEL2), LV_OPA_COVER);

    lv_draw_rect_dsc_init(&rd);
    rd.radius = 3;
    rd.border_width = 0;
    rd.shadow_width = 0;
    rd.bg_opa = LV_OPA_COVER;

    /* cinta entre los dos carretes */
    rd.bg_color = lv_color_hex(0x100A06);
    lv_canvas_draw_rect(g->reels, cx1, cy - 3, cx2 - cx1, 6, &rd);

    memset(&dsc, 0, sizeof(dsc));
    dsc.opa = LV_OPA_COVER;
    dsc.color = lv_color_hex(0x2A1F16);
    dsc.width = 3;

    for (k = 0; k < 2; k++)
    {
        int cx = (0 == k) ? cx1 : cx2;
        unsigned ang = a + (unsigned)k * 40U;

        /* cuerpo del carrete: aro oscuro + papel crema */
            circulo(g->reels, cx, cy, r_out, 20, C_PANEL, LV_OPA_COVER);
            circulo(g->reels, cx, cy, r_in, 16, C_PAPER, LV_OPA_COVER);

        /* tres radios (los huecos de una cinta de verdad) */
        for (i = 0; i < 3; i++)
        {
            float th = (float)(ang + (unsigned)i * 120U) * 0.0174533f;
            float s, c;

            s = sin(th);
            c = cos(th);
            p[0].x = (lv_coord_t)cx;
            p[0].y = (lv_coord_t)cy;
            p[1].x = (lv_coord_t)(cx + (int)(s * (float)(r_in - 1)));
            p[1].y = (lv_coord_t)(cy - (int)(c * (float)(r_in - 1)));
                    lv_canvas_draw_line(g->reels, p, 2, &dsc);
        }

        /* hub central */
            circulo(g->reels, cx, cy, r_hub, 10, C_INK, LV_OPA_COVER);
    }

    lvgl_unlock();
}

/* ---- pintores ------------------------------------------------------------ */

static void progreso_pintar(struct grabadora *g, uint32_t bytes)
{
    int w = (int)(bytes * 288U / CAPACITY);

    if (w > 288) w = 288;
    if (w < 0) w = 0;

    lvgl_lock();
    lv_obj_set_width(g->progreso, w);
    lvgl_unlock();
}

static void tiempo_pintar(struct grabadora *g, uint32_t bytes)
{
    char t1[16], t2[16], linea[40];

    mm_ss(t1, sizeof(t1), bytes / BYTES_MS);
    mm_ss(t2, sizeof(t2), MAX_SEC * 1000U);
    snprintf(linea, sizeof(linea), "%s / %s", t1, t2);

    lvgl_lock();
    lv_label_set_text(g->tiempo_lbl, linea);
    lvgl_unlock();
}

static void estado_pintar(struct grabadora *g, const char *txt, unsigned rgb)
{
    lvgl_lock();
    lv_label_set_text(g->estado_lbl, txt);
    lv_obj_set_style_text_color(g->estado_lbl, lv_color_hex(rgb), 0);
    lvgl_unlock();
}

static void rec_pintar(struct grabadora *g, int grabando)
{
    lvgl_lock();
    lv_obj_set_style_bg_color(g->rec_dot,
        lv_color_hex(grabando ? C_ROJO : 0x2A1F16), 0);
    lvgl_unlock();
}

static void boton_grabar(struct grabadora *g, const char *txt)
{
    int activo = (ST_GRABANDO == g->estado || ST_REPRODUCIENDO == g->estado);

    lvgl_lock();
    if (NULL != g->btn_grabar)
    {
        lv_obj_set_style_bg_color(g->btn_grabar,
            lv_color_hex(activo ? C_ROJO : C_VERDE), 0);
    }
    lvgl_unlock();
    ui_button_set_label(g->g, ID_GRABAR, txt);
}

/* ---- audio --------------------------------------------------------------- */

/* Captura, en la hebra "grabcap".  No toca LVGL ni el estado de la app: solo
 * llena g->buf, publica g->len y levanta cap_done al cerrar el micro.  main
 * repinta mirando g->len mientras esto corre.
 *
 * Igual que rec.c y p4demoTask: se empieza a guardar desde el primer momento
 * (sin tirar un tramo inicial), para no perder el comienzo de la voz.  El
 * fundido de ganancia del ADC del ES8311 puede dejar mudos los primeros ms,
 * pero es preferible a comer el arranque de quien habla. */
static void capturar(struct grabadora *g)
{
    long r;

    r = control(g->audio, AUDIO_CTRL_IN, 1, 0);
    printf("grabador: AUDIO_CTRL_IN=1 -> %ld\r\n", r);
    if (SYSERR == r)
    {
        g->fallo = 1;
        g->cap_done = 1;
        return;
    }

    while (!g->quit && !g->cap_stop && g->len < g->cap_max)
    {
        uint32_t chunk = g->cap_max - g->len;
        int got;

        if (chunk > REC_CHUNK) chunk = REC_CHUNK;
        got = (int)read(g->audio, g->buf + g->len, chunk);
        if (got > 0)
        {
            g->len += (uint32_t)got;
        }
        else
        {
            sleep(1);
        }
    }

    r = control(g->audio, AUDIO_CTRL_IN, 0, 0);
    printf("grabador: AUDIO_CTRL_IN=0 -> %ld\r\n", r);
    g->len -= g->len % 4U;
    g->cap_done = 1;
}

/* Reproduccion, en la hebra "grabrep".  write() bloquea hasta que el I2S deja
 * hueco en el anillo, asi que esto no puede correr en main.  Tampoco toca
 * LVGL: publica g->pos y rep_done. */
static void reproducir(struct grabadora *g)
{
    uint32_t marco[REC_CHUNK / 4U];    /* 4 KB en la pila, que es L2MEM */
    uint32_t total = g->len;
    uint32_t off = 0;
    int corte;
    long r;

    g->pos = 0;
    if (0U == total)
    {
        g->rep_done = 1;
        return;
    }

    (void)control(g->audio, AUDIO_CTRL_VOLUME, VA_VOLUMEN, 0);
    r = control(g->audio, AUDIO_CTRL_STREAM, 1, 0);
    printf("grabador: AUDIO_CTRL_STREAM=1 -> %ld\r\n", r);
    if (SYSERR == r)
    {
        g->fallo = 1;
        g->rep_done = 1;
        return;
    }

    while (!g->quit && !g->rep_stop && off < total)
    {
        uint32_t chunk = total - off;
        uint32_t frames, i;
        int written;

        if (chunk > REC_CHUNK) chunk = REC_CHUNK;
        chunk &= ~3U;
        if (0U == chunk)
        {
            break;
        }
        frames = chunk / 4U;

        /* El micro entra solo por el slot izquierdo: aqui se copia al derecho,
         * de palabra en palabra y sobre el marco de la pila.  Antes esto se
         * hacia sobre la toma entera en la ventana directa de PSRAM y a byte,
         * que es la forma mas lenta de mover memoria en este port: mas de doce
         * segundos para una toma llena, todos ellos con el boton muerto.
         * Repartido por chunks cuesta una lectura por trama, diluida en el
         * tiempo real que tarda en sonar. */
        for (i = 0; i < frames; i++)
        {
            uint32_t v = ((const uint32_t *)(g->buf + off))[i];
            uint32_t l = v & 0xFFFFU;

            marco[i] = l | (l << 16);
        }

        written = (int)write(g->audio, marco, chunk);
        if (written <= 0 || (uint32_t)written > chunk)
        {
            break;
        }
        off += (uint32_t)written;      /* write() devuelve tramas enteras */
        g->pos = off;
    }

    corte = g->quit || g->rep_stop || off < total;
    /* El drenaje solo si la toma acabo entera: tras un corte el STREAM_WAIT
     * se comeria otro tercio de segundo con el boton ya en GRABAR. */
    if (!corte)
    {
        (void)control(g->audio, AUDIO_CTRL_STREAM_WAIT, 0, 0);
    }
    (void)control(g->audio, AUDIO_CTRL_STREAM, 0, 0);
    g->pos = off;
    g->rep_done = 1;
}

static int cap_task(struct grabadora *g)
{
    while (!g->quit)
    {
        if (g->cap_cmd)
        {
            g->cap_cmd = 0;
            capturar(g);
        }
        if (!g->quit)
        {
            sleep(10);
        }
    }
    g->cap_fin = 1;
    send(g->main_tid, 1);
    return 0;
}

static int rep_task(struct grabadora *g)
{
    while (!g->quit)
    {
        if (g->rep_cmd)
        {
            g->rep_cmd = 0;
            reproducir(g);
        }
        if (!g->quit)
        {
            sleep(10);
        }
    }
    g->rep_fin = 1;
    send(g->main_tid, 1);
    return 0;
}

/* Pide la reproduccion de la ultima toma.  Solo main llama a esto, y solo
 * desde ST_LISTO: el boton se pone rojo y PARAR antes de que la hebra haya
 * escrito una sola muestra. */
static void arrancar_repro(struct grabadora *g)
{
    if (0U == g->len)
    {
        estado_pintar(g, "Toma vacia", C_ROJO);
        return;
    }

    g->estado = ST_REPRODUCIENDO;
    g->pos = 0;
    g->rep_stop = 0;
    g->rep_done = 0;
    g->fallo = 0;
    g->rep_cmd = 1;
    g->t_ui = (uint32_t)millis();
    estado_pintar(g, "REPRODUCIENDO...", C_CYAN);
    boton_grabar(g, "PARAR");
    rec_pintar(g, 0);
    progreso_pintar(g, 0);
    tiempo_pintar(g, 0);
}

/* Corta la reproduccion y espera a que la hebra desmonte el stream (puede
 * tardar lo que queda del write() en curso, ~21 ms).  Hasta rep_done no se
 * acepta otra orden, asi que el estado se muestra como transitorio. */
static void cortar_repro(struct grabadora *g)
{
    g->rep_stop = 1;
    g->estado = ST_CORTANDO;
    estado_pintar(g, "PARANDO...", C_AMBER);
}

/* ---- interfaz ------------------------------------------------------------ */

static void crear(struct grabadora *g)
{
    lv_obj_t *raiz = ui_root(g->g);

    lvgl_lock();

    /* Fondo: perla vintage con degradado calido */
    lv_obj_set_style_bg_color(raiz, lv_color_hex(C_FONDO), 0);
    lv_obj_set_style_bg_grad_color(raiz, lv_color_hex(C_FONDO2), 0);
    lv_obj_set_style_bg_grad_dir(raiz, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(raiz, LV_OPA_COVER, 0);

    rotulo(raiz, "GRABADOR", 10, 6, C_AMBER);
    g->rec_dot = lv_obj_create(raiz);
    lv_obj_set_size(g->rec_dot, 12, 12);
    lv_obj_set_pos(g->rec_dot, 102, 6);
    lv_obj_clear_flag(g->rec_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(g->rec_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(g->rec_dot, lv_color_hex(0x2A1F16), 0);
    lv_obj_set_style_border_width(g->rec_dot, 0, 0);
    lv_obj_set_style_radius(g->rec_dot, LV_RADIUS_CIRCLE, 0);

    g->estado_lbl = rotulo(raiz, "LISTO", 10, 26, C_VERDE);
    g->tiempo_lbl = rotulo(raiz, "00:00 / 00:20", 180, 26, C_CREAM);

    /* Osciloscopio */
    g->scope = lv_canvas_create(raiz);
    lv_canvas_set_buffer(g->scope, g->sbuf, SC_W, SC_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(g->scope, 12, 42);
    lv_canvas_fill_bg(g->scope, lv_color_hex(C_SCOPEBG), LV_OPA_COVER);

    /* Carretes */
    g->reels = lv_canvas_create(raiz);
    lv_canvas_set_buffer(g->reels, g->rbuf, RE_W, RE_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(g->reels, 24, 146);

    /* Barra de progreso */
    g->pista = lv_obj_create(raiz);
    lv_obj_set_size(g->pista, 292, 10);
    lv_obj_set_pos(g->pista, 14, 224);
    lv_obj_clear_flag(g->pista, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(g->pista, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(g->pista, lv_color_hex(C_PANEL2), 0);
    lv_obj_set_style_border_width(g->pista, 1, 0);
    lv_obj_set_style_border_color(g->pista, lv_color_hex(C_GRIS), 0);

    g->progreso = lv_obj_create(g->pista);
    lv_obj_set_height(g->progreso, 8);
    lv_obj_set_width(g->progreso, 0);
    lv_obj_clear_flag(g->progreso, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(g->progreso, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(g->progreso, lv_color_hex(C_AMBER), 0);
    lv_obj_set_style_border_width(g->progreso, 0, 0);
    lv_obj_set_style_radius(g->progreso, 0, 0);
    lv_obj_align(g->progreso, LV_ALIGN_LEFT_MID, 0, 0);

    lvgl_unlock();

    /* Mandos */
    g->btn_grabar = ui_button(g->g, ID_GRABAR, "GRABAR", 10, 336, 140, 72,
                              C_VERDE);
    g->btn_repetir = ui_button(g->g, ID_REPETIR, "REPETIR", 162, 336, 148, 72,
                               C_CYAN);

    /* Retroalimentacion de pulsacion (oscurecer al pulsar) */
    lvgl_lock();
    lv_obj_set_style_bg_color(g->btn_grabar, lv_color_hex(0x1E8449), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(g->btn_repetir, lv_color_hex(0x1A7F9E), LV_STATE_PRESSED);
    lvgl_unlock();
}

static void ir_fondo(struct grabadora *g, int *fuera)
{
    ui_event_t ev;
    int s;

    if (g->slot < 0)
    {
        return;
    }
    ui_show(g->g, 0);
    appmgr_set_state(g->slot, APP_BACKGROUND);
    while (!*fuera)
    {
        s = appmgr_get_state(g->slot);
        if (APP_RUNNING == s)
        {
            ui_show(g->g, 1);
            return;
        }
        if (APP_CLOSING == s || APP_FREE == s)
        {
            *fuera = 1;
            return;
        }
        while (ui_poll(g->g, &ev))
        {
            ;
        }
        sleep(50);
    }
}

static void salir(struct grabadora *g)
{
    if (g->slot >= 0)
    {
        appmgr_unregister(g->slot);
    }

    /* Las trabajadoras ejecutan codigo que vive dentro de esta imagen ELF y
     * tocan g y g->buf: hay que esperarlas antes de soltar nada.  Se espera por
     * cap_fin/rep_fin, que solo se ponen al terminar el bucle de la hebra, y no
     * por un flag de "viva" que main pudiera ver a cero antes de que la hebra
     * recien creada llegara a arrancar. */
    g->quit = 1;
    if (SYSERR != g->cap_tid)
    {
        while (!g->cap_fin) sleep(20);
    }
    if (SYSERR != g->rep_tid)
    {
        while (!g->rep_fin) sleep(20);
    }
    recvclr();                         /* los send() de despedida */

    control(g->audio, AUDIO_CTRL_IN, 0, 0);
    control(g->audio, AUDIO_CTRL_STREAM, 0, 0);
    ui_destroy(g->g);
    if (g->rbuf != NULL) psram_free(g->rbuf);
    if (g->sbuf != NULL) psram_free(g->sbuf);
    if (g->buf_base != NULL) psram_free(g->buf_base);
    psram_free(g);
}

static void procesar(struct grabadora *g, ui_event_t *ev, int *fuera)
{
    if (UI_EVENT_SWIPE == ev->tipo)
    {
        if (UI_EDGE_BOTTOM == ev->edge && UI_DIR_UP == ev->dir)
        {
            /* Con el micro abierto no se manda la app a segundo plano: se
             * cierra la captura y el siguiente gesto ya sale. */
            if (ST_GRABANDO == g->estado)
            {
                g->cap_stop = 1;
                g->estado = ST_PARANDO;
                g->autoplay = 0;
                estado_pintar(g, "PARANDO...", C_AMBER);
                rec_pintar(g, 0);
            }
            else if (ST_REPRODUCIENDO == g->estado)
            {
                cortar_repro(g);
            }
            else if (ST_LISTO == g->estado)
            {
                ir_fondo(g, fuera);
            }
        }
        return;
    }
    if (UI_EVENT_BUTTON != ev->tipo) return;

    if (ID_GRABAR == ev->id)
    {
        if (ST_LISTO == g->estado)
        {
            g->estado = ST_GRABANDO;
            g->autoplay = 0;
            g->len = 0;                /* antes de pedirla: main repinta ya */
            g->pos = 0;
            g->cap_stop = 0;
            g->cap_done = 0;
            g->fallo = 0;
            g->cap_max = CAPACITY;
            g->cap_cmd = 1;
            g->t_ui = (uint32_t)millis();
            estado_pintar(g, "GRABANDO...", C_ROJO);
            boton_grabar(g, "PARAR");
            rec_pintar(g, 1);
            progreso_pintar(g, 0);
            tiempo_pintar(g, 0);
            scope_pintar(g, 0);
        }
        else if (ST_GRABANDO == g->estado)
        {
            /* Se pide parar y se espera a que la hebra cierre el micro; en
             * cap_done main reproduce la toma (autoplay).  No se finge LISTO
             * antes de tiempo: hasta cap_done el boton no acepta otra orden. */
            g->cap_stop = 1;
            g->estado = ST_PARANDO;
            g->autoplay = 1;
            estado_pintar(g, "PARANDO...", C_AMBER);
            rec_pintar(g, 0);
        }
        else if (ST_REPRODUCIENDO == g->estado)
        {
            cortar_repro(g);
        }
        return;
    }

    if (ID_REPETIR == ev->id)
    {
        if (ST_LISTO == g->estado)
        {
            arrancar_repro(g);
        }
        return;
    }
}

/* Autoprueba por consola: `elf /apps/grabador/app.elf test`.
 * Graba VA_TEST_MS del micro y lo reproduce con las mismas hebras que usa la
 * pantalla, sin pintar nada: sirve para validar el camino de audio (arranque
 * del ADC, descarte del fundido, nivel de captura y escritura al altavoz)
 * cuando no hay dedos a mano. */
static void autoprueba(struct grabadora *g)
{
    uint32_t bytes;
    uint32_t i;
    uint32_t media;
    int pico = 0;
    unsigned long rms = 0;

    g->cap_max = BYTES_MS * VA_TEST_MS;
    g->len = 0;
    g->cap_stop = 0;
    g->cap_done = 0;
    g->fallo = 0;
    g->cap_cmd = 1;
    while (!g->cap_done) sleep(10);
    g->cap_done = 0;

    if (g->fallo)
    {
        g->fallo = 0;
        printf("grabador: test SIN MICRO (AUDIO_CTRL_IN=1 fallo)\r\n");
        return;
    }

    bytes = g->len;
    /* Solo el slot izquierdo: la toma ya no lleva el derecho duplicado.  El
     * acumulador va en unsigned long porque VA_TEST_MS son 72000 muestras y
     * con un int se desbordaba antes de acabar la cuenta. */
    for (i = 0U; i + 3U < bytes; i += 4U)
    {
        int s = (int)(int16_t)((uint16_t)g->buf[i] |
                               ((uint16_t)g->buf[i + 1U] << 8));

        if (s < 0)
        {
            s = -s;
        }
        if (s > pico)
        {
            pico = s;
        }
        rms += (unsigned long)s;
    }

    media = (bytes >= 4U) ? (uint32_t)(rms / (bytes / 4U)) : 0U;
    printf("grabador: test %u ms -> %u bytes, pico %d (%d%%), media %u (%u%%)\r\n",
           (unsigned)VA_TEST_MS, (unsigned)bytes, pico, (pico * 100) / 32767,
           (unsigned)media, (unsigned)((media * 100U) / 32767U));

    if (0U == bytes)
    {
        printf("grabador: test NO CAPTURO NADA\r\n");
        return;
    }

    printf("grabador: test reproduciendo...\r\n");
    g->rep_stop = 0;
    g->rep_done = 0;
    g->rep_cmd = 1;
    while (!g->rep_done) sleep(20);
    g->rep_done = 0;
    printf("grabador: test FIN\r\n");
}

int main(int argc, char *argv[])
{
    struct grabadora *g;
    ui_event_t ev;
    int fuera = 0;
    int modo_test = 0;

    if (argc > 1 && NULL != argv[1] && 0 == strcmp(argv[1], "test"))
    {
        modo_test = 1;
    }

    printf("grabador: arranca (tid=%d)%s\r\n", gettid(),
           modo_test ? " [test]" : "");

    g = (struct grabadora *)psram_malloc(sizeof(struct grabadora));
    if (NULL == g) { printf("grabador: sin PSRAM\r\n"); exit(1); }
    memset(g, 0, sizeof(struct grabadora));
    g->slot = SYSERR;
    /* SYSERR, no 0: el tid 0 es la hebra nula y salir() se quedaria esperando
     * un fin que nunca llega si se llama antes de crear las trabajadoras. */
    g->cap_tid = SYSERR;
    g->rep_tid = SYSERR;
    g->cap_max = CAPACITY;

    g->audio = getdev((char *)AUDIO_DEV);
    if (SYSERR == g->audio)
    {
        printf("grabador: no existe %s\r\n", AUDIO_DEV);
        psram_free(g);
        exit(1);
    }

    g->buf_base = (uint8_t *)psram_malloc(CAPACITY);
    if (NULL == g->buf_base)
    {
        printf("grabador: sin PSRAM para la toma\r\n");
        psram_free(g);
        exit(1);
    }
    /* La toma se lee y se reproduce por la ventana directa 0x88......, igual
     * que `audio rec`/`loop` de la shell: por la via cacheada (0x48......) el
     * PCM llega a ceros en este port (la misma trampa del audio de E4).  El
     * puntero cacheado se guarda solo para devolverlo al heap al salir. */
    g->buf = (uint8_t *)((uintptr_t)g->buf_base + 0x40000000U);

    g->sbuf = psram_malloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(SC_W, SC_H));
    g->rbuf = psram_malloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(RE_W, RE_H));
    if (NULL == g->sbuf || NULL == g->rbuf)
    {
        printf("grabador: sin PSRAM para los canvas\r\n");
        psram_free(g->sbuf);
        psram_free(g->rbuf);
        psram_free(g->buf_base);
        psram_free(g);
        exit(1);
    }
    memset(g->sbuf, 0, LV_CANVAS_BUF_SIZE_TRUE_COLOR(SC_W, SC_H));
    memset(g->rbuf, 0, LV_CANVAS_BUF_SIZE_TRUE_COLOR(RE_W, RE_H));

    g->g = ui_create_ex(UI_CHROME_NONE);
    if (NULL == g->g)
    {
        salir(g);
        exit(1);
    }

    g->slot = appmgr_register("Grabador", "/apps/grabador/app.elf", 0xFFB302,
                              gettid());

    crear(g);
    scope_pintar(g, 0);
    reels_pintar(g, 0);
    tiempo_pintar(g, 0);

    g->main_tid = gettid();
    g->cap_tid = create((void *)cap_task, CAP_STK, getprio(gettid()),
                        "grabcap", 1, (long)g);
    if (SYSERR == g->cap_tid)
    {
        printf("grabador: sin hebra de captura\r\n");
        salir(g);
        exit(1);
    }
    ready(g->cap_tid, RESCHED_NO);

    g->rep_tid = create((void *)rep_task, REP_STK, getprio(gettid()),
                        "grabrep", 1, (long)g);
    if (SYSERR == g->rep_tid)
    {
        printf("grabador: sin hebra de reproduccion\r\n");
        salir(g);
        exit(1);
    }
    ready(g->rep_tid, RESCHED_NO);

    if (modo_test)
    {
        autoprueba(g);
        salir(g);
        exit(0);
    }

    while (!fuera)
    {
        while (ui_poll(g->g, &ev))
        {
            procesar(g, &ev, &fuera);
            if (fuera) break;
        }

        /* Fin de captura: el micro ya esta cerrado y len es definitivo. */
        if (g->cap_done)
        {
            uint32_t bytes = g->len;
            int hubo_fallo = g->fallo;
            int suena;

            g->cap_done = 0;
            g->fallo = 0;
            g->estado = ST_LISTO;
            rec_pintar(g, 0);
            boton_grabar(g, "GRABAR");
            tiempo_pintar(g, bytes);
            progreso_pintar(g, bytes);
            scope_pintar(g, bytes);

            if (hubo_fallo)
            {
                estado_pintar(g, "No inicia el micro", C_ROJO);
            }
            else if (0U == bytes)
            {
                estado_pintar(g, "Toma vacia", C_AMBER);
            }
            else if (bytes >= g->cap_max)
            {
                estado_pintar(g, "Tope de 20 s", C_AMBER);
            }
            else
            {
                estado_pintar(g, "LISTO", C_VERDE);
            }

            /* La toma se reproduce sola tanto al pulsar PARAR (autoplay) como
             * al llegar al tope de los 20 s, que es lo que hacia antes. */
            suena = (g->autoplay || bytes >= g->cap_max) &&
                    !hubo_fallo && bytes > 0U && !fuera;
            g->autoplay = 0;
            if (suena)
            {
                arrancar_repro(g);
            }
        }

        /* Fin de reproduccion: entera o cortada, el stream ya esta desmontado. */
        if (g->rep_done)
        {
            int hubo_fallo = g->fallo;

            g->rep_done = 0;
            g->fallo = 0;
            g->estado = ST_LISTO;
            boton_grabar(g, "GRABAR");
            if (hubo_fallo)
            {
                estado_pintar(g, "No inicia el audio", C_ROJO);
            }
            else
            {
                estado_pintar(g, "LISTO", C_VERDE);
            }
            tiempo_pintar(g, g->len);
            progreso_pintar(g, g->len);
            reels_pintar(g, 0);
        }

        if (!fuera &&
            (ST_GRABANDO == g->estado || ST_REPRODUCIENDO == g->estado))
        {
            uint32_t ahora = (uint32_t)millis();

            if (ahora - g->t_ui >= 200U)
            {
                uint32_t fin = (ST_GRABANDO == g->estado) ? g->len : g->pos;

                g->t_ui = ahora;
                scope_pintar(g, fin);
                reels_pintar(g, 1);
                tiempo_pintar(g, fin);
                progreso_pintar(g, fin);
            }
        }

        if (!fuera && g->slot >= 0 &&
            APP_CLOSING == appmgr_get_state(g->slot))
        {
            fuera = 1;
        }
        if (!fuera) sleep(10);
    }

    salir(g);
    exit(0);
    return 0;
}
