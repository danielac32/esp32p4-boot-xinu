/**
 * @file xsh_dash.c
 *
 * Dashboard de voz con interfaz inspirada en Material Design.
 *
 * - Mantener pulsado el botón para grabar.
 * - Soltar para reproducir.
 * - Hasta 6 segundos por toma.
 * - Medidor de nivel durante grabación y reproducción.
 * - Indicador de batería.
 *
 * Usa los dispositivos existentes de Embedded Xinu:
 * /dev/lcd0, /dev/touch0, /dev/power0 y /dev/audio0.
 *
 * Conserva el acceso directo a PSRAM y el formato de audio
 * del dashboard original.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <kernel.h>
#include <clock.h>
#include <device.h>
#include <lcd.h>
#include <touch.h>
#include <power.h>
#include <audio.h>
#include <memory.h>

/* -------------------------------------------------------------
 * Audio y temporización
 * ------------------------------------------------------------- */

#define REC_PCM_DIRECT      0x40000000U

#define DASH_MAXPTS         5U
#define DASH_BYTES_PER_MS   192U
#define DASH_MAX_MS         6000U
#define DASH_CHUNK          4096U
#define DASH_FULLSCALE      32767U

#define DASH_POLL_MS        10U
#define DASH_METER_MS       40U
#define DASH_TOUCH_MS       15U
#define DASH_BATT_MS        500U
#define DASH_STALL_MS       600U
#define DASH_RELEASE_MISS   3U

/* -------------------------------------------------------------
 * Geometría: pantalla vertical 320 x 480
 * ------------------------------------------------------------- */

#define BTN_X               40
#define BTN_Y               230
#define BTN_W               240
#define BTN_H               120

#define CARD_X              24
#define CARD_Y              134
#define CARD_W              272
#define CARD_H              76

#define METER_X             40
#define METER_Y             174
#define METER_W             240
#define METER_H             12

#define HINT_Y              372
#define FOOTER_Y            420

/* -------------------------------------------------------------
 * Paleta RGB565
 * ------------------------------------------------------------- */

#define UI_BG               RGB565(16, 20, 28)
#define UI_CARD             RGB565(28, 34, 45)
#define UI_TRACK            RGB565(43, 51, 65)

#define UI_PRIMARY          RGB565(173, 198, 255)
#define UI_BUTTON           RGB565(48, 66, 100)

#define UI_TEXT             RGB565(235, 239, 248)
#define UI_MUTED            RGB565(159, 170, 190)

#define UI_RECORD           RGB565(255, 151, 158)
#define UI_REC_BG           RGB565(91, 38, 49)

#define UI_PLAY             RGB565(153, 222, 181)

/* -------------------------------------------------------------
 * Estado visual
 * ------------------------------------------------------------- */

enum dash_ui_state
{
    DASH_UI_IDLE,
    DASH_UI_RECORD,
    DASH_UI_PLAY
};

static enum dash_ui_state dash_visual_state = DASH_UI_IDLE;

static bool dash_screen_ready = false;

static long dash_last_battery = -999;

static int dash_meter_width = 0;
static uint16_t dash_meter_color = UI_PRIMARY;

static char dash_last_hint[48] = "";

/* -------------------------------------------------------------
 * Primitivas gráficas
 * ------------------------------------------------------------- */

static void dash_fill(int lcd, int x, int y,
                      int w, int h, uint16_t color)
{
    struct lcd_rect r;

    /* Recorte para no enviar rectángulos fuera de pantalla. */
    if (x < 0)
    {
        w += x;
        x = 0;
    }

    if (y < 0)
    {
        h += y;
        y = 0;
    }

    if (x >= LCD_W || y >= LCD_H)
    {
        return;
    }

    if (w > LCD_W - x)
    {
        w = LCD_W - x;
    }

    if (h > LCD_H - y)
    {
        h = LCD_H - y;
    }

    if (w <= 0 || h <= 0)
    {
        return;
    }

    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    r.color = color;

    control(lcd, LCD_CTRL_RECT, (long)&r, 0);
}

