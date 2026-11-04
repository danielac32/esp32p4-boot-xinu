/*
 * App ELF freestanding: figuras geometricas por consola.
 *
 * Sin cabeceras, malloc, libc enlazada ni helper.o.
 * Unico simbolo externo requerido: printf.
 *
 * Mantener las opciones de compilacion de tu app ELF:
 *   -ffreestanding -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie
 *   -mno-relax -msmall-data-limit=0
 *   arquitectura del kernel sin extension c
 *
 * La terminal debe interpretar ANSI para animar en el mismo sitio.
 */

extern int printf(const char *fmt, ...);

#define WIDTH       61
#define HEIGHT      19
#define FRAMES      120U

/*
 * 1: terminal ANSI, actualiza el dibujo en el mismo sitio.
 * 0: consola simple, imprime cada fotograma debajo del anterior.
 */
#define USE_ANSI    1

static const char title[] = "Xinu ELF - Figuras geometricas";

static int absolute(int value)
{
    return value < 0 ? -value : value;
}

/* Oscilacion triangular: 0,1,...,limit,...,1,0,... */
static int oscillate(unsigned frame, unsigned limit)
{
    unsigned position = frame % (2U * limit);

    if (position > limit)
        position = 2U * limit - position;

    return (int)position;
}

static char pixel(int x, int y, unsigned frame)
{
    int dx;
    int dy;
    int radius;
    int distance;
    int phase;

    /* Marco exterior. */
    if ((x == 0 || x == WIDTH - 1) &&
        (y == 0 || y == HEIGHT - 1))
        return '+';

    if (y == 0 || y == HEIGHT - 1)
        return '-';

    if (x == 0 || x == WIDTH - 1)
        return '|';

    /*
     * Rombo izquierdo.
     * Dos columnas por unidad horizontal para compensar
     * aproximadamente la forma alargada de los caracteres.
     */
    dx = absolute(x - 10);
    dy = absolute(y - 9);
    radius = 2 + oscillate(frame, 5U);
    distance = dx + 2 * dy;

    if (distance >= 2 * radius &&
        distance <= 2 * radius + 1)
        return '#';

    /* Centro del rombo. */
    if (x == 10 && y == 9)
        return '+';

    /* Circulo central, con radio variable. */
    dx = x - 30;
    dy = y - 9;
    radius = 3 + oscillate(frame / 2U, 3U);

    distance = dx * dx + 4 * dy * dy;

    if (distance >= (2 * radius - 1) * (2 * radius - 1) &&
        distance <= (2 * radius + 1) * (2 * radius + 1))
        return 'o';

    /* Cuadrado derecho. */
    if (x >= 43 && x <= 57 && y >= 3 && y <= 15) {
        if (y == 3 || y == 15)
            return '=';

        if (x == 43 || x == 57)
            return '|';

        /* Diagonal que se desplaza por el interior. */
        phase = (int)(frame % 13U);

        if (((x - 44) + (y - 4) + phase) % 13 == 0)
            return '*';
    }

    return ' ';
}

int main(void)
{
    char line[WIDTH + 1];
    unsigned frame;
    int x;
    int y;

#if USE_ANSI
    /* Limpiar terminal una sola vez. */
    printf("\033[2J\033[H");
#endif

    for (frame = 0U; frame < FRAMES; ++frame) {

#if USE_ANSI
        /* Volver al inicio sin limpiar entre fotogramas. */
        printf("\033[H");
#endif

        printf("%s\r\n", title);
        printf("Fotograma %3u / %3u\r\n", frame + 1U, FRAMES);
        printf("       ROMBO               CIRCULO           CUADRADO\r\n");

        for (y = 0; y < HEIGHT; ++y) {
            for (x = 0; x < WIDTH; ++x)
                line[x] = pixel(x, y, frame);

            line[WIDTH] = '\0';

            /* Una llamada por fila, no por cada caracter. */
            printf("%s\r\n", line);
        }

        printf("            Geometria ASCII mediante printf\r\n");
    }

    while(1){}
    return 0;
}
