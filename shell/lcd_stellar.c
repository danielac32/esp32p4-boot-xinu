#include <stdint.h>
#include <math.h>

/* Ajusta esta ruta según dónde guardes el archivo. */
#include "../device/lcd-esp32p4/lcdhw.h"

#define SW 160
#define SH 240

#define CENTER_X (SW / 2)
#define CENTER_Y (SH / 2)

#define STAR_COUNT     60
#define PARTICLE_COUNT 90

#define PI2 6.28318530718f

typedef struct {
    int x;
    int y;
    unsigned phase;
} Star;

typedef struct {
    float dx;
    float dy;
    float speed;
    unsigned shade;
} Particle;

/* Todos los búferes grandes son estáticos. */
static uint16_t image[SH][SW];
static uint16_t output_rows[320 * 2];

static Star stars[STAR_COUNT];
static Particle particles[PARTICLE_COUNT];

static unsigned tick;
static uint32_t random_state;
static int initialized;

static uint32_t random_next(void)
{
    random_state = random_state * 1664525U + 1013904223U;
    return random_state;
}

static uint16_t rgb(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)(
        ((r & 248U) << 8) |
        ((g & 252U) << 3) |
        (b >> 3)
    );
}

static void pixel(int x, int y, uint16_t color)
{
    if (x >= 0 && x < SW && y >= 0 && y < SH)
        image[y][x] = color;
}

static void clear_image(void)
{
    for (int y = 0; y < SH; ++y)
        for (int x = 0; x < SW; ++x)
            image[y][x] = 0;
}

static void disk(int cx, int cy, int radius, uint16_t color)
{
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            if (x * x + y * y <= radius * radius)
                pixel(cx + x, cy + y, color);
        }
    }
}

static void ring(int cx, int cy, int radius,
                 int thickness, uint16_t color)
{
    int inner = radius - thickness;

    if (inner < 0)
        inner = 0;

    int outer_squared = radius * radius;
    int inner_squared = inner * inner;

    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            int distance = x * x + y * y;

            if (distance <= outer_squared &&
                distance >= inner_squared) {
                pixel(cx + x, cy + y, color);
            }
        }
    }
}

/* Órbita elíptica inclinada respecto a la pantalla. */
static void orbit(int radius, uint16_t color)
{
    for (int i = 0; i < 128; ++i) {
        float angle = PI2 * (float)i / 128.0f;

        float x = sinef(angle, 1) * (float)radius;
        float y = sinef(angle, 0) * (float)radius * 0.48f;

        pixel(
            CENTER_X + (int)x,
            CENTER_Y + (int)(y + x * 0.22f),
            color
        );
    }
}

static void draw_stars(void)
{
    for (int i = 0; i < STAR_COUNT; ++i) {
        unsigned phase = (tick + stars[i].phase) % 80U;

        unsigned brightness =
            60U + (phase < 40U ? phase : 80U - phase) * 4U;

        pixel(
            stars[i].x,
            stars[i].y,
            rgb(brightness, brightness, 255U)
        );

        if (brightness > 200U && i % 7 == 0) {
            uint16_t dim = rgb(50, 60, 100);

            pixel(stars[i].x - 1, stars[i].y, dim);
            pixel(stars[i].x + 1, stars[i].y, dim);
            pixel(stars[i].x, stars[i].y - 1, dim);
            pixel(stars[i].x, stars[i].y + 1, dim);
        }
    }
}

static void draw_planet(int index, float time, int front)
{
    static const int orbit_radius[4] = {23, 36, 52, 70};
    static const int planet_radius[4] = {2, 3, 4, 5};

    static const float speed[4] = {
        1.9f, 1.25f, 0.82f, 0.53f
    };

    static const uint8_t colors[4][3] = {
        {255, 130, 60},
        {70, 180, 255},
        {220, 100, 240},
        {230, 190, 100}
    };

    float angle = time * speed[index] + (float)index;
    float depth = sinef(angle, 0);

    /* Dibujar primero los planetas que pasan detrás del sol. */
    if ((depth >= 0.0f) != (front != 0))
        return;

    float x = sinef(angle, 1) * orbit_radius[index];
    float y = depth * orbit_radius[index] * 0.48f;

    int px = CENTER_X + (int)x;
    int py = CENTER_Y + (int)(y + x * 0.22f);
    int radius = planet_radius[index];

    disk(
        px, py, radius,
        rgb(colors[index][0],
            colors[index][1],
            colors[index][2])
    );

    /* Pequeño reflejo para dar volumen. */
    pixel(px - 1, py - 1, rgb(255, 255, 220));

    /* Anillos del planeta exterior. */
    if (index == 3) {
        for (int i = -8; i <= 8; ++i) {
            pixel(
                px + i,
                py + i / 3,
                rgb(180, 145, 90)
            );
        }
    }
}

