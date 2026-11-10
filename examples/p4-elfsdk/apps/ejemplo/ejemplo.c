/**
 * @file ejemplo.c
 *
 * El probador de la libreria de gestos: lo mismo que antes hacia a mano, pero
 * ahora encima de apps/lib/gestos.[ch].  Si quieres saber que da el touch de
 * esta placa, esto es lo que se lanza.
 *
 *   elf /elf/ejemplo.elf
 *
 * El app deja de ser UI y pasa a ser logica: abre la pantalla con
 * ui_create(), pone dos botones y se pasa la vida sacando eventos del anillo
 * con ui_poll().  Todo lo que se ve (fondo, flecha que gira en vivo,
 * pastilla de la direccion y nota) lo pinta la libreria.
 *
 * Las dos reglas del formato que manda gestos.h siguen aplicando: aqui no hay
 * estaticas escribibles de fichero y exit() se llama desde ESTE bucle, no desde
 * un callback, porque exit() es kill(gettid()) y mataria la hebra equivocada.
 * Antes de salir se llama a ui_destroy(): kill() devuelve la PSRAM de la
 * imagen del app, pero el handle de la libreria es un psram_malloc aparte que
 * solo se suelta aqui.
 */
#include <stdio.h>
#include <kernel.h>
#include <timer.h>
#include "gestos.h"

void exit(int codigo);

#define BOTON_SUMAR 1
#define BOTON_SALIR 2

int main(int argc, char *argv[])
{
    ui_input_t *g;
    ui_event_t ev;
    char texto[64];
    int cuenta = 0;

    (void)argc;
    (void)argv;

    printf("ejemplo: probador de gestos y botones (tid=%d, millis=%lu)\r\n",
           gettid(), (unsigned long)millis());

    g = ui_create();
    if (NULL == g)
    {
        exit(1);
    }

    /* Geometria a pelo sobre los 320x480 de la placa. */
    ui_button(g, BOTON_SUMAR, "SUMAR", 20, 400, 130, 55, 0x2ECC71);
    ui_button(g, BOTON_SALIR, "SALIR", 170, 400, 130, 55, 0xE8453C);
    ui_set_note(g, "desliza, toca, manten pulsado");

    printf("ejemplo: 4 direcciones de arrastre, toque, larga y dos botones\r\n");

    for (;;)
    {
        if (!ui_poll(g, &ev))
        {
            sleep(50);
            continue;
        }

        switch (ev.tipo)
        {
        case UI_EVENT_SWIPE:
            printf("ejemplo: arrastre %s  (%d,%d)->(%d,%d) dx=%d dy=%d\r\n",
                   ui_dir_name(ev.dir), ev.x1, ev.y1, ev.x2, ev.y2,
                   ev.dx, ev.dy);
            snprintf(texto, sizeof(texto), "(%d,%d)->(%d,%d)  dx=%d dy=%d",
                     ev.x1, ev.y1, ev.x2, ev.y2, ev.dx, ev.dy);
            ui_set_note(g, texto);
            break;

        case UI_EVENT_TAP:
            printf("ejemplo: toque en (%d,%d)\r\n", ev.x1, ev.y1);
            break;

        case UI_EVENT_LONG_PRESS:
            printf("ejemplo: pulsacion larga en (%d,%d)\r\n", ev.x1, ev.y1);
            break;

        case UI_EVENT_BUTTON:
            if (BOTON_SALIR == ev.id)
            {
                printf("ejemplo: SALIR con cuenta=%d\r\n", cuenta);
                ui_destroy(g);
                exit(0);
            }
            cuenta++;
            printf("ejemplo: SUMAR -> cuenta=%d\r\n", cuenta);
            snprintf(texto, sizeof(texto), "cuenta: %d", cuenta);
            ui_set_title(g, texto);
            break;

        default:
            break;
        }
    }
}