static void dash_text(int lcd, const char *s,
                      int x, int y,
                      uint16_t fg, uint16_t bg)
{
    struct lcd_text t;

    t.s = s;
    t.x = x;
    t.y = y;
    t.fg = fg;
    t.bg = bg;

    control(lcd, LCD_CTRL_TEXT, (long)&t, 0);
}

static void dash_text_center(int lcd, const char *s,
                             int y, uint16_t fg, uint16_t bg)
{
    int width = (int)strlen(s) * LCD_TEXT_W;
    int x = (LCD_W - width) / 2;

    if (x < 0)
    {
        x = 0;
    }

    dash_text(lcd, s, x, y, fg, bg);
}

/*
 * Rectángulo redondeado mediante franjas horizontales.
 * No requiere framebuffer ni operaciones nuevas del controlador.
 */
static void dash_round(int lcd, int x, int y,
                       int w, int h, int radius,
                       uint16_t color)
{
    int row;

    if (w <= 0 || h <= 0)
    {
        return;
    }

    if (radius > w / 2)
    {
        radius = w / 2;
    }

    if (radius > h / 2)
    {
        radius = h / 2;
    }

    if (radius <= 0)
    {
        dash_fill(lcd, x, y, w, h, color);
        return;
    }

    dash_fill(lcd, x, y + radius,
              w, h - 2 * radius, color);

    for (row = 0; row < radius; ++row)
    {
        int dy = radius - row - 1;
        int dx = 0;
        int inset;

        while ((dx + 1) * (dx + 1) + dy * dy
               <= radius * radius)
        {
            ++dx;
        }

        inset = radius - dx;

        dash_fill(lcd, x + inset, y + row,
                  w - 2 * inset, 1, color);

        dash_fill(lcd, x + inset, y + h - row - 1,
                  w - 2 * inset, 1, color);
    }
}

/* -------------------------------------------------------------
 * Iconos
 * ------------------------------------------------------------- */

static void dash_mic_icon(int lcd, int cx, int y,
                          uint16_t fg, uint16_t bg)
{
    /* Soporte exterior del micrófono. */
    dash_round(lcd, cx - 14, y + 10,
               28, 25, 12, fg);

    dash_round(lcd, cx - 11, y + 7,
               22, 25, 10, bg);

    /* Cápsula. */
    dash_round(lcd, cx - 7, y,
               14, 25, 7, fg);

    /* Pie. */
    dash_fill(lcd, cx - 2, y + 34, 4, 7, fg);

    dash_round(lcd, cx - 11, y + 40,
               22, 3, 1, fg);
}

static void dash_play_icon(int lcd, int cx, int cy,
                           uint16_t color)
{
    int column;

    for (column = 0; column < 25; ++column)
    {
        int half = (24 - column) / 2;

        dash_fill(lcd, cx - 10 + column,
                  cy - half, 1, 2 * half + 1, color);
    }
}

/* -------------------------------------------------------------
 * Mensajes y botón
 * ------------------------------------------------------------- */

static void dash_hint(int lcd, const char *message)
{
    if (strcmp(dash_last_hint, message) == 0)
    {
        return;
    }

    dash_fill(lcd, 8, HINT_Y, LCD_W - 16, 20, UI_BG);

    dash_text_center(lcd, message, HINT_Y,
                     UI_MUTED, UI_BG);

    snprintf(dash_last_hint, sizeof(dash_last_hint),
             "%s", message);
}