static void draw_solar_system(unsigned age)
{
    float time = (float)age * 0.04f;

    for (int i = 0; i < 4; ++i) {
        static const int radius[4] = {23, 36, 52, 70};
        orbit(radius[i], rgb(15, 28, 45));
    }

    for (int i = 0; i < 4; ++i)
        draw_planet(i, time, 0);

    int pulse = (int)(2.0f * sinef(time * 2.0f, 0));

    /* El sol crece antes de explotar. */
    int swelling = age > 210U ? (int)(age - 210U) / 5 : 0;
    int radius = 10 + pulse + swelling;

    disk(CENTER_X, CENTER_Y, radius + 5, rgb(45, 12, 2));
    disk(CENTER_X, CENTER_Y, radius + 2, rgb(160, 45, 5));
    disk(CENTER_X, CENTER_Y, radius, rgb(255, 160, 15));
    disk(CENTER_X - 2, CENTER_Y - 2,
         radius / 2, rgb(255, 240, 150));

    for (int i = 0; i < 4; ++i)
        draw_planet(i, time, 1);
}

static void draw_explosion(unsigned age)
{
    /* Onda expansiva, el radio permanece acotado. */
    int radius = 2 + (int)age;

    if (radius < 148) {
        unsigned brightness = 255U - age;

        ring(
            CENTER_X, CENTER_Y,
            radius, 2,
            rgb(brightness / 2U, brightness, 255)
        );
    }

    /* Destello inicial. */
    if (age < 12U) {
        int flash_radius = 24 - (int)age;

        disk(
            CENTER_X, CENTER_Y, flash_radius,
            rgb(255, 255, 230)
        );
    }

    for (int i = 0; i < PARTICLE_COUNT; ++i) {
        float distance =
            (float)age * particles[i].speed;

        int x = CENTER_X + (int)(particles[i].dx * distance);
        int y = CENTER_Y + (int)(particles[i].dy * distance);

        unsigned brightness = 255U - age;
        unsigned green = brightness / 3U + particles[i].shade;

        if (green > 255U)
            green = 255U;

        disk(
            x, y, age < 35U ? 2 : 1,
            rgb(brightness, green, brightness / 8U)
        );

        /* Estela hacia el centro. */
        for (int trail = 1; trail <= 3; ++trail) {
            float behind = distance - (float)(trail * 3);

            if (behind <= 0.0f)
                break;

            pixel(
                CENTER_X + (int)(particles[i].dx * behind),
                CENTER_Y + (int)(particles[i].dy * behind),
                rgb(brightness / (unsigned)(trail + 1),
                    green / (unsigned)(trail + 2),
                    0)
            );
        }
    }

    /* Remanente azul en el centro. */
    if (age >= 12U) {
        disk(CENTER_X, CENTER_Y, 4, rgb(30, 90, 180));
        disk(CENTER_X, CENTER_Y, 2, rgb(180, 230, 255));
    }
}

static void present(void)
{
    for (int y = 0; y < SH; ++y) {
        for (int x = 0; x < SW; ++x) {
            uint16_t color = image[y][x];
            int out = x * 2;

            output_rows[out] = color;
            output_rows[out + 1] = color;
            output_rows[320 + out] = color;
            output_rows[320 + out + 1] = color;
        }

        lcdPanelBlit(0, y * 2, 320, 2, output_rows);
    }
}

void lcdStellarStart(void)
{
    initialized = 0;
    random_state = 0x1234ABCDU;
    tick = 0;

    for (int i = 0; i < STAR_COUNT; ++i) {
        stars[i].x = (int)(random_next() % SW);
        stars[i].y = (int)(random_next() % SH);
        stars[i].phase = random_next() % 80U;
    }

    for (int i = 0; i < PARTICLE_COUNT; ++i) {
        float angle = PI2 * (float)i / PARTICLE_COUNT;

        particles[i].dx = sinef(angle, 1);
        particles[i].dy = sinef(angle, 0);

        particles[i].speed =
            0.45f + (float)(random_next() % 100U) * 0.012f;

        particles[i].shade = random_next() % 65U;
    }

    initialized = 1;
}

/*
 * Un fotograma por llamada:
 *   260 fotogramas de sistema solar.
 *   180 fotogramas de explosión.
 * Después vuelve a comenzar.
 */
void lcdStellarFrame(void)
{
    if (!initialized)
        return;

    clear_image();
    draw_stars();

    if (tick < 260U)
        draw_solar_system(tick);
    else
        draw_explosion(tick - 260U);

    present();

    tick++;

    if (tick >= 440U)
        tick = 0;
}
