#include <stdint.h>
#include <math.h>

/* Ajusta la ruta si guardas el archivo en otra carpeta. */
#include "../device/lcd-esp32p4/lcdhw.h"

#define PLASMA_W 320
#define PLASMA_H 480

/*
 * Calculamos a 160 x 240.
 * Cada punto ocupa 2 x 2 píxeles en pantalla.
 */
#define FIELD_W (PLASMA_W / 2)
#define FIELD_H (PLASMA_H / 2)

static int16_t wave[256];
static uint16_t palette[256];

static int16_t horizontal[FIELD_W];

/* Dos filas completas, en RGB565. */
static uint16_t rows[PLASMA_W * 2];

static uint32_t frame;
static int initialized;

static unsigned wrap(int value)
{
    return ((uint32_t)value) & 255U;
}

static uint16_t rgb565(unsigned r,
                       unsigned g,
                       unsigned b)
{
    return (uint16_t)(
        ((r & 0xF8U) << 8) |
        ((g & 0xFCU) << 3) |
        (b >> 3)
    );
}

/* Llamar una vez antes del primer fotograma. */
void lcdPlasmaStart(void)
{
    const float two_pi = 6.28318530718f;

    initialized = 0;

    /* Tabla de ondas: evita trigonometría por píxel. */
    for (int i = 0; i < 256; ++i) {
        float angle = two_pi * (float)i / 256.0f;

        wave[i] = (int16_t)(
            127.0f * sinef(angle, 0)
        );
    }

    /* Paleta de colores con canales desfasados. */
    for (int i = 0; i < 256; ++i) {
        unsigned r =
            (unsigned)(127 + wave[wrap(i)]);

        unsigned g =
            (unsigned)(127 + wave[wrap(i + 85)]);

        unsigned b =
            (unsigned)(127 + wave[wrap(i + 170)]);

        palette[i] = rgb565(r, g, b);
    }

    frame = 0;
    initialized = 1;
}

/*
 * Dibuja un fotograma completo.
 * La espera se hace en la tarea que llama a esta función.
 */
void lcdPlasmaFrame(void)
{
    if (!initialized) {
        return;
    }

    int t = (int)(frame & 4095U);

    /* Centro móvil de las ondas circulares. */
    int center_x =
        FIELD_W / 2 + wave[wrap(t / 3)] / 3;

    int center_y =
        FIELD_H / 2 + wave[wrap(t / 4 + 64)] / 2;

    for (int x = 0; x < FIELD_W; ++x) {
        horizontal[x] =
            wave[wrap(x * 3 + t * 2)];
    }

    for (int y = 0; y < FIELD_H; ++y) {
        int vertical = wave[wrap(y * 2 - t)];
        int bend = wave[wrap(y + t)] / 4;
        int dy = y - center_y;

        for (int x = 0; x < FIELD_W; ++x) {
            int dx = x - center_x;

            /* Distancia al cuadrado: no requiere sqrt(). */
            int radial_phase =
                (dx * dx + dy * dy) / 48;

            int radial =
                wave[wrap(radial_phase - t * 3)];

            int diagonal = wave[wrap(
                x * 2 + y * 2 + bend + t
            )];

            int combined =
                horizontal[x] +
                vertical +
                radial +
                diagonal;

            unsigned index = wrap(
                (combined + 508) / 4 + t
            );

            uint16_t color = palette[index];

            int out = x * 2;

            /* Primera fila. */
            rows[out]     = color;
            rows[out + 1] = color;

            /* Segunda fila: ampliación vertical. */
            rows[PLASMA_W + out]     = color;
            rows[PLASMA_W + out + 1] = color;
        }

        lcdPanelBlit(
            0,
            y * 2,
            PLASMA_W,
            2,
            rows
        );
    }

    frame++;
}
