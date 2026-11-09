#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <kernel.h>
#include <clock.h>
#include <device.h>
#include <lcd.h>
#include <touch.h>
#include <audio.h>
#include <memory.h>
#include <timer.h>

/*
 * Grabadora vintage para LCD vertical 320 x 480.
 *
 * HABLAR: mantener pulsado para grabar.
 * Soltar: reproducir automaticamente.
 * REPETIR: escuchar la ultima grabacion.
 *
 * Ejecutar UNA sola instancia.
 * No ejecutar p4demoTask, audioDemoTask u otra tarea
 * que utilice simultaneamente LCD, touch o audio.
 *
 * Conserva las APIs y el alias PSRAM del codigo original.
 */

/* ---------- Audio ---------- */

#define VA_PCM_DIRECT    0x40000000U
#define VA_BYTES_MS      192U
#define VA_CHUNK         4096U
#define VA_VOLUME        80U

#define VA_POINTS        5U
#define VA_RELEASE_MISS  3U
#define VA_POLL          15U
#define VA_FRAME_MS      60U
#define VA_STALL_MS      600U

/* ---------- Colores ---------- */

#define VA_BG      RGB565(232,218,187)
#define VA_INK     RGB565(65,46,32)
#define VA_MUTED   RGB565(128,106,77)
#define VA_PANEL   RGB565(207,187,149)
#define VA_PAPER   RGB565(246,233,202)
#define VA_DARK    RGB565(41,48,38)
#define VA_GRID    RGB565(62,72,49)
#define VA_GREEN   RGB565(166,192,106)
#define VA_ORANGE  RGB565(186,83,42)
#define VA_RED     RGB565(163,52,38)

/* ---------- Pantalla ---------- */

#define VA_SX      32
#define VA_SY      203
#define VA_SW      256
#define VA_SH      84
#define VA_BARS    32

#define VA_BX      24
#define VA_BY      337
#define VA_BW      272
#define VA_BH      60

#define VA_RX      24
#define VA_RY      408
#define VA_RW      272
#define VA_RH      40

enum va_state {
    VA_IDLE,
    VA_RECORD,
    VA_PLAY
};

static int va_lcd;
static int va_touch;
static int va_audio;

static uint8_t *va_cached;
static uint8_t *va_pcm;
static uint32_t va_capacity;
static uint32_t va_length;
static uint32_t va_record_peak;

static enum va_state va_state_now;
static const char *va_error;

static uint8_t va_history[VA_BARS];
static int va_last_seconds = -1;
static int va_last_progress = -1;
static char va_last_hint[64];

/* ---------- Utilidades ---------- */

static uint32_t va_now(void)
{
    return (uint32_t)millis();
}

static bool va_inside(
    int x, int y, int bx, int by, int bw, int bh
)
{
    return x >= bx && x < bx + bw
        && y >= by && y < by + bh;
}

static void va_rect(
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

    control(va_lcd, LCD_CTRL_RECT, (long)&r, 0);
}

static void va_round(
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
        va_rect(x, y, w, h, color);
        return;
    }

    va_rect(x, y + radius, w, h - 2 * radius, color);

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

        va_rect(
            x + inset, y + row,
            w - 2 * inset, 1, color
        );

        va_rect(
            x + inset, y + h - row - 1,
            w - 2 * inset, 1, color
        );
    }
}

static void va_text(
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

    control(va_lcd, LCD_CTRL_TEXT, (long)&t, 0);
}

static void va_center(
    const char *s, int y,
    uint16_t fg, uint16_t bg
)
{
    int x = (LCD_W - (int)strlen(s) * LCD_TEXT_W) / 2;

    if (x < 0)
        x = 0;

    va_text(s, x, y, fg, bg);
}

static void va_hint(const char *message)
{
    if (strcmp(va_last_hint, message) == 0)
        return;

    va_rect(8, 457, LCD_W - 16, 20, VA_BG);
    va_center(message, 459, VA_MUTED, VA_BG);

    snprintf(
        va_last_hint, sizeof(va_last_hint),
        "%s", message
    );
}

/* ---------- Interfaz vintage ---------- */

