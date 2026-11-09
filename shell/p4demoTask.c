/**
 * xsh_p4demo.c
 *
 * Embedded Xinu / LCD vertical 320 x 480.
 *
 * Ejecutar UNA sola instancia:
 *
 * ready(create(p4demoTask, INITSTK, INITPRIO,
 *              "p4demo", 0), RESCHED_YES);
 *
 * No ejecutar dashTask ni p4monitorTask simultaneamente.
 *
 * Barra superior:
 * - Icono WiFi de tres arcos y un punto.
 * - Desconectado: gris con raya.
 * - Asociado sin RSSI: gris.
 * - Senal conocida: rosa, amarillo o verde.
 *
 * Conserva las APIs del kernel original.
 * El RPC de RSSI sigue dependiendo de wifiSignalRead().
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <kernel.h>
#include <clock.h>
#include <device.h>
#include <lcd.h>
#include <touch.h>
#include <power.h>
#include <audio.h>
#include <memory.h>
#include <fsutil.h>
#include <timer.h>
#include <esp_hosted_ctrl.h>

/* Implementadas en tu kernel, como en el archivo original. */
int wifiSignalRead(int32_t *rssi, uint32_t *channel);
int wifiSignalQuality(int32_t rssi);

/* ---------- Colores ---------- */

#define P_BG        RGB565(16,20,29)
#define P_CARD      RGB565(29,35,47)
#define P_TRACK     RGB565(47,55,71)
#define P_TEXT      RGB565(237,240,249)
#define P_MUTED     RGB565(159,171,192)
#define P_BLUE      RGB565(173,198,255)
#define P_GREEN     RGB565(156,224,184)
#define P_PINK      RGB565(255,179,201)
#define P_YELLOW    RGB565(244,218,147)
#define P_PURPLE    RGB565(211,188,255)
#define P_CYAN      RGB565(151,222,235)
#define P_INK       RGB565(29,37,54)
#define P_REC_BG    RGB565(91,38,49)
#define P_VOICE_BG  RGB565(48,66,100)
#define P_WIFI_DIM  RGB565(65,77,96)

/* ---------- Tiempo ---------- */

#define P_POINTS        5U
#define P_POLL          15U
#define P_BATT_MS       1000U
#define P_WIFI_MS       15000U
#define P_WIFI_TRIES    3U
#define P_RELEASE_MISS  3U
#define P_IDLE_MS       120000U

/* ---------- Audio ---------- */

#define REC_PCM_DIRECT  0x40000000U
#define REC_BYTES_MS    192U
#define REC_MAX_MS      6000U
#define REC_CAPACITY    (REC_BYTES_MS * REC_MAX_MS)
#define REC_CHUNK       4096U
#define REC_FULLSCALE   32767U
#define REC_METER_MS    40U
#define REC_TOUCH_MS    15U
#define REC_STALL_MS    600U

#define V_BTN_X         40
#define V_BTN_Y         230
#define V_BTN_W         240
#define V_BTN_H         120

#define V_METER_X       40
#define V_METER_Y       174
#define V_METER_W       240
#define V_METER_H       12

/* ---------- Archivos ---------- */

#define FM_ROWS         5U
#define FM_LIST_Y       156
#define FM_ROW_H        38

/*
 * WiFi ocupa x=159..195, y=14..41.
 * Bateria empieza en x=206: no se superponen.
 */
#define P_WIFI_BOX_X    159
#define P_WIFI_BOX_Y    14
#define P_WIFI_BOX_W    37
#define P_WIFI_BOX_H    28
#define P_WIFI_CX       177
#define P_WIFI_CY       37

enum p_app {
    P_HOME = 0,
    P_VOICE,
    P_MUSIC,
    P_NOTES,
    P_WEATHER,
    P_SETTINGS,
    P_FILES
};

enum voice_state {
    VOICE_IDLE = 0,
    VOICE_RECORD,
    VOICE_PLAY
};

static int p_lcd;
static int p_touch;
static int p_power;
static int p_audio;

static enum p_app p_page = P_HOME;

static const char *p_names[] = {
    "Inicio", "Voz", "Musica", "Notas",
    "Tiempo", "Ajustes", "Archivos"
};

static const uint16_t p_colors[] = {
    P_BLUE, P_PINK, P_PURPLE, P_YELLOW,
    P_CYAN, P_GREEN, P_BLUE
};

static const uint16_t p_accents[] = {
    P_BLUE, P_GREEN, P_PINK
};

static int p_theme;
static bool p_playing;
static bool p_switch;
static int p_note;
static int p_volume;

static long p_battery;
static uint32_t p_last_battery;

/* WiFi */

static int p_signal;
static bool p_link;
static uint32_t p_last_wifi_ms;
static int p_wifi_shown = -100;
static unsigned p_wifi_tries;

/* Reposo y touch */

static uint32_t p_last_touch_ms;
static bool p_screen_asleep;
static bool p_ignore_touch;
static unsigned p_ignore_release;

/* Voz */

static uint8_t *v_cached;
static uint8_t *v_buffer;
static enum voice_state v_state;
static int v_meter_width;
static uint16_t v_meter_color;
static char v_last_hint[48];
static const char *v_error;

/* FAT */

static char fm_path[FS_PATH_MAX];
static fl_dirent fm_entries[FM_ROWS];
static unsigned fm_offset;
static unsigned fm_count;
static bool fm_more;
static bool fm_ready;
static int fm_selected;
static bool fm_confirm;
static char fm_delete_path[FS_PATH_MAX];
static char fm_status[80];

/* Declaraciones para refrescar indicadores al despertar. */
static void p_draw_battery(void);
static void p_draw_signal(void);

/* ---------- Reloj y reposo ---------- */

/*
 * Requiere que millis() del kernel avance en milisegundos.
 * La resta uint32_t admite el rollover normal del contador.
 */
static uint32_t p_now(void)
{
    return (uint32_t)millis();
}

static void p_idle_poll(void)
{
    uint32_t now;
    uint32_t elapsed;
    long result;

    if (p_screen_asleep)
        return;

    now = p_now();
    elapsed = (uint32_t)(now - p_last_touch_ms);

    if (elapsed < P_IDLE_MS)
        return;

    printf(
        "p4demo: intento OFF now=%lu last=%lu "
        "elapsed=%lu limite=%lu\n",
        (unsigned long)now,
        (unsigned long)p_last_touch_ms,
        (unsigned long)elapsed,
        (unsigned long)P_IDLE_MS
    );

    result = control(p_lcd, LCD_CTRL_BACKLIGHT, 0, 0);

    if (result != SYSERR) {
        p_screen_asleep = true;
        printf("p4demo: pantalla apagada por inactividad\n");
    } else {
        printf("p4demo: fallo al apagar backlight\n");
    }
}