static void dash_state(int lcd, enum dash_ui_state state)
{
    uint16_t bg;
    uint16_t fg;
    const char *label;
    const char *hint;

    if (state == DASH_UI_RECORD)
    {
        bg = UI_REC_BG;
        fg = UI_RECORD;
        label = "GRABANDO";
        hint = "Suelta para escuchar";
    }
    else if (state == DASH_UI_PLAY)
    {
        bg = UI_BUTTON;
        fg = UI_PLAY;
        label = "REPRODUCIENDO";
        hint = "Escucha tu mensaje";
    }
    else
    {
        bg = UI_BUTTON;
        fg = UI_PRIMARY;
        label = "MANTEN PULSADO";
        hint = "Tu voz, de vuelta";
    }

    if (!dash_screen_ready || state != dash_visual_state)
    {
        dash_round(lcd, BTN_X, BTN_Y,
                   BTN_W, BTN_H, 24, bg);

        if (state == DASH_UI_PLAY)
        {
            dash_play_icon(lcd, LCD_W / 2,
                           BTN_Y + 40, fg);
        }
        else
        {
            dash_mic_icon(lcd, LCD_W / 2,
                          BTN_Y + 17, fg, bg);
        }

        dash_text_center(lcd, label,
                         BTN_Y + 81, fg, bg);
    }

    dash_visual_state = state;
    dash_hint(lcd, hint);
}

/* -------------------------------------------------------------
 * Batería
 * ------------------------------------------------------------- */

static void dash_battery(int lcd, int powerfd)
{
    long pct;
    char text[12];
    uint16_t color;
    int fill;

    if (powerfd == SYSERR)
    {
        pct = -1;
    }
    else
    {
        pct = control(powerfd, POWER_CTRL_PERCENT, 0, 0);

        if (pct < 0 || pct > 100)
        {
            pct = -1;
        }
    }

    if (pct == dash_last_battery)
    {
        return;
    }

    dash_last_battery = pct;

    dash_fill(lcd, 200, 18, 104, 26, UI_BG);

    if (pct < 0)
    {
        snprintf(text, sizeof(text), "--");
    }
    else
    {
        snprintf(text, sizeof(text), "%ld%%", pct);
    }

    dash_text(lcd, text, 200, 23,
              UI_MUTED, UI_BG);

    color = (pct >= 0 && pct <= 20)
        ? UI_RECORD : UI_PLAY;

    /* Contorno y terminal de la batería. */
    dash_round(lcd, 252, 22, 42, 18, 4, UI_MUTED);
    dash_round(lcd, 254, 24, 38, 14, 2, UI_BG);

    dash_fill(lcd, 294, 27, 3, 8, UI_MUTED);

    fill = (pct < 0) ? 0 : (int)(pct * 34L / 100L);

    if (fill > 0)
    {
        dash_fill(lcd, 256, 26, fill, 10, color);
    }
}

/* -------------------------------------------------------------
 * Medidor con ataque rápido y caída suave
 * ------------------------------------------------------------- */

static uint16_t dash_current_meter_color(void)
{
    if (dash_visual_state == DASH_UI_RECORD)
    {
        return UI_RECORD;
    }

    if (dash_visual_state == DASH_UI_PLAY)
    {
        return UI_PLAY;
    }

    return UI_PRIMARY;
}

static void dash_meter_reset(int lcd)
{
    dash_fill(lcd, METER_X, METER_Y,
              METER_W, METER_H, UI_TRACK);

    dash_meter_width = 0;
    dash_meter_color = dash_current_meter_color();
}

static void dash_meter(int lcd, uint32_t peak)
{
    int target;
    int width;
    uint16_t color;

    if (peak > DASH_FULLSCALE)
    {
        peak = DASH_FULLSCALE;
    }

    target = (int)(peak * METER_W / DASH_FULLSCALE);
    width = dash_meter_width;

    if (target >= width)
    {
        width = target;
    }
    else
    {
        width -= (width - target + 3) / 4;
    }

    color = dash_current_meter_color();

    if (color != dash_meter_color)
    {
        dash_fill(lcd, METER_X, METER_Y,
                  METER_W, METER_H, UI_TRACK);

        if (width > 0)
        {
            dash_fill(lcd, METER_X, METER_Y,
                      width, METER_H, color);
        }
    }
    else if (width > dash_meter_width)
    {
        dash_fill(lcd, METER_X + dash_meter_width, METER_Y,
                  width - dash_meter_width,
                  METER_H, color);
    }
    else if (width < dash_meter_width)
    {
        dash_fill(lcd, METER_X + width, METER_Y,
                  dash_meter_width - width,
                  METER_H, UI_TRACK);
    }

    dash_meter_width = width;
    dash_meter_color = color;
}