static void va_reel(int cx, int cy)
{
    int i;

    static const int offsets[4][2] = {
        {-9, -9}, {9, -9},
        {-9,  9}, {9,  9}
    };

    va_round(cx - 27, cy - 27, 54, 54, 27, VA_INK);
    va_round(cx - 23, cy - 23, 46, 46, 23, VA_PAPER);

    for (i = 0; i < 4; ++i) {
        va_round(
            cx + offsets[i][0] - 5,
            cy + offsets[i][1] - 5,
            10, 10, 5, VA_PANEL
        );
    }

    va_round(cx - 5, cy - 5, 10, 10, 5, VA_INK);
}

static void va_buttons(void)
{
    uint16_t color = va_state_now == VA_RECORD
        ? VA_RED : VA_ORANGE;

    const char *label = va_state_now == VA_RECORD
        ? "GRABANDO..." : "HABLAR";

    va_round(
        VA_BX, VA_BY + 3, VA_BW, VA_BH,
        12, VA_INK
    );

    va_round(
        VA_BX, VA_BY, VA_BW, VA_BH,
        12, color
    );

    va_center(label, VA_BY + 12, VA_PAPER, color);
    va_center(
        "MANTEN PULSADO",
        VA_BY + 35, VA_PAPER, color
    );

    va_round(
        VA_RX, VA_RY, VA_RW, VA_RH,
        10, VA_PANEL
    );

    va_center(
        va_state_now == VA_PLAY
            ? "REPRODUCIENDO..." : "REPETIR",
        VA_RY + 12,
        va_length > 0U ? VA_INK : VA_MUTED,
        VA_PANEL
    );
}

static void va_state_set(enum va_state state)
{
    const char *label;

    if (state == va_state_now)
        return;

    va_state_now = state;

    label = state == VA_RECORD ? "REC / MICROFONO"
          : state == VA_PLAY ? "PLAY / ALTAVOZ"
          : "LISTO / VOZ";

    va_rect(32, 180, 256, 18, VA_PANEL);
    va_center(label, 181, VA_INK, VA_PANEL);

    va_buttons();
}

/*
 * Historial de amplitud real, no FFT.
 * Solo redibuja las columnas que cambian.
 */
static void va_scope(uint32_t peak, bool reset)
{
    uint8_t next[VA_BARS];
    unsigned i;

    if (reset) {
        memset(next, 0, sizeof(next));
    } else {
        unsigned height = 0U;

        if (peak > 32767U)
            peak = 32767U;

        while (
            height < 36U &&
            (height + 1U) * (height + 1U) * 32767U
                <= peak * 36U * 36U
        )
            ++height;

        for (i = 0; i + 1U < VA_BARS; ++i)
            next[i] = va_history[i + 1U];

        next[VA_BARS - 1U] = (uint8_t)height;
    }

    for (i = 0; i < VA_BARS; ++i) {
        int x = VA_SX + (int)i * 8;
        int h = next[i];
        int y;

        if (!reset && next[i] == va_history[i])
            continue;

        va_rect(x, VA_SY, 8, VA_SH, VA_DARK);

        for (y = 14; y < VA_SH; y += 14)
            va_rect(x, VA_SY + y, 8, 1, VA_GRID);

        if ((i % 4U) == 0U)
            va_rect(x, VA_SY, 1, VA_SH, VA_GRID);

        va_rect(
            x + 1,
            VA_SY + VA_SH / 2 - h,
            5,
            h > 0 ? 2 * h + 1 : 1,
            VA_GREEN
        );
    }

    memcpy(va_history, next, sizeof(next));
}

static void va_progress(uint32_t bytes)
{
    char text[32];
    int seconds;
    int maximum;
    int width;

    seconds = (int)(bytes / VA_BYTES_MS / 1000U);
    maximum = (int)(va_capacity / VA_BYTES_MS / 1000U);

    width = va_capacity > 0U
        ? (int)(bytes * 256U / va_capacity) : 0;

    if (width > 256)
        width = 256;

    if (seconds != va_last_seconds) {
        va_last_seconds = seconds;

        snprintf(
            text, sizeof(text),
            "%02d:%02d / %02d:%02d",
            seconds / 60, seconds % 60,
            maximum / 60, maximum % 60
        );

        va_rect(32, 299, 256, 18, VA_BG);
        va_center(text, 300, VA_INK, VA_BG);
    }

    if (width != va_last_progress) {
        va_last_progress = width;

        va_rect(32, 324, 256, 4, VA_PANEL);

        if (width > 0)
            va_rect(32, 324, width, 4, VA_ORANGE);
    }
}