static int p_read_touch(touch_point_t *points)
{
    int count = (int)read(p_touch, points, P_POINTS);
    uint32_t now = p_now();

    if (count > (int)P_POINTS)
        count = SYSERR;

    if (count > 0) {
        p_last_touch_ms = now;
        p_ignore_release = 0U;

        if (p_screen_asleep) {
            p_ignore_touch = true;

            printf(
                "p4demo: despertar por touch now=%lu\n",
                (unsigned long)now
            );

            if (control(
                    p_lcd, LCD_CTRL_BACKLIGHT, 1, 0
                ) != SYSERR) {
                p_screen_asleep = false;

                p_draw_signal();
                p_draw_battery();

                printf(
                    "p4demo: pantalla encendida por touch\n"
                );
            } else {
                printf(
                    "p4demo: fallo al encender backlight\n"
                );
            }
        }
    } else if (count == 0 && p_ignore_touch) {
        if (++p_ignore_release >= P_RELEASE_MISS) {
            p_ignore_touch = false;
            p_ignore_release = 0U;
        }
    } else if (count < 0) {
        p_ignore_release = 0U;
    }

    p_idle_poll();
    return count;
}

/* ---------- Dibujo ---------- */

static void p_rect(
    int x, int y, int w, int h, uint16_t color
)
{
    struct lcd_rect r;

    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }

    if (x >= LCD_W || y >= LCD_H)
        return;

    if (w > LCD_W - x)
        w = LCD_W - x;
    if (h > LCD_H - y)
        h = LCD_H - y;
    if (w <= 0 || h <= 0)
        return;

    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    r.color = color;

    control(p_lcd, LCD_CTRL_RECT, (long)&r, 0);
}

static void p_round(
    int x, int y, int w, int h,
    int radius, uint16_t color
)
{
    int row;

    if (w <= 0 || h <= 0)
        return;

    if (radius > w / 2)
        radius = w / 2;
    if (radius > h / 2)
        radius = h / 2;

    if (radius <= 0) {
        p_rect(x, y, w, h, color);
        return;
    }

    p_rect(x, y + radius, w, h - 2 * radius, color);

    for (row = 0; row < radius; ++row) {
        int dy = radius - row - 1;
        int dx = 0;
        int inset;

        while (
            (dx + 1) * (dx + 1) + dy * dy
            <= radius * radius
        )
            ++dx;

        inset = radius - dx;

        p_rect(
            x + inset, y + row,
            w - 2 * inset, 1, color
        );
        p_rect(
            x + inset, y + h - row - 1,
            w - 2 * inset, 1, color
        );
    }
}

static void p_text(
    const char *s, int x, int y,
    uint16_t fg, uint16_t bg
)
{
    struct lcd_text t;

    t.s = s;
    t.x = x;
    t.y = y;
    t.fg = fg;
    t.bg = bg;

    control(p_lcd, LCD_CTRL_TEXT, (long)&t, 0);
}

static void p_center(
    const char *s, int y, uint16_t fg, uint16_t bg
)
{
    int x = (LCD_W - (int)strlen(s) * LCD_TEXT_W) / 2;

    if (x < 0)
        x = 0;

    p_text(s, x, y, fg, bg);
}

static bool p_inside(
    int x, int y, int bx, int by, int bw, int bh
)
{
    return x >= bx && x < bx + bw
        && y >= by && y < by + bh;
}

static void p_button(
    int x, int y, int w,
    const char *label, uint16_t color
)
{
    int tx = x + (w - (int)strlen(label) * LCD_TEXT_W) / 2;

    p_round(x, y, w, 44, 18, color);
    p_text(label, tx, y + 14, P_INK, color);
}

/* ---------- Iconos ---------- */

static void p_triangle(int cx, int cy, uint16_t color)
{
    int i;

    for (i = 0; i < 22; ++i) {
        int half = (21 - i) / 2;
        p_rect(cx - 8 + i, cy - half,
               1, 2 * half + 1, color);
    }
}

static void p_mic(
    int cx, int y, uint16_t fg, uint16_t bg
)
{
    p_round(cx - 14, y + 10, 28, 25, 12, fg);
    p_round(cx - 11, y + 7, 22, 25, 10, bg);
    p_round(cx - 7, y, 14, 25, 7, fg);
    p_rect(cx - 2, y + 34, 4, 7, fg);
    p_round(cx - 11, y + 40, 22, 3, 1, fg);
}

static void p_icon(enum p_app app, int x, int y)
{
    uint16_t bg = p_colors[app];
    uint16_t fg = P_INK;

    p_round(x, y, 64, 64, 20, bg);

    switch (app) {
    case P_VOICE:
        p_mic(x + 32, y + 10, fg, bg);
        break;

    case P_MUSIC:
        p_rect(x + 26, y + 16, 5, 28, fg);
        p_rect(x + 43, y + 13, 5, 26, fg);
        p_rect(x + 26, y + 13, 22, 6, fg);
        p_round(x + 16, y + 37, 15, 11, 5, fg);
        p_round(x + 33, y + 32, 15, 11, 5, fg);
        break;

    case P_NOTES:
        p_round(x + 17, y + 12, 31, 41, 5, fg);
        p_rect(x + 23, y + 22, 19, 3, bg);
        p_rect(x + 23, y + 31, 19, 3, bg);
        p_rect(x + 23, y + 40, 13, 3, bg);
        break;

    case P_WEATHER:
        p_round(x + 13, y + 13, 26, 26, 13, fg);
        p_round(x + 25, y + 29, 28, 20, 10, bg);
        p_round(x + 18, y + 35, 36, 14, 7, fg);
        p_round(x + 29, y + 27, 18, 20, 9, fg);
        break;

    case P_SETTINGS:
        p_rect(x + 15, y + 20, 35, 3, fg);
        p_rect(x + 15, y + 31, 35, 3, fg);
        p_rect(x + 15, y + 42, 35, 3, fg);
        p_round(x + 23, y + 16, 9, 11, 4, fg);
        p_round(x + 37, y + 27, 9, 11, 4, fg);
        p_round(x + 20, y + 38, 9, 11, 4, fg);
        break;

    case P_FILES:
        p_round(x + 11, y + 17, 21, 12, 4, fg);
        p_round(x + 11, y + 24, 43, 27, 5, fg);
        p_rect(x + 16, y + 30, 33, 3, bg);
        break;

    default:
        break;
    }
}

/* ---------- Bateria ---------- */