/* -------------------------------------------------------------
 * Procesamiento de audio
 * ------------------------------------------------------------- */

/*
 * Duplica el canal izquierdo en el derecho.
 * Conserva el comportamiento del código original para el ES8311.
 */
static void dash_dup_mono(uint8_t *p, uint32_t n)
{
    uint32_t k;

    for (k = 0U; k + 3U < n; k += 4U)
    {
        p[k + 2U] = p[k];
        p[k + 3U] = p[k + 1U];
    }
}

static uint32_t dash_peak(const uint8_t *p, uint32_t n)
{
    uint32_t i;
    uint32_t peak = 0U;

    for (i = 0U; i + 1U < n; i += 2U)
    {
        int16_t value;
        int32_t signed_value;
        uint32_t amplitude;

        value = (int16_t)(
            (uint16_t)p[i] |
            ((uint16_t)p[i + 1U] << 8)
        );

        signed_value = (int32_t)value;

        amplitude = (signed_value < 0)
            ? (uint32_t)(-signed_value)
            : (uint32_t)signed_value;

        if (amplitude > peak)
        {
            peak = amplitude;
        }
    }

    return peak;
}

/* -------------------------------------------------------------
 * Touch
 * ------------------------------------------------------------- */

static bool dash_in_button(const touch_point_t *points, int count)
{
    int i;

    if (count > (int)DASH_MAXPTS)
    {
        count = (int)DASH_MAXPTS;
    }

    for (i = 0; i < count; ++i)
    {
        int x = points[i].x;
        int y = points[i].y;

        if (x >= BTN_X && x < BTN_X + BTN_W &&
            y >= BTN_Y && y < BTN_Y + BTN_H)
        {
            return true;
        }
    }

    return false;
}

/*
 * Espera una suelta confirmada.
 * Evita que un toque largo inicie otra grabación inmediatamente.
 */
static void dash_wait_release(int touchfd)
{
    touch_point_t points[DASH_MAXPTS];
    uint32_t misses = 0U;

    while (misses < DASH_RELEASE_MISS)
    {
        int count = (int)read(touchfd, points, DASH_MAXPTS);

        if (count == 0)
        {
            ++misses;
        }
        else
        {
            misses = 0U;
        }

        sleep(DASH_POLL_MS);
    }
}

/* -------------------------------------------------------------
 * Pantalla de reposo
 * ------------------------------------------------------------- */

static void dash_idle_screen(int lcd, int powerfd)
{
    if (!dash_screen_ready)
    {
        dash_fill(lcd, 0, 0, LCD_W, LCD_H, UI_BG);

        dash_text(lcd, "VOZ", 24, 24,
                  UI_PRIMARY, UI_BG);

        dash_text(lcd, "Grabadora", 24, 72,
                  UI_TEXT, UI_BG);

        dash_text(lcd, "Pulsa. Habla. Escucha.", 24, 98,
                  UI_MUTED, UI_BG);

        dash_round(lcd, CARD_X, CARD_Y,
                   CARD_W, CARD_H, 16, UI_CARD);

        dash_text(lcd, "NIVEL DE VOZ", 40, 146,
                  UI_MUTED, UI_CARD);

        dash_text_center(lcd, "Hasta 6 segundos por toma",
                         FOOTER_Y, UI_MUTED, UI_BG);
    }

    dash_state(lcd, DASH_UI_IDLE);
    dash_meter_reset(lcd);
    dash_battery(lcd, powerfd);

    dash_screen_ready = true;
}

/* -------------------------------------------------------------
 * Grabación
 * ------------------------------------------------------------- */