static void va_draw(void)
{
    va_rect(0, 0, LCD_W, LCD_H, VA_BG);

    va_text("AUDIO", 24, 18, VA_MUTED, VA_BG);
    va_rect(24, 43, 272, 2, VA_INK);

   // va_center("VOZ ANALOGICA", 57, VA_INK, VA_BG);

    va_round(24, 89, 272, 83, 12, VA_PANEL);
    va_reel(91, 130);
    va_reel(229, 130);

    va_rect(122, 121, 76, 18, VA_INK);

    va_text(
        "TAPE", 160 - 2 * LCD_TEXT_W,
        122, VA_PAPER, VA_INK
    );

    va_round(24, 175, 272, 118, 10, VA_PANEL);
    va_rect(VA_SX, VA_SY, VA_SW, VA_SH, VA_DARK);

    va_last_hint[0] = '\0';
    va_last_seconds = -1;
    va_last_progress = -1;

    va_state_now = VA_PLAY;
    va_state_set(VA_IDLE);

    va_scope(0U, true);
    va_progress(0U);
    va_hint("Habla y suelta para oir");
}

/* ---------- Memoria ---------- */

static int va_prepare(void)
{
    unsigned i;
    static const uint32_t seconds[] = {20U, 10U, 6U};

    if (va_pcm != NULL)
        return OK;

    for (
        i = 0;
        i < sizeof(seconds) / sizeof(seconds[0]);
        ++i
    ) {
        uint32_t capacity =
            VA_BYTES_MS * 1000U * seconds[i];

        va_cached = (uint8_t *)psram_malloc(capacity);

        if (va_cached != NULL) {
            va_capacity = capacity;

            va_pcm = (uint8_t *)(
                (uintptr_t)va_cached + VA_PCM_DIRECT
            );

            return OK;
        }
    }

    return SYSERR;
}

/* ---------- PCM ---------- */

static uint32_t va_peak(
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

        int32_t value = sample;

        uint32_t amplitude = value < 0
            ? (uint32_t)(-value) : (uint32_t)value;

        if (amplitude > peak)
            peak = amplitude;
    }

    return peak;
}

static void va_duplicate_mono(uint32_t length)
{
    uint32_t i;

    for (i = 0U; i + 3U < length; i += 4U) {
        va_pcm[i + 2U] = va_pcm[i];
        va_pcm[i + 3U] = va_pcm[i + 1U];
    }
}

/* ---------- Touch ---------- */

static int va_read_touch(touch_point_t *points)
{
    int count = (int)read(
        va_touch, points, VA_POINTS
    );

    if (count > (int)VA_POINTS)
        return SYSERR;

    return count;
}

static int va_wait_release(void)
{
    touch_point_t points[VA_POINTS];
    unsigned misses = 0U;

    while (misses < VA_RELEASE_MISS) {
        int count = va_read_touch(points);

        if (count < 0)
            return SYSERR;

        if (count == 0)
            ++misses;
        else
            misses = 0U;

        sleep(VA_POLL);
    }

    return OK;
}

/* ---------- Grabacion ---------- */