static void p_draw_battery(void)
{
    char text[12];
    int fill;
    uint16_t color;

    p_rect(206, 16, 100, 25, P_BG);

    if (p_battery < 0)
        snprintf(text, sizeof(text), "--");
    else
        snprintf(text, sizeof(text), "%ld%%", p_battery);

    p_text(text, 206, 21, P_MUTED, P_BG);

    color = (p_battery >= 0 && p_battery <= 20)
        ? P_PINK : P_GREEN;

    p_round(253, 21, 39, 17, 4, P_MUTED);
    p_round(255, 23, 35, 13, 2, P_BG);
    p_rect(292, 26, 3, 7, P_MUTED);

    fill = p_battery < 0
        ? 0 : (int)(p_battery * 31L / 100L);

    if (fill > 0)
        p_rect(257, 25, fill, 9, color);
}

static void p_update_battery(void)
{
    long value = -1;

    if (p_power != SYSERR) {
        value = control(p_power, POWER_CTRL_PERCENT, 0, 0);

        if (value < 0 || value > 100)
            value = -1;
    }

    if (value != p_battery) {
        p_battery = value;

        if (!p_screen_asleep)
            p_draw_battery();
    }
}

static void p_poll_battery(uint32_t now)
{
    if ((uint32_t)(now - p_last_battery) < P_BATT_MS)
        return;

    p_last_battery = now;

    if (!p_screen_asleep)
        p_update_battery();
}

/* ---------- WiFi: icono de arcos ---------- */

/*
 * Estados:
 * -2: desconectado.
 * -1: conectado, RSSI desconocido.
 *  1: senal debil, punto.
 *  2: punto + arco interior.
 *  3: punto + dos arcos.
 *  4: punto + tres arcos.
 */
static int p_wifi_state(void)
{
    if (!p_link)
        return -2;
    if (p_signal < 0)
        return -1;
    if (p_signal < 25)
        return 1;
    if (p_signal < 50)
        return 2;
    if (p_signal < 75)
        return 3;

    return 4;
}

/*
 * Arco superior de 90 grados, grosor 2 pixeles.
 *
 * Se dibuja por tramos horizontales.
 * Sin float, sqrt, fuentes ni imagenes externas.
 */
static void p_wifi_arc(
    int cx, int cy, int radius, uint16_t color
)
{
    int dy;
    int inner = radius - 2;
    int outer2 = radius * radius;
    int inner2 = inner * inner;

    for (dy = -radius; dy <= 0; ++dy) {
        int dx;
        int start = -radius;

        for (dx = -radius; dx <= radius + 1; ++dx) {
            bool paint = false;

            if (dx <= radius) {
                int distance = dx * dx + dy * dy;
                int ax = dx < 0 ? -dx : dx;

                paint = distance <= outer2
                    && distance >= inner2
                    && ax <= -dy;
            }

            if (paint) {
                if (start == -radius)
                    start = dx;
            } else if (start != -radius) {
                p_rect(
                    cx + start, cy + dy,
                    dx - start, 1, color
                );
                start = -radius;
            }
        }
    }
}

static void p_draw_signal(void)
{
    int state = p_wifi_state();
    int level = state > 0 ? state : 0;
    uint16_t active;
    uint16_t inactive;
    int i;

    static const int radii[3] = {8, 14, 20};

    p_rect(
        P_WIFI_BOX_X, P_WIFI_BOX_Y,
        P_WIFI_BOX_W, P_WIFI_BOX_H, P_BG
    );

    if (level >= 3)
        active = P_GREEN;
    else if (level == 2)
        active = P_YELLOW;
    else
        active = P_PINK;

    inactive = state == -1 ? P_MUTED : P_WIFI_DIM;

    for (i = 0; i < 3; ++i) {
        bool enabled = level >= i + 2;

        p_wifi_arc(
            P_WIFI_CX, P_WIFI_CY, radii[i],
            enabled ? active : inactive
        );
    }

    /* Punto inferior de 4 x 4, con esquinas recortadas. */
    {
        uint16_t dot = level > 0 ? active : inactive;

        p_rect(P_WIFI_CX - 1, P_WIFI_CY - 2, 2, 1, dot);
        p_rect(P_WIFI_CX - 2, P_WIFI_CY - 1, 4, 2, dot);
        p_rect(P_WIFI_CX - 1, P_WIFI_CY + 1, 2, 1, dot);
    }

    if (state == -2) {
        /*
         * Raya diagonal con borde de fondo para que
         * destaque sobre los arcos.
         */
        for (i = 0; i < 19; ++i) {
            int x = P_WIFI_CX - 9 + i;
            int y = 19 + i;

            p_rect(x - 1, y - 1, 4, 3, P_BG);
        }

        for (i = 0; i < 19; ++i)
            p_rect(P_WIFI_CX - 9 + i, 19 + i,
                   2, 1, P_MUTED);
    }

    p_wifi_shown = state;
}

static void p_update_wifi(void)
{
    bool connected;
    int32_t rssi = 0;
    uint32_t channel = 0;
    int state;

    connected = esp_hosted_wifi_sta_is_connected();

    /*
     * Detectar cambios de asociacion.
     * No conservar la senal de un enlace desconectado.
     */
    if (connected != p_link) {
        p_link = connected;
        p_signal = -1;
        p_wifi_tries = 0U;
    }

    if (!p_link) {
        p_signal = -1;
        p_wifi_tries = 0U;
    } else if (
        p_signal < 0 &&
        p_wifi_tries < P_WIFI_TRIES
    ) {
        /*
         * Se conserva el limite del original.
         * Esta llamada puede bloquear: no se ha modificado
         * el transporte del kernel.
         */
        ++p_wifi_tries;

        if (wifiSignalRead(&rssi, &channel) == OK) {
            int quality = wifiSignalQuality(rssi);

            if (quality >= 0) {
                if (quality > 100)
                    quality = 100;

                p_signal = quality;
            }
        }
    }

    state = p_wifi_state();

    if (!p_screen_asleep && state != p_wifi_shown)
        p_draw_signal();
}

static void p_poll_wifi(uint32_t now)
{
    if ((uint32_t)(now - p_last_wifi_ms) < P_WIFI_MS)
        return;

    p_last_wifi_ms = now;

    if (p_screen_asleep)
        return;

    /*
     * Siempre comprobar la asociacion.
     *
     * p_update_wifi() NO repite el RPC si ya tiene RSSI,
     * pero si detecta una desconexion.
     */
    p_update_wifi();
}

/* ---------- Launcher ---------- */

