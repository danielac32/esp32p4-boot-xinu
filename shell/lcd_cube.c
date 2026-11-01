#include <stdint.h>
#include <stdbool.h>
#include <math.h>

/* Ajusta la ruta si guardas el archivo en otra carpeta. */
#include "../device/lcd-esp32p4/lcdhw.h"

#define CUBE_SCREEN_W 320
#define CUBE_SCREEN_H 480

#define COLOR_BLACK 0x0000
#define COLOR_CYAN  0x07FF

typedef struct {
    float x;
    float y;
    float z;
} Vertex3D;

typedef struct {
    int x;
    int y;
} Point2D;

static const Vertex3D vertices[8] = {
    {-1.0f, -1.0f, -1.0f},
    { 1.0f, -1.0f, -1.0f},
    { 1.0f,  1.0f, -1.0f},
    {-1.0f,  1.0f, -1.0f},
    {-1.0f, -1.0f,  1.0f},
    { 1.0f, -1.0f,  1.0f},
    { 1.0f,  1.0f,  1.0f},
    {-1.0f,  1.0f,  1.0f}
};

static const uint8_t edges[12][2] = {
    {0, 1}, {1, 2}, {2, 3}, {3, 0},
    {4, 5}, {5, 6}, {6, 7}, {7, 4},
    {0, 4}, {1, 5}, {2, 6}, {3, 7}
};

static Point2D previous[8];
static bool previous_valid;

static float angle_x;
static float angle_y;

static int int_abs(int value)
{
    return value < 0 ? -value : value;
}

static void put_pixel(int x, int y, uint16_t color)
{
    if (x >= 0 && x < CUBE_SCREEN_W &&
        y >= 0 && y < CUBE_SCREEN_H) {
        lcdPanelPixel(x, y, color);
    }
}

/* Algoritmo de Bresenham. */
static void draw_line(int x0, int y0,
                      int x1, int y1,
                      uint16_t color)
{
    int dx = int_abs(x1 - x0);
    int dy = -int_abs(y1 - y0);

    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;

    int error = dx + dy;

    for (;;) {
        put_pixel(x0, y0, color);

        if (x0 == x1 && y0 == y1) {
            break;
        }

        int twice_error = 2 * error;

        if (twice_error >= dy) {
            error += dy;
            x0 += sx;
        }

        if (twice_error <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

static void draw_edges(const Point2D points[8],
                       uint16_t color)
{
    for (int i = 0; i < 12; ++i) {
        unsigned a = edges[i][0];
        unsigned b = edges[i][1];

        draw_line(
            points[a].x, points[a].y,
            points[b].x, points[b].y,
            color
        );
    }
}

/* Llamar una vez al iniciar la animación. */
void lcdCubeStart(void)
{
    angle_x = 0.0f;
    angle_y = 0.0f;
    previous_valid = false;

    lcdPanelFill(COLOR_BLACK);
}

/*
 * Dibuja un fotograma.
 *
 * dt_seconds: segundos transcurridos.
 * Ejemplo: 0.040f para pasos de 40 ms.
 */
void lcdCubeFrame(float dt_seconds)
{
    const float two_pi = 6.28318530718f;
    Point2D current[8];

    if (!(dt_seconds > 0.0f)) {
        return;
    }

    if (dt_seconds > 0.1f) {
        dt_seconds = 0.1f;
    }

    angle_x += 0.70f * dt_seconds;
    angle_y += 1.10f * dt_seconds;

    if (angle_x >= two_pi) {
        angle_x -= two_pi;
    }

    if (angle_y >= two_pi) {
        angle_y -= two_pi;
    }

    /* Funciones disponibles en tu biblioteca matemática. */
    float sx = sinef(angle_x, 0);
    float cx = sinef(angle_x, 1);
    float sy = sinef(angle_y, 0);
    float cy = sinef(angle_y, 1);

    for (int i = 0; i < 8; ++i) {
        float x = vertices[i].x;
        float y = vertices[i].y;
        float z = vertices[i].z;

        /* Rotación sobre X. */
        float rx = x;
        float ry = y * cx - z * sx;
        float rz = y * sx + z * cx;

        /* Rotación sobre Y. */
        float tx = rx * cy + rz * sy;
        float ty = ry;
        float tz = -rx * sy + rz * cy;

        /* Proyección con perspectiva. */
        float perspective = 210.0f / (tz + 4.0f);

        current[i].x =
            CUBE_SCREEN_W / 2 + (int)(tx * perspective);

        current[i].y =
            CUBE_SCREEN_H / 2 - (int)(ty * perspective);
    }

    bool changed = !previous_valid;

    for (int i = 0; i < 8 && !changed; ++i) {
        if (current[i].x != previous[i].x ||
            current[i].y != previous[i].y) {
            changed = true;
        }
    }

    if (!changed) {
        return;
    }

    if (previous_valid) {
        draw_edges(previous, COLOR_BLACK);
    }

    draw_edges(current, COLOR_CYAN);

    for (int i = 0; i < 8; ++i) {
        previous[i] = current[i];
    }

    previous_valid = true;
}