static uint32_t dash_record(int lcd, int touchfd,
                            int powerfd, int audiofd,
                            uint8_t *recbuf, uint32_t cap)
{
    uint32_t rec_len = 0U;

    uint32_t last_meter;
    uint32_t last_touch;
    uint32_t last_batt;
    uint32_t last_data;

    uint32_t peak_window = 0U;
    uint32_t misses = 0U;

    touch_point_t points[DASH_MAXPTS];

    if (cap == 0U)
    {
        return 0U;
    }

    dash_state(lcd, DASH_UI_RECORD);
    dash_meter_reset(lcd);

    if (SYSERR == control(audiofd, AUDIO_CTRL_IN, 1, 0))
    {
        dash_hint(lcd, "No se pudo iniciar el micro");
        return 0U;
    }

    last_meter = clktotal;
    last_touch = last_meter;
    last_batt = last_meter;
    last_data = last_meter;

    while (rec_len < cap)
    {
        uint32_t chunk = cap - rec_len;
        uint32_t now;
        int got;

        if (chunk > DASH_CHUNK)
        {
            chunk = DASH_CHUNK;
        }

        got = (int)read(audiofd, recbuf + rec_len, chunk);
        now = clktotal;

        if (got > 0)
        {
            uint32_t block_peak;

            last_data = now;

            block_peak = dash_peak(recbuf + rec_len,
                                   (uint32_t)got);

            if (block_peak > peak_window)
            {
                peak_window = block_peak;
            }

            rec_len += (uint32_t)got;
        }
        else if (got < 0)
        {
            dash_hint(lcd, "Error al leer el micro");
            break;
        }

        if (now - last_meter >= DASH_METER_MS)
        {
            last_meter = now;

            dash_meter(lcd, peak_window);
            peak_window = 0U;
        }

        if (now - last_batt >= DASH_BATT_MS)
        {
            last_batt = now;
            dash_battery(lcd, powerfd);
        }

        if (now - last_touch >= DASH_TOUCH_MS)
        {
            int down;

            last_touch = now;
            down = (int)read(touchfd, points, DASH_MAXPTS);

            /*
             * Después de iniciar la grabación, cualquier contacto
             * mantiene la toma aunque el dedo salga del botón.
             */
            if (down > 0)
            {
                misses = 0U;
            }
            else if (down == 0)
            {
                if (++misses >= DASH_RELEASE_MISS)
                {
                    break;
                }
            }
            else
            {
                dash_hint(lcd, "Error de pantalla tactil");
                break;
            }
        }

        if (got == 0)
        {
            if (now - last_data >= DASH_STALL_MS)
            {
                dash_hint(lcd, "El micro no entrega audio");
                break;
            }

            sleep(1);
        }
    }

    control(audiofd, AUDIO_CTRL_IN, 0, 0);

    /*
     * Trabajar sobre la toma completa evita depender de que
     * cada read() termine exactamente en una trama estéreo.
     */
    rec_len -= rec_len % 4U;

    dash_dup_mono(recbuf, rec_len);

    return rec_len;
}

/* -------------------------------------------------------------
 * Reproducción
 * ------------------------------------------------------------- */