static void p_shell(void)
{
    p_rect(0, 0, LCD_W, LCD_H, P_BG);
    p_text("P4 UI", 20, 21, p_accents[p_theme], P_BG);

    p_draw_signal();
    p_draw_battery();

    p_round(90, 438, 140, 30, 15, P_CARD);
    p_round(132, 451, 56, 4, 2, P_MUTED);
}

static void p_home(void)
{
    int i;
    uint16_t accent = p_accents[p_theme];

    p_text("Hola.", 24, 76, P_TEXT, P_BG);
    p_round(24, 124, 272, 70, 22, accent);

    p_text("Material Playground", 40, 139, P_INK, accent);
    p_text("Toca una app para explorar",
           40, 164, P_INK, accent);

    for (i = 0; i < 6; ++i) {
        enum p_app app = (enum p_app)(i + 1);
        int x = 32 + (i % 3) * 96;
        int y = 216 + (i / 3) * 104;
        int tx = x + (
            64 - (int)strlen(p_names[app]) * LCD_TEXT_W
        ) / 2;

        p_icon(app, x, y);
        p_text(p_names[app], tx, y + 72, P_TEXT, P_BG);
    }
}

/* ---------- Voz: dibujo ---------- */

static void v_hint(const char *message)
{
    if (strcmp(v_last_hint, message) == 0)
        return;

    p_rect(8, 372, LCD_W - 16, 20, P_BG);
    p_center(message, 372, P_MUTED, P_BG);

    snprintf(v_last_hint, sizeof(v_last_hint),
             "%s", message);
}

static uint16_t v_color(void)
{
    if (v_state == VOICE_RECORD)
        return P_PINK;
    if (v_state == VOICE_PLAY)
        return P_GREEN;

    return p_accents[p_theme];
}

static void v_meter_reset(void)
{
    p_rect(V_METER_X, V_METER_Y,
           V_METER_W, V_METER_H, P_TRACK);

    v_meter_width = 0;
    v_meter_color = v_color();
}

static void v_meter(uint32_t peak)
{
    int target;
    int width = v_meter_width;
    uint16_t color = v_color();

    if (p_screen_asleep)
        return;

    if (peak > REC_FULLSCALE)
        peak = REC_FULLSCALE;

    target = (int)(peak * V_METER_W / REC_FULLSCALE);

    if (target >= width)
        width = target;
    else
        width -= (width - target + 3) / 4;

    if (color != v_meter_color) {
        p_rect(V_METER_X, V_METER_Y,
               V_METER_W, V_METER_H, P_TRACK);

        if (width > 0)
            p_rect(V_METER_X, V_METER_Y,
                   width, V_METER_H, color);
    } else if (width > v_meter_width) {
        p_rect(
            V_METER_X + v_meter_width, V_METER_Y,
            width - v_meter_width, V_METER_H, color
        );
    } else if (width < v_meter_width) {
        p_rect(
            V_METER_X + width, V_METER_Y,
            v_meter_width - width, V_METER_H, P_TRACK
        );
    }

    v_meter_width = width;
    v_meter_color = color;
}

static void v_draw_state(enum voice_state state)
{
    uint16_t bg;
    uint16_t fg;
    const char *label;
    const char *hint;

    v_state = state;

    if (state == VOICE_RECORD) {
        bg = P_REC_BG;
        fg = P_PINK;
        label = "GRABANDO";
        hint = "Suelta para escuchar";
    } else if (state == VOICE_PLAY) {
        bg = P_VOICE_BG;
        fg = P_GREEN;
        label = "REPRODUCIENDO";
        hint = "Escucha tu mensaje";
    } else {
        bg = P_VOICE_BG;
        fg = p_accents[p_theme];
        label = "MANTEN PULSADO";
        hint = "Tu voz, de vuelta";
    }

    p_round(V_BTN_X, V_BTN_Y, V_BTN_W, V_BTN_H, 24, bg);

    if (state == VOICE_PLAY)
        p_triangle(LCD_W / 2, V_BTN_Y + 40, fg);
    else
        p_mic(LCD_W / 2, V_BTN_Y + 17, fg, bg);

    p_center(label, V_BTN_Y + 81, fg, bg);
    v_hint(hint);
}

static bool v_prepare(void)
{
    if (p_audio == SYSERR)
        return false;
    if (v_buffer != NULL)
        return true;

    v_cached = (uint8_t *)psram_malloc(REC_CAPACITY);

    if (v_cached == NULL)
        return false;

    /* Alias PSRAM conservado del original. */
    v_buffer = (uint8_t *)(
        (uintptr_t)v_cached + REC_PCM_DIRECT
    );

    return true;
}

static void p_voice(void)
{
    v_last_hint[0] = '\0';

    p_text("Voz", 24, 68, P_TEXT, P_BG);
    p_text("Pulsa. Habla. Escucha.", 24, 94, P_MUTED, P_BG);

    p_round(24, 134, 272, 76, 16, P_CARD);
    p_text("NIVEL DE VOZ", 40, 146, P_MUTED, P_CARD);

    v_draw_state(VOICE_IDLE);
    v_meter_reset();

    p_center("Hasta 6 segundos por toma", 414, P_MUTED, P_BG);

    if (p_audio == SYSERR)
        v_hint("Audio no disponible");
    else if (!v_prepare())
        v_hint("No hay memoria para grabar");
}

/* ---------- Voz: audio ---------- */

static uint32_t v_peak(
    const uint8_t *data, uint32_t length
)
{
    uint32_t i;
    uint32_t peak = 0U;

    for (i = 0U; i + 1U < length; i += 2U) {
        int16_t sample = (int16_t)(
            (uint16_t)data[i] |
            ((uint16_t)data[i + 1U] << 8)
        );
        int32_t value = (int32_t)sample;
        uint32_t amplitude = value < 0
            ? (uint32_t)(-value) : (uint32_t)value;

        if (amplitude > peak)
            peak = amplitude;
    }

    return peak;
}

static void v_dup_mono(uint8_t *data, uint32_t length)
{
    uint32_t i;

    for (i = 0U; i + 3U < length; i += 4U) {
        data[i + 2U] = data[i];
        data[i + 3U] = data[i + 1U];
    }
}

static void p_wait_release(void)
{
    touch_point_t points[P_POINTS];
    unsigned misses = 0U;

    while (misses < P_RELEASE_MISS) {
        int count = p_read_touch(points);

        if (count == 0) {
            ++misses;
        } else if (count > 0) {
            misses = 0U;
        } else {
            printf("p4demo: error touch esperando liberacion\n");
            return;
        }

        p_poll_battery(p_now());
        sleep(P_POLL);
    }
}

