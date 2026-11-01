/**
 * @file xsh_dash.c
 *
 * Dashboard de arranque que ejercita los cuatro periféricos de a bordo de una
 * vez: la batería del PMIC se lee en la esquina superior derecha, el panel se
 * pinta por /dev/lcd0 y el dedo se lee por /dev/touch0.  Sobre el panel hay un
 * botón: mantenerlo pulsado graba el micro mientras un medidor dibuja el nivel,
 * y al soltarlo se reproduce lo grabado.  Es un lazo sin fin.
 *
 * Se lanza como tarea desde main() justo después del bootTask, así que arranca
 * con la consola ya en marcha y los devices montados.  Todo va por descriptores
 * de device: read()/control() sobre /dev/touch0 y /dev/power0, control()+read()
 * +write() sobre /dev/audio0 y control() sobre /dev/lcd0.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
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

/* Ventana de acceso directo del CPU a la PSRAM: la forma con caché que da
 * psram_malloc se desplaza 0x40000000 para leerla/escribirla de palabra.  Es el
 * mismo salto que usa 'audio rec' de la consola. */
#define REC_PCM_DIRECT      0x40000000U

#define DASH_MAXPTS         5U       /* capacidad del bloque de puntos FT6x36 */
#define DASH_BYTES_PER_MS   192U     /* 48 kHz * 2 canales * 2 bytes          */
#define DASH_MAX_MS         6000U    /* techo de una toma (~1,1 MB)           */
#define DASH_CHUNK          4096U    /* bytes por lectura/escritura al device */
#define DASH_POLL_MS        10U      /* cadencia del sondeo en reposo         */
#define DASH_FULLSCALE      32767U   /* tope de un int16, para el medidor     */

/* Durante la toma el micro vuelca 192000 B/s al anillo y el CPU hay que
 * drenarlo por debajo de ese ritmo o el AHB-DMA da vueltas y tira capturas:
 * entonces la grabacion dura menos que el pulso y al reproducir suena muy
 * rapido.  El anillo mide 64 kB (~330 ms), asi que basta con no dormir cuando
 * hay dato; estos topes son la cadencia a la que se hacen los trabajos LENTOS
 * (pintar el medidor por GPSPI2, sondear el tacto y la bateria por I2C), para
 * que no se paguen en cada micro-lectura. */
#define DASH_METER_MS       10U      /* redraw del medidor (~25 fps)          */
#define DASH_TOUCH_MS       15U      /* sondeo del dedo durante la toma       */
#define DASH_BATT_MS        500U     /* refresco de la bateria                */
#define DASH_STALL_MS       600U     /* sin dato del micro este rato: se rinde*/
#define DASH_RELEASE_MISS   3U       /* lecturas de tacto en 0 para dar por
                                      * soltado: un 0 suelto es un glitch, no
                                      * una suelta, y cortaria la toma antes. */

/* Geometría del panel (320x480 vertical) para esta demo. */
#define BTN_X               60
#define BTN_Y               210
#define BTN_W               200
#define BTN_H               120
#define METER_X             40
#define METER_Y             150
#define METER_W             240
#define METER_H             24

/**
 * Rellenar un rectángulo en el panel.
 */
static void dash_fill(int lcd, int x, int y, int w, int h, uint16_t color)
{
    struct lcd_rect r;

    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    r.color = color;
    control(lcd, LCD_CTRL_RECT, (long)&r, 0);
}

/**
 * Pintar una cadena con la fuente de 8x16 del panel.
 */
static void dash_text(int lcd, const char *s, int x, int y, uint16_t fg,
                      uint16_t bg)
{
    struct lcd_text t;

    t.s = s;
    t.x = x;
    t.y = y;
    t.fg = fg;
    t.bg = bg;
    control(lcd, LCD_CTRL_TEXT, (long)&t, 0);
}

/**
 * Cadena centrada horizontalmente en el panel, para el rótulo del botón.
 */
static void dash_text_center(int lcd, const char *s, int y, uint16_t fg,
                             uint16_t bg)
{
    int w = (int)strlen(s) * LCD_TEXT_W;
    int x = (LCD_W - w) / 2;

    if (x < 0)
    {
        x = 0;
    }
    dash_text(lcd, s, x, y, fg, bg);
}

/**
 * Duplicar el canal izquierdo sobre el derecho, trama a trama.
 *
 * El ES8311 es mono: el ADC vuelca la muestra solo en el slot izquierdo y deja
 * el derecho a 0, y en esta placa el DAC reproduce el slot contrario al que
 * saca el ADC.  Sin duplicar, la reproducción de una toma sale muda.  Es el
 * mismo arreglo que 'audio rec' de la consola.
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

/**
 * Pico (valor absoluto máximo) de un trozo de muestras int16 little-endian.
 */