static uint32_t va_record(void)
{
    touch_point_t points[VA_POINTS];
    uint32_t length = 0U;
    uint32_t last_frame;
    uint32_t last_touch;
    uint32_t last_data;
    uint32_t peak_window = 0U;
    long result;
    unsigned misses = 0U;

    va_record_peak = 0U;

    va_state_set(VA_RECORD);
    va_scope(0U, true);
    va_progress(0U);
    va_hint("Suelta para escuchar");

    result = control(
        va_audio, AUDIO_CTRL_IN, 1, 0
    );

    printf("vintageAudio: iniciar micro=%ld\n", result);

    if (result == SYSERR) {
        va_error = "No inicia el micro";
        return 0U;
    }

    last_frame = va_now();
    last_touch = last_frame;
    last_data = last_frame;

    while (length < va_capacity) {
        uint32_t chunk = va_capacity - length;
        uint32_t now;
        int got;

        if (chunk > VA_CHUNK)
            chunk = VA_CHUNK;

        got = (int)read(
            va_audio, va_pcm + length, chunk
        );

        now = va_now();

        if (got > 0) {
            uint32_t peak;

            if ((uint32_t)got > chunk) {
                va_error = "Tamano PCM incorrecto";
                break;
            }

            peak = va_peak(
                va_pcm + length, (uint32_t)got
            );

            if (peak > peak_window)
                peak_window = peak;

            if (peak > va_record_peak)
                va_record_peak = peak;

            length += (uint32_t)got;
            last_data = now;
        } else if (got < 0) {
            va_error = "Error leyendo micro";
            break;
        }

        if (
            (uint32_t)(now - last_touch)
            >= VA_POLL
        ) {
            int count;

            last_touch = now;
            count = va_read_touch(points);

            if (count < 0) {
                va_error = "Error de touch";
                break;
            }

            if (count == 0) {
                if (++misses >= VA_RELEASE_MISS)
                    break;
            } else {
                misses = 0U;
            }
        }

        if (
            (uint32_t)(now - last_frame)
            >= VA_FRAME_MS
        ) {
            last_frame = now;

            va_scope(peak_window, false);
            va_progress(length);

            peak_window = 0U;
        }

        if (got == 0) {
            if (
                (uint32_t)(now - last_data)
                >= VA_STALL_MS
            ) {
                va_error = "Micro sin datos";
                break;
            }

            sleep(1);
        }
    }

    result = control(
        va_audio, AUDIO_CTRL_IN, 0, 0
    );

    if (result == SYSERR && va_error == NULL)
        va_error = "No se detiene el micro";

    length -= length % 4U;
    va_duplicate_mono(length);

    va_progress(length);

    printf(
        "vintageAudio: grabados=%lu bytes, "
        "pico entrada=%lu, pico salida=%lu, error=%s\n",
        (unsigned long)length,
        (unsigned long)va_record_peak,
        (unsigned long)va_peak(va_pcm, length),
        va_error != NULL ? va_error : "ninguno"
    );

    return length;
}

/* ---------- Reproduccion ---------- */

static void va_play(void)
{
    uint32_t offset = 0U;
    uint32_t last_frame;
    uint32_t peak_window = 0U;
    long result;

    if (va_length == 0U) {
        va_error = "No hay audio grabado";
        return;
    }

    va_state_set(VA_PLAY);
    va_scope(0U, true);
    va_progress(0U);
    va_hint("Escuchando tu grabacion");

    /* Recupera el volumen del codigo funcional. */
    result = control(
        va_audio, AUDIO_CTRL_VOLUME, VA_VOLUME, 0
    );

    printf(
        "vintageAudio: volumen=%u, resultado=%ld\n",
        (unsigned)VA_VOLUME,
        result
    );

    if (result == SYSERR) {
        va_error = "Error ajustando volumen";
        return;
    }

    result = control(
        va_audio, AUDIO_CTRL_STREAM, 1, 0
    );

    printf(
        "vintageAudio: iniciar salida=%ld\n",
        result
    );

    if (result == SYSERR) {
        va_error = "No inicia el altavoz";
        return;
    }

    last_frame = va_now();

    while (offset < va_length) {
        uint32_t chunk = va_length - offset;
        uint32_t peak;
        uint32_t now;
        long written;

        if (chunk > VA_CHUNK)
            chunk = VA_CHUNK;

        written = write(
            va_audio, va_pcm + offset, chunk
        );

        if (offset == 0U) {
            printf(
                "vintageAudio: primer bloque "
                "enviado=%lu, aceptado=%ld\n",
                (unsigned long)chunk,
                written
            );
        }

        if (
            written <= 0 ||
            (uint32_t)written > chunk
        ) {
            va_error = "Error de reproduccion";
            break;
        }

        peak = va_peak(
            va_pcm + offset, (uint32_t)written
        );

        if (peak > peak_window)
            peak_window = peak;

        offset += (uint32_t)written;
        now = va_now();

        if (
            (uint32_t)(now - last_frame)
            >= VA_FRAME_MS
        ) {
            last_frame = now;

            va_scope(peak_window, false);
            va_progress(offset);

            peak_window = 0U;
        }
    }

    /*
     * Esperar a que termine la salida antes de cerrar.
     * Mismo orden que el codigo original funcional.
     */
    result = control(
        va_audio, AUDIO_CTRL_STREAM_WAIT, 0, 0
    );

    printf(
        "vintageAudio: enviados=%lu bytes, espera=%ld\n",
        (unsigned long)offset,
        result
    );

    if (result == SYSERR && va_error == NULL)
        va_error = "Error esperando audio";

    result = control(
        va_audio, AUDIO_CTRL_STREAM, 0, 0
    );

    if (result == SYSERR && va_error == NULL)
        va_error = "Error cerrando salida";

    va_progress(offset);
}