static uint32_t v_record(void)
{
    touch_point_t points[P_POINTS];
    uint32_t length = 0U;
    uint32_t last_meter;
    uint32_t last_touch;
    uint32_t last_data;
    uint32_t peak_window = 0U;
    unsigned misses = 0U;

    v_draw_state(VOICE_RECORD);
    v_meter_reset();

    if (control(p_audio, AUDIO_CTRL_IN, 1, 0) == SYSERR) {
        v_error = "No se pudo iniciar el micro";
        return 0U;
    }

    last_meter = p_now();
    last_touch = last_meter;
    last_data = last_meter;

    while (length < REC_CAPACITY) {
        uint32_t chunk = REC_CAPACITY - length;
        uint32_t now;
        int got;

        if (chunk > REC_CHUNK)
            chunk = REC_CHUNK;

        got = (int)read(p_audio, v_buffer + length, chunk);
        now = p_now();

        if (got > 0) {
            uint32_t peak;

            if ((uint32_t)got > chunk) {
                v_error = "Tamano de audio incorrecto";
                break;
            }

            last_data = now;
            peak = v_peak(v_buffer + length, (uint32_t)got);

            if (peak > peak_window)
                peak_window = peak;

            length += (uint32_t)got;
        } else if (got < 0) {
            v_error = "Error al leer el micro";
            break;
        }

        if ((uint32_t)(now - last_meter) >= REC_METER_MS) {
            last_meter = now;
            v_meter(peak_window);
            peak_window = 0U;
        }

        p_poll_battery(now);

        if ((uint32_t)(now - last_touch) >= REC_TOUCH_MS) {
            int down;

            last_touch = now;
            down = p_read_touch(points);

            if (down > 0) {
                misses = 0U;
            } else if (down == 0) {
                if (++misses >= P_RELEASE_MISS)
                    break;
            } else {
                v_error = "Error de pantalla tactil";
                break;
            }
        }

        p_idle_poll();

        if (got == 0) {
            if ((uint32_t)(now - last_data) >= REC_STALL_MS) {
                v_error = "El micro no entrega audio";
                break;
            }

            sleep(1);
        }
    }

    control(p_audio, AUDIO_CTRL_IN, 0, 0);

    length -= length % 4U;
    v_dup_mono(v_buffer, length);

    return length;
}

static void v_play(uint32_t length)
{
    touch_point_t points[P_POINTS];
    uint32_t offset = 0U;
    uint32_t last_meter;
    uint32_t last_touch;
    uint32_t peak_window = 0U;

    if (length == 0U)
        return;

    v_draw_state(VOICE_PLAY);
    v_meter_reset();

    if (control(p_audio, AUDIO_CTRL_STREAM, 1, 0) == SYSERR) {
        v_error = "No se pudo iniciar la salida";
        return;
    }

    last_meter = p_now();
    last_touch = last_meter;

    while (offset < length) {
        uint32_t chunk = length - offset;
        uint32_t now;
        uint32_t peak;
        long written;

        if (chunk > REC_CHUNK)
            chunk = REC_CHUNK;

        written = write(p_audio, v_buffer + offset, chunk);

        if (written <= 0 || (uint32_t)written > chunk) {
            v_error = "Error al reproducir el audio";
            break;
        }

        peak = v_peak(v_buffer + offset, (uint32_t)written);

        if (peak > peak_window)
            peak_window = peak;

        offset += (uint32_t)written;
        now = p_now();

        if ((uint32_t)(now - last_meter) >= REC_METER_MS) {
            last_meter = now;
            v_meter(peak_window);
            peak_window = 0U;
        }

        if ((uint32_t)(now - last_touch) >= REC_TOUCH_MS) {
            last_touch = now;
            p_read_touch(points);
        }

        p_idle_poll();
        p_poll_battery(now);
    }

    if (peak_window > 0U)
        v_meter(peak_window);

    control(p_audio, AUDIO_CTRL_STREAM_WAIT, 0, 0);
    control(p_audio, AUDIO_CTRL_STREAM, 0, 0);
}

static void v_run(void)
{
    uint32_t length;

    if (p_audio == SYSERR) {
        v_hint("Audio no disponible");
        return;
    }

    if (!v_prepare()) {
        v_hint("No hay memoria para grabar");
        return;
    }

    v_error = NULL;
    control(p_audio, AUDIO_CTRL_VOLUME, 80, 0);

    length = v_record();

    if (v_error == NULL)
        v_play(length);

    v_hint("Levanta el dedo para continuar");
    p_wait_release();

    v_draw_state(VOICE_IDLE);
    v_meter_reset();

    if (v_error != NULL)
        v_hint(v_error);
    else if (length == 0U)
        v_hint("No se ha grabado audio");
}

/* ---------- FAT: utilidades ---------- */

static void fm_text_fit(
    const char *text, int x, int y,
    int pixels, uint16_t fg, uint16_t bg
)
{
    char shown[64];
    size_t length = strlen(text);
    size_t limit;

    if (pixels <= 0 || LCD_TEXT_W <= 0)
        return;

    limit = (size_t)(pixels / LCD_TEXT_W);

    if (limit >= sizeof(shown))
        limit = sizeof(shown) - 1U;
    if (limit == 0U)
        return;

    if (length <= limit) {
        memcpy(shown, text, length);
        shown[length] = '\0';
    } else {
        memcpy(shown, text, limit);
        shown[limit] = '\0';

        if (limit >= 3U) {
            shown[limit - 3U] = '.';
            shown[limit - 2U] = '.';
            shown[limit - 1U] = '.';
        }
    }

    p_text(shown, x, y, fg, bg);
}

static void fm_message(const char *message)
{
    snprintf(fm_status, sizeof(fm_status), "%s", message);
}

static bool fm_join(
    const char *base, const char *name,
    char *result, size_t capacity
)
{
    size_t a = strlen(base);
    size_t b = strlen(name);
    bool slash = a > 0U && base[a - 1U] != '/';
    size_t needed = a + (slash ? 1U : 0U) + b + 1U;

    if (a == 0U || needed > capacity)
        return false;

    if (b == 0U ||
        strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0 ||
        strchr(name, '/') != NULL ||
        strchr(name, '\\') != NULL)
        return false;

    memcpy(result, base, a);

    if (slash)
        result[a++] = '/';

    memcpy(result + a, name, b + 1U);
    return true;
}

static void fm_small_button(
    int x, int y, int width,
    const char *label, uint16_t color
)
{
    int tx = x + (
        width - (int)strlen(label) * LCD_TEXT_W
    ) / 2;

    p_round(x, y, width, 30, 10, color);
    p_text(label, tx, y + 7, P_INK, color);
}