static void dash_play(int lcd, int powerfd, int audiofd,
                      const uint8_t *recbuf, uint32_t len)
{
    uint32_t offset = 0U;
    uint32_t last_meter;
    uint32_t last_batt;
    uint32_t peak_window = 0U;
    bool output_error = false;

    if (len == 0U)
    {
        return;
    }

    dash_state(lcd, DASH_UI_PLAY);
    dash_meter_reset(lcd);

    if (SYSERR == control(audiofd, AUDIO_CTRL_STREAM, 1, 0))
    {
        dash_hint(lcd, "No se pudo iniciar la salida");
        return;
    }

    last_meter = clktotal;
    last_batt = last_meter;

    while (offset < len)
    {
        uint32_t chunk = len - offset;
        uint32_t now;
        long written;

        if (chunk > DASH_CHUNK)
        {
            chunk = DASH_CHUNK;
        }

        written = write(audiofd, recbuf + offset, chunk);

        if (written <= 0)
        {
            output_error = true;
            break;
        }

        {
            uint32_t block_peak = dash_peak(
                recbuf + offset, (uint32_t)written
            );

            if (block_peak > peak_window)
            {
                peak_window = block_peak;
            }
        }

        offset += (uint32_t)written;
        now = clktotal;

        if (now - last_meter >= DASH_METER_MS)
        {
            last_meter = now;

            dash_meter(lcd, peak_window);
            peak_window = 0U;
        }

        if (now - last_batt >= DASH_BATT_MS)
        {
            last_batt = now;
            dash_battery(lcd, powerfd);
        }
    }

    if (peak_window > 0U)
    {
        dash_meter(lcd, peak_window);
    }

    /*
     * Esperar a que salga el audio pendiente antes de cerrar
     * el stream, igual que en el dashboard original.
     */
    control(audiofd, AUDIO_CTRL_STREAM_WAIT, 0, 0);
    control(audiofd, AUDIO_CTRL_STREAM, 0, 0);

    if (output_error)
    {
        dash_hint(lcd, "Error al reproducir el audio");
    }

    dash_meter_reset(lcd);
}

/* -------------------------------------------------------------
 * Tarea principal
 * ------------------------------------------------------------- */

int dashTask(void)
{
    int lcd;
    int touchfd;
    int powerfd;
    int audiofd;

    touch_point_t points[DASH_MAXPTS];

    uint8_t *cached;
    uint8_t *recbuf;

    uint32_t cap;
    uint32_t last_idle_batt;

    lcd = getdev("/dev/lcd0");
    touchfd = getdev("/dev/touch0");
    powerfd = getdev("/dev/power0");
    audiofd = getdev("/dev/audio0");

    if (lcd == SYSERR)
    {
        return 0;
    }

    /* Restablecer el estado si la tarea vuelve a iniciarse. */
    dash_screen_ready = false;
    dash_visual_state = DASH_UI_IDLE;
    dash_last_battery = -999;
    dash_last_hint[0] = '\0';
    dash_meter_width = 0;
    dash_meter_color = UI_PRIMARY;

    dash_idle_screen(lcd, powerfd);

    if (touchfd == SYSERR || audiofd == SYSERR)
    {
        dash_hint(lcd, "Falta el dispositivo de audio/touch");
        return 0;
    }

    cap = DASH_MAX_MS * DASH_BYTES_PER_MS;

    cached = (uint8_t *)psram_malloc(cap);

    if (cached == NULL)
    {
        dash_hint(lcd, "No hay memoria para grabar");
        return 0;
    }

    /*
     * Mismo alias directo de PSRAM utilizado por el código
     * original de esta placa.
     */
    recbuf = (uint8_t *)(
        (uintptr_t)cached + REC_PCM_DIRECT
    );

    control(audiofd, AUDIO_CTRL_VOLUME, 80, 0);

    last_idle_batt = clktotal;

    for (;;)
    {
        int count;

        count = (int)read(touchfd, points, DASH_MAXPTS);

        if (count > 0 && dash_in_button(points, count))
        {
            uint32_t len;

            len = dash_record(lcd, touchfd, powerfd,
                              audiofd, recbuf, cap);

            dash_play(lcd, powerfd, audiofd,
                      recbuf, len);

            /*
             * Al llegar al máximo de grabación puede que
             * el dedo siga apoyado.
             */
            dash_hint(lcd, "Levanta el dedo para continuar");
            dash_wait_release(touchfd);

            dash_idle_screen(lcd, powerfd);

            last_idle_batt = clktotal;
        }

        if (clktotal - last_idle_batt >= DASH_BATT_MS)
        {
            last_idle_batt = clktotal;
            dash_battery(lcd, powerfd);
        }

        sleep(DASH_POLL_MS);
    }
}