/* ---------- Tarea principal ---------- */

int audioDemoTask(void)
{
    touch_point_t points[VA_POINTS];
    bool latched = false;
    unsigned release_count = 0U;
    bool touch_error_reported = false;

    va_lcd = getdev("/dev/lcd0");
    va_touch = getdev("/dev/touch0");
    va_audio = getdev("/dev/audio0");

    if (
        va_lcd == SYSERR ||
        va_touch == SYSERR ||
        va_audio == SYSERR
    ) {
        printf(
            "vintageAudio: falta LCD, touch o audio\n"
        );
        return 0;
    }

    if (va_prepare() == SYSERR) {
        printf(
            "vintageAudio: no hay PSRAM suficiente\n"
        );
        return 0;
    }

    if (
        control(
            va_lcd, LCD_CTRL_BACKLIGHT, 1, 0
        ) == SYSERR
    ) {
        printf(
            "vintageAudio: no se pudo encender LCD\n"
        );
        return 0;
    }

    va_length = 0U;
    va_record_peak = 0U;
    va_error = NULL;

    va_draw();
    printf(
        "vintageAudio: capacidad=%lu segundos\n",
        (unsigned long)(
            va_capacity / VA_BYTES_MS / 1000U
        )
    );

    for (;;) {
        int count = va_read_touch(points);

        if (count < 0) {
            release_count = 0U;

            if (!touch_error_reported) {
                va_hint("Error de touch");
                printf("vintageAudio: error de touch\n");
                touch_error_reported = true;
            }

            sleep(VA_POLL);
            continue;
        }

        touch_error_reported = false;

        if (count == 0) {
            if (release_count < VA_RELEASE_MISS)
                ++release_count;

            if (release_count >= VA_RELEASE_MISS)
                latched = false;
        } else {
            release_count = 0U;

            if (!latched) {
                int x = points[0].x;
                int y = points[0].y;
                bool handled = false;

                latched = true;
                va_error = NULL;

                if (
                    va_inside(
                        x, y,
                        VA_BX, VA_BY, VA_BW, VA_BH
                    )
                ) {
                    handled = true;
                    va_length = va_record();

                    if (
                        va_error == NULL &&
                        va_length > 0U
                    ) {
                        va_play();
                    } else if (
                        va_error == NULL &&
                        va_length == 0U
                    ) {
                        va_error = "No se grabo audio";
                    }
                } else if (
                    va_inside(
                        x, y,
                        VA_RX, VA_RY, VA_RW, VA_RH
                    )
                ) {
                    handled = true;

                    if (va_length > 0U)
                        va_play();
                    else
                        va_error = "Primero graba tu voz";
                }

                if (handled) {
                    va_state_set(VA_IDLE);
                    va_scope(0U, true);

                    if (va_error != NULL) {
                        va_hint(va_error);

                        printf(
                            "vintageAudio: %s\n",
                            va_error
                        );
                    } else if (
                        va_peak(va_pcm, va_length) == 0U
                    ) {
                        va_hint("PCM grabado en silencio");
                    } else {
                        va_hint("Lista para otra toma");
                    }

                    if (va_wait_release() == SYSERR) {
                        va_hint("Error de touch");
                        return 0;
                    }

                    latched = false;
                    release_count = 0U;
                }
            }
        }

        /* Unidad de sleep(): la definida por tu kernel. */
        sleep(VA_POLL);
    }

    return 0;
}