/* ---------- FAT: listado ---------- */

static bool fm_load(void)
{
    FL_DIR dir;
    fl_dirent entry;
    unsigned skipped = 0U;

    fm_count = 0U;
    fm_more = false;
    fm_ready = false;
    fm_selected = -1;
    fm_confirm = false;

    if (!fs_mounted()) {
        fm_message("No hay volumen FAT montado");
        return false;
    }

    if (fm_path[0] == '\0') {
        if (!fs_resolve("/", fm_path, sizeof(fm_path))) {
            fm_message("No se pudo resolver la raiz");
            return false;
        }
    }

    if (fl_opendir(fm_path, &dir) == NULL) {
        fm_message("No se puede abrir la carpeta");
        return false;
    }

    while (fl_readdir(&dir, &entry) == 0) {
        if (strcmp(entry.filename, ".") == 0 ||
            strcmp(entry.filename, "..") == 0)
            continue;

        if (skipped < fm_offset) {
            ++skipped;
            continue;
        }

        if (fm_count == FM_ROWS) {
            fm_more = true;
            break;
        }

        fm_entries[fm_count++] = entry;
    }

    fl_closedir(&dir);
    fm_ready = true;

    if (fm_count == 0U && fm_offset != 0U) {
        fm_offset = 0U;
        return fm_load();
    }

    return true;
}

static void fm_draw(void)
{
    unsigned i;
    char text[64];

    p_rect(16, 122, 288, 308, P_BG);
    p_round(24, 124, 272, 26, 8, P_CARD);

    fm_text_fit(fm_path[0] ? fm_path : "/",
                32, 130, 256, P_BLUE, P_CARD);

    for (i = 0U; i < fm_count; ++i) {
        int y = FM_LIST_Y + (int)i * FM_ROW_H;
        uint16_t bg = (int)i == fm_selected
            ? P_VOICE_BG : P_CARD;

        p_round(24, y, 272, 34, 8, bg);

        p_text(
            fm_entries[i].is_dir ? "DIR" : "FILE",
            32, y + 9,
            fm_entries[i].is_dir ? P_YELLOW : P_CYAN,
            bg
        );

        fm_text_fit(
            fm_entries[i].filename,
            32 + 5 * LCD_TEXT_W, y + 9,
            256 - 5 * LCD_TEXT_W, P_TEXT, bg
        );
    }

    if (fm_count == 0U)
        p_center(fm_ready ? "Carpeta vacia" : "Sin acceso FAT",
                 218, P_MUTED, P_BG);

    if (fm_confirm) {
        fm_small_button(24, 350, 128, "CANCELAR", P_BLUE);
        fm_small_button(168, 350, 128, "BORRAR", P_PINK);

        fm_text_fit("Borrar elemento seleccionado?",
                    24, 388, 272, P_PINK, P_BG);

        if (fm_selected >= 0 && fm_selected < (int)fm_count)
            fm_text_fit(
                fm_entries[fm_selected].filename,
                24, 408, 272, P_TEXT, P_BG
            );

        return;
    }

    fm_small_button(24, 350, 82, "SUBIR", P_BLUE);
    fm_small_button(119, 350, 82, "ANT", P_BLUE);
    fm_small_button(214, 350, 82, "SIG", P_BLUE);
    fm_small_button(24, 386, 128, "CREAR DIR", P_GREEN);
    fm_small_button(168, 386, 128, "BORRAR", P_PINK);

    if (fm_status[0] != '\0') {
        fm_text_fit(fm_status, 24, 420, 272, P_MUTED, P_BG);
    } else {
        snprintf(text, sizeof(text),
                 "Pagina %u | Toca para seleccionar",
                 fm_offset / FM_ROWS + 1U);

        fm_text_fit(text, 24, 420, 272, P_MUTED, P_BG);
    }
}

/* ---------- FAT: operaciones ---------- */

static void fm_parent(void)
{
    char *last;
    size_t length = strlen(fm_path);

    while (length > 1U && fm_path[length - 1U] == '/')
        fm_path[--length] = '\0';

    last = strrchr(fm_path, '/');

    if (last == NULL || last == fm_path)
        strcpy(fm_path, "/");
    else
        *last = '\0';

    fm_offset = 0U;
    fm_message("");
    fm_load();
    fm_draw();
}

static void fm_create(void)
{
    char name[24];
    char path[FS_PATH_MAX];
    unsigned number;

    if (!fs_mounted()) {
        fm_message("No hay volumen FAT montado");
        fm_draw();
        return;
    }

    if (!fm_ready) {
        fm_message("Abre primero una carpeta valida");
        fm_draw();
        return;
    }

    for (number = 1U; number <= 999U; ++number) {
        snprintf(name, sizeof(name), "Carpeta%03u", number);

        if (!fm_join(fm_path, name, path, sizeof(path))) {
            fm_message("Ruta demasiado larga");
            fm_draw();
            return;
        }

        if (fs_exists(path) || fl_is_dir(path))
            continue;

        if (fl_createdirectory(path) == 1) {
            snprintf(fm_status, sizeof(fm_status),
                     "Creada: %s", name);

            fm_offset = 0U;
            fm_load();
        } else {
            fm_message("No se pudo crear la carpeta");
        }

        fm_draw();
        return;
    }

    fm_message("Ya existen Carpeta001 a Carpeta999");
    fm_draw();
}

static void fm_ask_delete(void)
{
    if (!fm_ready ||
        fm_selected < 0 ||
        fm_selected >= (int)fm_count) {
        fm_message("Selecciona primero un elemento");
        fm_draw();
        return;
    }

    if (!fm_join(
            fm_path, fm_entries[fm_selected].filename,
            fm_delete_path, sizeof(fm_delete_path)
        )) {
        fm_message("Ruta demasiado larga o invalida");
        fm_draw();
        return;
    }

    fm_confirm = true;
    fm_draw();
}

static void fm_delete(void)
{
    if (!fm_confirm)
        return;

    if (!fs_mounted()) {
        fm_message("No hay volumen FAT montado");
    } else if (strcmp(fm_delete_path, "/") == 0) {
        fm_message("No se puede borrar la raiz");
    } else if (fl_is_dir(fm_delete_path)) {
        if (fl_removedirectory(fm_delete_path) == 0)
            fm_message("Carpeta eliminada");
        else
            fm_message("Carpeta no vacia, ocupada o error");
    } else if (!fs_exists(fm_delete_path)) {
        fm_message("El elemento ya no existe");
    } else if (fl_remove(fm_delete_path) == 0) {
        fm_message("Archivo eliminado");
    } else {
        fm_message("No se pudo eliminar el archivo");
    }

    fm_confirm = false;
    fm_load();
    fm_draw();
}

