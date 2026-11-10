/**
 * Plantilla de app ELF cooperativa (LVGL + appmgr + ui_*).
 * Renombra myapp -> tunombre (fichero, APP en Makefile, app.txt, ruta).
 * Reglas ELF: sin estáticas escribibles (estado en PSRAM); no exit() en callbacks.
 */
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <thread.h>
#include <memory.h>
#include <timer.h>
#include <lvgl.h>
#include <appmgr.h>
#include "gestos.h"

void exit(int codigo);

#define APP_NAME "MyApp"
#define APP_PATH "/apps/myapp/app.elf"
#define APP_COLOR 0x4f7d3a

struct state { ui_input_t *ui; int slot; };

static void ir_fondo(struct state *s)
{
    ui_event_t ev; int st;
    if (s->slot < 0) return;
    ui_show(s->ui, 0);
    appmgr_set_state(s->slot, APP_BACKGROUND);
    for (;;) {
        st = appmgr_get_state(s->slot);
        if (APP_RUNNING == st) { ui_show(s->ui, 1); return; }
        if (APP_CLOSING == st || APP_FREE == st) { appmgr_set_state(s->slot, APP_FREE); ui_destroy(s->ui); psram_free(s); exit(0); }
        while (ui_poll(s->ui, &ev)) {}
        sleep(50);
    }
}

int main(int argc, char *argv[])
{
    struct state *s; ui_event_t ev; (void)argc; (void)argv;
    s = psram_malloc(sizeof(*s)); if (!s) exit(1); memset(s,0,sizeof(*s)); s->slot = -1;
    s->ui = ui_create_ex(UI_CHROME_NONE); if (!s->ui) { psram_free(s); exit(1); }
    s->slot = appmgr_register(APP_NAME, APP_PATH, APP_COLOR, gettid());

    ui_button(s->ui, 1, "Hola", 60, 200, 200, 60, APP_COLOR);

    for (;;) {
        if (s->slot >= 0 && appmgr_get_state(s->slot) == APP_CLOSING) {
            appmgr_unregister(s->slot); ui_destroy(s->ui); psram_free(s); exit(0);
        }
        while (ui_poll(s->ui, &ev)) {
            if (ev.tipo == UI_EVENT_SWIPE && ev.dir == UI_DIR_UP && ev.edge == UI_EDGE_BOTTOM) ir_fondo(s);
            if (ev.tipo == UI_EVENT_BUTTON && ev.id == 1) printf("hola\n");
        }
        sleep(40);
    }
    return 0;
}