static uint32_t dash_peak(const uint8_t *p, uint32_t n)
{
    uint32_t i;
    uint32_t peak = 0U;

    for (i = 0U; i + 1U < n; i += 2U)
    {
        int16_t v = (int16_t)((uint16_t)p[i] | ((uint16_t)p[i + 1U] << 8));
        uint32_t a = (v < 0) ? (uint32_t)(-v) : (uint32_t)v;

        if (a > peak)
        {
            peak = a;
        }
    }
    return peak;
}

/**
 * Decir si algún punto del reporte cae dentro del rectángulo del botón.
 */
static bool dash_in_button(const touch_point_t *pts, int n)
{
    int i;

    for (i = 0; i < n; i++)
    {
        int x = pts[i].x;
        int y = pts[i].y;

        if (x >= BTN_X && x < BTN_X + BTN_W &&
            y >= BTN_Y && y < BTN_Y + BTN_H)
        {
            return true;
        }
    }
    return false;
}

/**
 * Refrescar la lectura de batería en la esquina superior derecha.
 */
static void dash_battery(int lcd, int powerfd)
{
    long pct;
    char s[16];
    int x;
    int barw;

    pct = control(powerfd, POWER_CTRL_PERCENT, 0, 0);
    if (SYSERR == pct)
    {
        pct = 0;
    }
    sprintf(s, "Bateria %ld%%", pct);

    x = LCD_W - (int)strlen(s) * LCD_TEXT_W - 4;
    if (x < 0)
    {
        x = 0;
    }
    dash_fill(lcd, x - 4, 4, LCD_W - (x - 4), LCD_TEXT_H + 4, C_BLACK);
    dash_text(lcd, s, x, 6, C_AMBER, C_BLACK);

    /* Una barrita de repaso bajo el texto, para que el nivel se vea de reojo. */
    barw = (int)(pct * 60L / 100L);
    dash_fill(lcd, LCD_W - 64, 24, 60, 6, C_GRAY);
    dash_fill(lcd, LCD_W - 64, 24, barw, 6, C_GREEN);
}

/**
 * Dibujar el medidor de nivel con el pico de la última trama leída.
 */
static void dash_meter(int lcd, uint32_t peak)
{
    int fill;

    fill = (int)(peak * METER_W / DASH_FULLSCALE);
    if (fill > METER_W)
    {
        fill = METER_W;
    }
    dash_fill(lcd, METER_X, METER_Y, METER_W, METER_H, C_DARKBLUE);
    dash_fill(lcd, METER_X, METER_Y, fill, METER_H, C_CYAN);
}

/**
 * Grabar mientras el dedo no se levante, pintando el nivel en vivo.
 *
 * Devuelve los bytes de PCM (estéreo duplicado) dejados en @p recbuf.
 */
static uint32_t dash_record(int lcd, int touchfd, int powerfd, int audiofd,
                            uint8_t *recbuf, uint32_t cap)
{
    uint32_t recLen = 0U;
    uint32_t lastMeter, lastTouch, lastBatt, lastData, now;
    uint32_t peak = 0U;
    uint32_t miss = 0U;
    touch_point_t pts[DASH_MAXPTS];

    dash_fill(lcd, METER_X, METER_Y, METER_W, METER_H, C_DARKBLUE);
    dash_text_center(lcd, "GRABANDO", BTN_Y - 24, C_RED, C_BLACK);

    if (SYSERR == control(audiofd, AUDIO_CTRL_IN, 1, 0))
    {
        dash_text_center(lcd, "sin micro", LCD_H - 40, C_RED, C_BLACK);
        return 0U;
    }

    /* El drenaje va suelto (sin dormir cuando hay dato) para no dejar que el
     * anillo de entrada rebose; los trabajos lentos se hacen por tiempo.  Se
     * arranca asumiendo el dedo puesto y sin sondear el tacto los primeros
     * DASH_TOUCH_MS, que el primer reporte puede tardar en llegar. */
    lastMeter = lastTouch = lastBatt = lastData = clktotal;

    for (;;)
    {
        uint32_t chunk = cap - recLen;
        int got;
        int down;

        if (chunk > DASH_CHUNK)
        {
            chunk = DASH_CHUNK;
        }

        got = (int)read(audiofd, recbuf + recLen, chunk);
        now = clktotal;
        if (got > 0)
        {
            lastData = now;
            dash_dup_mono(recbuf + recLen, (uint32_t)got);
            peak = dash_peak(recbuf + recLen, (uint32_t)got);
            recLen += (uint32_t)got;
        }

        if (now - lastMeter >= DASH_METER_MS)
        {
            lastMeter = now;
            dash_meter(lcd, peak);
        }

        if (now - lastBatt >= DASH_BATT_MS)
        {
            lastBatt = now;
            dash_battery(lcd, powerfd);
        }

        /* Suelta confirmada con debounce: un reporte en 0 suelto es un glitch
         * del FT6x36, no el fin del pulso.  Tres en fila si que son soltar. */
        if (now - lastTouch >= DASH_TOUCH_MS)
        {
            lastTouch = now;
            down = (int)read(touchfd, pts, DASH_MAXPTS);
            if (down > 0)
            {
                miss = 0U;
            }
            else if (++miss >= DASH_RELEASE_MISS)
            {
                break;
            }
        }

        if (recLen >= cap)
        {
            break;
        }

        if (0 == got)
        {
            if (now - lastData >= DASH_STALL_MS)
            {
                break;
            }
            sleep(1);
        }
    }

    control(audiofd, AUDIO_CTRL_IN, 0, 0);
    return recLen;
}