static void fm_tap(int x, int y)
{
    unsigned i;

    if (fm_confirm) {
        if (p_inside(x, y, 24, 350, 128, 30)) {
            fm_confirm = false;
            fm_draw();
        } else if (p_inside(x, y, 168, 350, 128, 30)) {
            fm_delete();
        }

        return;
    }

    for (i = 0U; i < fm_count; ++i) {
        int row_y = FM_LIST_Y + (int)i * FM_ROW_H;

        if (!p_inside(x, y, 24, row_y, 272, 34))
            continue;

        if (fm_selected == (int)i && fm_entries[i].is_dir) {
            char next[FS_PATH_MAX];
            FL_DIR check;

            if (!fs_mounted()) {
                fm_message("No hay volumen FAT montado");
            } else if (!fm_join(
                    fm_path, fm_entries[i].filename,
                    next, sizeof(next)
                )) {
                fm_message("Ruta demasiado larga o invalida");
            } else if (fl_opendir(next, &check) == NULL) {
                fm_message("No se puede abrir la carpeta");
            } else {
                fl_closedir(&check);
                strcpy(fm_path, next);
                fm_offset = 0U;
                fm_message("");
                fm_load();
            }
        } else {
            fm_selected = (int)i;

            if (fm_entries[i].is_dir) {
                fm_message("Toca otra vez para abrir");
            } else {
                snprintf(fm_status, sizeof(fm_status),
                         "Archivo: %lu bytes",
                         (unsigned long)fm_entries[i].size);
            }
        }

        fm_draw();
        return;
    }

    if (p_inside(x, y, 24, 350, 82, 30)) {
        fm_parent();
        return;
    }

    if (p_inside(x, y, 119, 350, 82, 30)) {
        if (fm_offset != 0U) {
            fm_offset = fm_offset >= FM_ROWS
                ? fm_offset - FM_ROWS : 0U;

            fm_message("");
            fm_load();
            fm_draw();
        }
        return;
    }

    if (p_inside(x, y, 214, 350, 82, 30)) {
        if (fm_more) {
            fm_offset += FM_ROWS;
            fm_message("");
            fm_load();
            fm_draw();
        }
        return;
    }

    if (p_inside(x, y, 24, 386, 128, 30)) {
        fm_create();
        return;
    }

    if (p_inside(x, y, 168, 386, 128, 30))
        fm_ask_delete();
}

static void p_files(void)
{
    p_text("Archivos", 24, 68, P_TEXT, P_BG);
    p_text("Gestor FAT", 24, 94, P_MUTED, P_BG);

    fm_message("");
    fm_load();
    fm_draw();
}

/* ---------- Aplicaciones de demostracion ---------- */

static void p_music_control(void)
{
    p_round(128, 368, 64, 48, 20, P_PURPLE);

    if (p_playing) {
        p_rect(150, 382, 5, 20, P_INK);
        p_rect(165, 382, 5, 20, P_INK);
    } else {
        p_triangle(158, 392, P_INK);
    }
}

static void p_music(void)
{
    p_round(24, 128, 272, 174, 22, P_CARD);
    p_icon(P_MUSIC, 128, 144);

    p_center("Material Dreams", 225, P_TEXT, P_CARD);
    p_center("Audio de demostracion", 251, P_MUTED, P_CARD);

    p_round(40, 322, 240, 6, 3, P_TRACK);
    p_round(40, 322, 90, 6, 3, P_PURPLE);

    p_text("0:42", 40, 340, P_MUTED, P_BG);
    p_text("3:18", 248, 340, P_MUTED, P_BG);

    p_music_control();
}

static void p_note_content(void)
{
    p_rect(40, 184, 240, 76, P_YELLOW);

    if (p_note == 0) {
        p_text("Las buenas ideas", 42, 188, P_INK, P_YELLOW);
        p_text("empiezan con una", 42, 212, P_INK, P_YELLOW);
        p_text("pantalla en blanco.", 42, 236, P_INK, P_YELLOW);
    } else {
        p_text("Crear.", 42, 188, P_INK, P_YELLOW);
        p_text("Probar.", 42, 212, P_INK, P_YELLOW);
        p_text("Volver a imaginar.", 42, 236, P_INK, P_YELLOW);
    }
}

static void p_notes(void)
{
    p_round(24, 128, 272, 212, 22, P_YELLOW);
    p_text("Mi bloc", 42, 148, P_INK, P_YELLOW);

    p_note_content();

    p_text("Nota de ejemplo", 42, 304, P_INK, P_YELLOW);
    p_button(64, 364, 192, "CAMBIAR NOTA", P_YELLOW);
}

static void p_weather(void)
{
    p_round(24, 128, 272, 184, 22, P_CYAN);
    p_text("Ciudad demo", 42, 148, P_INK, P_CYAN);

    p_round(126, 181, 68, 68, 34, P_YELLOW);
    p_text("24 C", 144, 208, P_INK, P_YELLOW);
    p_text("Cielo despejado", 100, 278, P_INK, P_CYAN);

    p_round(24, 332, 272, 70, 20, P_CARD);
    p_text("Humedad  45%", 42, 346, P_TEXT, P_CARD);
    p_text("Viento   8 km/h", 42, 373, P_MUTED, P_CARD);

    p_center("Datos ficticios / sin conexion",
             414, P_MUTED, P_BG);
}

static void p_settings_switch(void)
{
    p_round(226, 252, 50, 28, 14,
            p_switch ? P_GREEN : P_TRACK);

    p_round(p_switch ? 251 : 229,
            255, 22, 22, 11, P_TEXT);
}

static void p_settings_volume(void)
{
    char text[28];

    p_rect(40, 333, 240, 18, P_CARD);
    snprintf(text, sizeof(text), "Volumen demo: %d%%", p_volume);
    p_text(text, 40, 333, P_TEXT, P_CARD);

    p_rect(40, 366, 240, 20, P_CARD);
    p_round(40, 373, 240, 6, 3, P_TRACK);

    if (p_volume > 0)
        p_round(40, 373, p_volume * 240 / 100,
                6, 3, p_accents[p_theme]);

    p_round(40 + p_volume * 224 / 100,
            368, 16, 16, 8, p_accents[p_theme]);
}