/**
 * Reproducir por el stream de salida lo grabado en @p recbuf.
 */
static void dash_play(int lcd, int audiofd, const uint8_t *recbuf,
                      uint32_t len)
{
    uint32_t off = 0U;

    if (0U == len)
    {
        return;
    }
    dash_text_center(lcd, "REPRODUCIENDO", BTN_Y - 24, C_GREEN, C_BLACK);

    if (SYSERR == control(audiofd, AUDIO_CTRL_STREAM, 1, 0))
    {
        dash_text_center(lcd, "sin salida", LCD_H - 40, C_RED, C_BLACK);
        return;
    }
    while (off < len)
    {
        uint32_t chunk = len - off;
        long w;

        if (chunk > DASH_CHUNK)
        {
            chunk = DASH_CHUNK;
        }
        w = write(audiofd, recbuf + off, chunk);
        if (w <= 0)
        {
            break;
        }
        off += (uint32_t)w;
    }
    control(audiofd, AUDIO_CTRL_STREAM_WAIT, 0, 0);
    control(audiofd, AUDIO_CTRL_STREAM, 0, 0);
}

/**
 * Pantalla de reposo: rótulo, batería y botón vacío esperando el dedo.
 */
static void dash_idle_screen(int lcd, int touchfd, int powerfd)
{
    dash_fill(lcd, 0, 0, LCD_W, LCD_H, C_BLACK);
    dash_text_center(lcd, "PULSA Y MANTEN", BTN_Y + 40, C_WHITE, C_BLUE);
    dash_text_center(lcd, "habla y suelta", BTN_Y + BTN_H + 12, C_GRAY,
                     C_BLACK);
    dash_fill(lcd, BTN_X, BTN_Y, BTN_W, BTN_H, C_BLUE);
    dash_fill(lcd, BTN_X + 3, BTN_Y + 3, BTN_W - 6, BTN_H - 6, C_DARKBLUE);
    dash_battery(lcd, powerfd);
    (void)touchfd;
}

/**
 * Cuerpo de la tarea del dashboard.
 *
 * Bucle sin fin: espera el pulso en el botón, graba mientras se mantiene,
 * reproduce al soltar y vuelve al reposo.
 */
int dashTask(void)
{
    int lcd, touchfd, powerfd, audiofd;
    touch_point_t pts[DASH_MAXPTS];
    uint8_t *cached;
    uint8_t *recbuf;
    uint32_t cap;
    int n;

    lcd = getdev("/dev/lcd0");
    touchfd = getdev("/dev/touch0");
    powerfd = getdev("/dev/power0");
    audiofd = getdev("/dev/audio0");

    if (SYSERR == lcd || SYSERR == touchfd || SYSERR == audiofd)
    {
        return 0;
    }

    cap = DASH_MAX_MS * DASH_BYTES_PER_MS;
    cached = (uint8_t *)psram_malloc(cap);
    if (NULL == cached)
    {
        return 0;
    }
    recbuf = (uint8_t *)((uintptr_t)cached + REC_PCM_DIRECT);

    /* Techo limpio de volumen (0 dB): el 100 % es realce que envuelve. */
    control(audiofd, AUDIO_CTRL_VOLUME, 80, 0);

    dash_idle_screen(lcd, touchfd, powerfd);

    for (;;)
    {
        n = (int)read(touchfd, pts, DASH_MAXPTS);
        if (n < 0)
        {
            n = 0;
        }

        if (n > 0 && dash_in_button(pts, n))
        {
            uint32_t len;

            dash_fill(lcd, BTN_X + 3, BTN_Y + 3, BTN_W - 6, BTN_H - 6, C_RED);
            len = dash_record(lcd, touchfd, powerfd, audiofd, recbuf, cap);

            dash_fill(lcd, BTN_X + 3, BTN_Y + 3, BTN_W - 6, BTN_H - 6, C_DARKBLUE);
            dash_play(lcd, audiofd, recbuf, len);

            /* Se vuelve al reposo sin repintar todo si el dedo aún está puesto,
             * para no relanzar la captura al instante de un toque largo. */
            while ((n = (int)read(touchfd, pts, DASH_MAXPTS)) > 0)
            {
                sleep(DASH_POLL_MS);
            }
            dash_idle_screen(lcd, touchfd, powerfd);
        }
        sleep(DASH_POLL_MS);
    }
}