static void p_settings(void)
{
    int i;

    p_round(24, 128, 272, 90, 22, P_CARD);
    p_text("Color del sistema", 40, 144, P_TEXT, P_CARD);

    for (i = 0; i < 3; ++i) {
        int x = 44 + i * 78;

        p_round(x, 174, 56, 28, 12, p_accents[i]);

        if (i == p_theme)
            p_text("*", x + 24, 180, P_INK, p_accents[i]);
    }

    p_round(24, 234, 272, 66, 20, P_CARD);
    p_text("Modo demo", 40, 259, P_TEXT, P_CARD);
    p_settings_switch();

    p_round(24, 316, 272, 94, 20, P_CARD);
    p_settings_volume();
}

/* ---------- Navegacion ---------- */

static void p_render(void)
{
    p_shell();

    switch (p_page) {
    case P_HOME:
        p_home();
        return;

    case P_VOICE:
        p_voice();
        return;

    case P_FILES:
        p_files();
        return;

    default:
        p_text(p_names[p_page], 24, 68, P_TEXT, P_BG);
        p_text("Vista de demostracion", 24, 94, P_MUTED, P_BG);
        break;
    }

    switch (p_page) {
    case P_MUSIC:
        p_music();
        break;
    case P_NOTES:
        p_notes();
        break;
    case P_WEATHER:
        p_weather();
        break;
    case P_SETTINGS:
        p_settings();
        break;
    default:
        break;
    }
}

static void p_tap(int x, int y)
{
    int i;

    if (p_inside(x, y, 80, 432, 160, 48)) {
        if (p_page != P_HOME) {
            fm_confirm = false;
            p_page = P_HOME;
            p_render();
        }
        return;
    }

    if (p_page == P_HOME) {
        for (i = 0; i < 6; ++i) {
            int bx = 24 + (i % 3) * 96;
            int by = 210 + (i / 3) * 104;

            if (p_inside(x, y, bx, by, 80, 96)) {
                p_page = (enum p_app)(i + 1);
                p_render();
                return;
            }
        }
        return;
    }

    if (p_page == P_FILES) {
        fm_tap(x, y);
        return;
    }

    if (p_page == P_VOICE) {
        if (p_inside(x, y, V_BTN_X, V_BTN_Y,
                     V_BTN_W, V_BTN_H))
            v_run();

        return;
    }

    if (p_page == P_MUSIC &&
        p_inside(x, y, 118, 358, 84, 68)) {
        p_playing = !p_playing;
        p_music_control();
        return;
    }

    if (p_page == P_NOTES &&
        p_inside(x, y, 64, 364, 192, 44)) {
        p_note = !p_note;
        p_note_content();
        return;
    }

    if (p_page != P_SETTINGS)
        return;

    for (i = 0; i < 3; ++i) {
        if (p_inside(x, y, 44 + i * 78, 170, 56, 36)) {
            if (p_theme != i) {
                p_theme = i;
                p_render();
            }
            return;
        }
    }

    if (p_inside(x, y, 214, 240, 74, 48)) {
        p_switch = !p_switch;
        p_settings_switch();
        return;
    }

    if (p_inside(x, y, 36, 358, 248, 40)) {
        int value = (x - 40) * 100 / 240;

        if (value < 0)
            value = 0;
        if (value > 100)
            value = 100;

        if (value != p_volume) {
            p_volume = value;
            p_settings_volume();
        }
    }
}

/* ---------- Tarea principal ---------- */

int p4demoTask(void)
{
    touch_point_t points[P_POINTS];
    bool touch_latched = false;
    unsigned release_count = 0U;
    bool touch_error_reported = false;

    printf("p4demo: INICIO de tarea\n");

    p_lcd = getdev("/dev/lcd0");
    p_touch = getdev("/dev/touch0");
    p_power = getdev("/dev/power0");
    p_audio = getdev("/dev/audio0");

    if (p_lcd == SYSERR || p_touch == SYSERR) {
        printf("p4demo: LCD o touch no disponible\n");
        return 0;
    }

    p_page = P_HOME;
    p_theme = 0;
    p_playing = false;
    p_switch = true;
    p_note = 0;
    p_volume = 60;
    p_battery = -1;

    fm_path[0] = '\0';
    fm_delete_path[0] = '\0';
    fm_status[0] = '\0';
    fm_offset = 0U;
    fm_count = 0U;
    fm_more = false;
    fm_ready = false;
    fm_selected = -1;
    fm_confirm = false;

    /*
     * Conservar reserva PSRAM existente.
     * No permite ejecutar instancias concurrentes.
     */
    v_state = VOICE_IDLE;
    v_last_hint[0] = '\0';
    v_error = NULL;

    p_screen_asleep = false;
    p_ignore_touch = false;
    p_ignore_release = 0U;

    p_signal = -1;
    p_link = esp_hosted_wifi_sta_is_connected();
    p_wifi_shown = -100;
    p_wifi_tries = 0U;

    p_last_touch_ms = p_now();
    p_last_battery = p_last_touch_ms;
    p_last_wifi_ms = p_last_touch_ms;

    if (control(p_lcd, LCD_CTRL_BACKLIGHT, 1, 0) == SYSERR) {
        printf("p4demo: no se pudo encender backlight\n");
        return 0;
    }

    p_render();
    p_update_battery();

    /*
     * Mantiene el primer intento de RSSI del original.
     * Puede bloquear segun wifiSignalRead().
     */
    p_update_wifi();

    p_last_touch_ms = p_now();
    p_last_battery = p_last_touch_ms;
    p_last_wifi_ms = p_last_touch_ms;

    //printf("p4demo: reposo tras %lu ms sin touch\n",
           //(unsigned long)P_IDLE_MS);

    for (;;) {
        int count = p_read_touch(points);

        if (count > 0) {
            touch_error_reported = false;
            release_count = 0U;

            if (p_ignore_touch || p_screen_asleep) {
                touch_latched = true;
            } else if (!touch_latched) {
                touch_latched = true;
                p_tap(points[0].x, points[0].y);
            }
        } else if (count == 0) {
            touch_error_reported = false;

            if (release_count < P_RELEASE_MISS)
                ++release_count;

            if (release_count >= P_RELEASE_MISS)
                touch_latched = false;
        } else {
            release_count = 0U;

            if (!touch_error_reported) {
                printf("p4demo: error al leer touch\n");
                touch_error_reported = true;
            }
        }

        p_poll_battery(p_now());

        /*
         * Evitar iniciar un RPC potencialmente bloqueante
         * mientras se esta manteniendo un gesto tactil.
         */
        if (!touch_latched && count == 0)
            p_poll_wifi(p_now());

        p_idle_poll();

        /* Unidad de sleep(): la definida por tu kernel. */
        sleep(P_POLL);
    }

    return 0;
}
