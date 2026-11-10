/**
 * @file launcher.c
 *
 * Lanzador y gestor de aplicaciones.  Descubre las apps instaladas escaneando
 * /apps/<carpeta>/app.txt en la SD (cada app en su carpeta con app.elf, icon.jpg
 * y app.txt).  La pantalla de inicio muestra solo el icono (dibujo alusivo) de
 * cada app; al tocarlo crea una tarea que ejecuta el ELF (create + elf_run).
 *
 * Con un gesto de abajo hacia arriba en una app, la app se oculta y pasa a
 * segundo plano.  Con un gesto de abajo hacia arriba en el launcher se abre el
 * conmutador de apps activas (tap retoma, X cierra cooperativamente).
 *
 *   elf /apps/launcher/app.elf
 *
 * Estado en PSRAM, sin estaticas escribibles.  LVGL lo bombea 'lvglui'.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <thread.h>
#include <device.h>
#include <timer.h>
#include <memory.h>
#include <lvgl.h>
#include <elfload.h>
#include <appmgr.h>
#include <fat_filelib.h>
#include "gestos.h"
#include "estado.h"

void exit(int codigo);

/* Una app descubierta en /apps/. */
#define MAX_APPS    12
#define APPS_PER_PAGE 6
#define APP_NAMELEN 32
#define APP_PATHLEN 80
#define APP_ICONLEN 96

#define ICON_W 96
#define ICON_H 96
#define ICON565_SIZE (ICON_W * ICON_H * 2)

struct app_info
{
    char   name[APP_NAMELEN];
    char   dir[APP_NAMELEN];
    char   exec[APP_PATHLEN];
    char   icon[APP_ICONLEN]; /* ruta LVGL fallback, ej. S:/apps/x/icon.jpg */
    uint32 color;
    int    used;
    uint8_t *px;               /* RGB565 cache en PSRAM (ICON565_SIZE) */
    lv_img_dsc_t dsc;          /* descriptor LVGL apuntando a px */
};

#define P_HOME    0
#define P_SWITCH  1

#define ID_APP      100   /* 100 + indice */
#define ID_RESUME   200   /* 200 + slot */
#define ID_CLOSE    300   /* 300 + slot */
#define ID_BACK     400

#define VIGIA_STK   8192
#define VIGIA_MS    200
#define SCREEN_FILE "/screen.txt"
#define SCREEN_DEFAULT_MIN 2
#define RUN_STK     16384

#define C_FONDO  0x1B1408
#define C_TEXTO  0xE8D8B0
#define C_HINT   0x6E5A3A
#define C_CARD   0x2A1E10

struct launcher
{
    ui_input_t *g;
    statusbar_t *e;
    lv_obj_t *chip;
    lv_obj_t *hint;
    struct app_info apps[MAX_APPS];
    int napp;
    int page;           /* pagina horizontal del home */
    int pagina;         /* P_HOME / P_SWITCH */
    int oculto;         /* UI oculta (hay app en frente) */
    int cerrar;
    int screen_ms;
    tid_typ vig_tid;
};

/* --- utilidades de texto --- */

static void recorta(char *s)
{
    char *p = s;
    char *f;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    f = s + strlen(s);
    while (f > s && (f[-1] == ' ' || f[-1] == '\t' || f[-1] == '\r' || f[-1] == '\n'))
    {
        f--;
    }
    *f = '\0';
}

static uint32 parse_color(const char *s)
{
    uint32 v = 0;
    char c;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (*s)
    {
        c = *s++;
        if (c >= '0' && c <= '9') v = (v << 4) | (uint32)(c - '0');
        else if (c >= 'a' && c <= 'f') v = (v << 4) | (uint32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = (v << 4) | (uint32)(c - 'A' + 10);
        else break;
    }
    return v;
}

/* Lee /apps/<dir>/app.txt y rellena info. Devuelve 1 si es valida. */
static int leer_app(const char *dir, struct app_info *ai)
{
    char path[96];
    char buf[384];
    void *f;
    int n, i, lin;
    char *p;

    memset(ai, 0, sizeof(*ai));
    ai->color = C_CARD;
    snprintf(path, sizeof(path), "/apps/%s/app.txt", dir);
    f = fl_fopen(path, "r");
    if (NULL == f) return 0;
    n = fl_fread(buf, 1, (int)sizeof(buf) - 1, f);
    fl_fclose(f);
    if (n <= 0) return 0;
    buf[n] = '\0';

    /* Parseo linea a linea: clave=valor */
    p = buf;
    for (lin = 0; lin < 32 && *p; lin++)
    {
        char *eol = p;
        char *eq;
        while (*eol && *eol != '\n' && *eol != '\r') eol++;
        if (*eol) { *eol = '\0'; eol++; }
        while (*eol == '\r' || *eol == '\n') eol++;
        recorta(p);
        if (*p == '\0' || *p == '#') { p = eol; continue; }
        eq = p;
        while (*eq && *eq != '=') eq++;
        if (*eq == '=')
        {
            *eq = '\0';
            recorta(p);
            recorta(eq + 1);
            if (0 == strcmp(p, "name"))
                strncpy(ai->name, eq + 1, APP_NAMELEN - 1);
            else if (0 == strcmp(p, "exec"))
                strncpy(ai->exec, eq + 1, APP_PATHLEN - 1);
            else if (0 == strcmp(p, "icon"))
                strncpy(ai->icon, eq + 1, APP_ICONLEN - 1);
            else if (0 == strcmp(p, "color"))
                ai->color = parse_color(eq + 1);
        }
        p = eol;
    }

    strncpy(ai->dir, dir, APP_NAMELEN - 1);
    if (ai->name[0] == '\0') strncpy(ai->name, dir, APP_NAMELEN - 1);
    if (ai->exec[0] == '\0') snprintf(ai->exec, sizeof(ai->exec), "/apps/%s/app.elf", dir);
    ai->used = 1;
    (void)i;
    return 1;
}

static int load_icon565(struct app_info *ai)
{
    char path[96];
    void *f;
    int n;

    if (NULL == ai || ai->dir[0] == '\0') return 0;
    if (ai->dsc.data != NULL) return 1;
    if (ai->px == (uint8_t *)-1) return 0; /* ya intento y fallo */

    snprintf(path, sizeof(path), "/apps/%s/icon565.bin", ai->dir);
    f = fl_fopen(path, "r");
    if (NULL == f) { ai->px = (uint8_t *)-1; return 0; }
    ai->px = (uint8_t *)psram_malloc(ICON565_SIZE);
    if (NULL == ai->px) { fl_fclose(f); ai->px = (uint8_t *)-1; return 0; }
    n = fl_fread(ai->px, 1, ICON565_SIZE, f);
    fl_fclose(f);
    if (n != ICON565_SIZE) { psram_free(ai->px); ai->px = (uint8_t *)-1; return 0; }

    memset(&ai->dsc, 0, sizeof(ai->dsc));
    ai->dsc.header.always_zero = 0;
    ai->dsc.header.w = ICON_W;
    ai->dsc.header.h = ICON_H;
    ai->dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    ai->dsc.data_size = ICON565_SIZE;
    ai->dsc.data = ai->px;
    return 1;
}

static void free_app_icons(struct launcher *l)
{
    int i;
    for (i = 0; i < MAX_APPS; i++)
        if (l->apps[i].px != NULL && l->apps[i].px != (uint8_t *)-1)
            psram_free(l->apps[i].px);
}

/* Escanea /apps/ y carga el catalogo. */
static void scan_apps(struct launcher *l)
{
    FL_DIR d;
    fl_dirent e;

    free_app_icons(l);
    l->napp = 0;
    memset(l->apps, 0, sizeof(l->apps));
    if (NULL == fl_opendir("/apps", &d))
    {
        return;
    }
    while (0 == fl_readdir(&d, &e))
    {
        if (e.filename[0] == '.') continue;
        if (!e.is_dir) continue;
        /* El launcher vive en /apps/ pero no es una app gestionada. */
        if (0 == strcmp(e.filename, "launcher")) continue;
        if (l->napp >= MAX_APPS) break;
        if (leer_app(e.filename, &l->apps[l->napp]))
        {
            l->napp++;
        }
    }
    fl_closedir(&d);
}

/* Hebra que ejecuta un ELF. */
static int app_runner(char *path)
{
    int r = elf_run(path, 0, NULL);
    if (SYSERR == r)
    {
        int slot = appmgr_find_path(path);
        if (slot >= 0)
        {
            appmgr_unregister(slot);
        }
    }
    return 0;
}

/* Chip label removed by design; keep no-op to preserve call sites. */
static void chip_set(struct launcher *l, const char *txt)
{
    (void)l; (void)txt;
}

static void hint_set(struct launcher *l, const char *txt)
{
    if (NULL == l->hint) return;
    lvgl_lock();
    lv_label_set_text(l->hint, txt);
    lvgl_unlock();
}

static int hay_app_running(void)
{
    int i, n = appmgr_count();
    struct app_entry *t = appmgr_table();
    for (i = 0; i < n; i++)
    {
        if (APP_RUNNING == t[i].state) return 1;
    }
    return 0;
}

static void ocultar_ui(struct launcher *l)
{
    if (l->oculto) return;
    ui_show(l->g, 0);
    l->oculto = 1;
}

static void mostrar_ui(struct launcher *l)
{
    if (!l->oculto) return;
    ui_show(l->g, 1);
    l->oculto = 0;
}

static void lanzar_app(struct launcher *l, int idx)
{
    int slot;
    tid_typ tid;
    struct app_info *ai;

    if (idx < 0 || idx >= l->napp) return;
    ai = &l->apps[idx];

    slot = appmgr_find_path(ai->exec);
    if (slot >= 0)
    {
        int st = appmgr_get_state(slot);
        if (APP_CLOSING == st) { chip_set(l, "closing..."); return; }
        if (APP_BACKGROUND == st || APP_RUNNING == st)
        {
            appmgr_take_foreground(slot);
            ocultar_ui(l);
            chip_set(l, ai->name);
            return;
        }
        appmgr_unregister(slot);
    }

    chip_set(l, ai->name);
    tid = create((void *)app_runner, RUN_STK, getprio(gettid()),
                 "apprun", 1, (long)ai->exec);
    if (SYSERR != tid)
    {
        appmgr_register(ai->name, ai->exec, ai->color, tid);
        appmgr_take_foreground(appmgr_find_path(ai->exec));
        ocultar_ui(l);
        ready(tid, RESCHED_NO);
    }
    else
    {
        mostrar_ui(l);
        chip_set(l, "no memory/task");
    }
}

/* Boton de icono: tarjeta ui_button sin texto + lv_img centrada.
 * Prefiere icon565.bin precacheado en PSRAM (carga rapida) y cae a icon (SJPG). */
static void boton_icono(struct launcher *l, int id, struct app_info *ai,
                        int x, int y, int w, int h)
{
    lv_obj_t *btn = ui_button(l->g, id, "", x, y, w, h, ai->color);
    if (NULL == btn) return;
    load_icon565(ai); /* FAT I/O fuera de lvgl_lock */
    lvgl_lock();
    /* Quitar el rotulo de texto (unico hijo) para mostrar solo el dibujo. */
    if (lv_obj_get_child_cnt(btn) > 0)
    {
        lv_obj_del(lv_obj_get_child(btn, 0));
    }
    if (ai->dsc.data != NULL || ai->icon[0] != '\0')
    {
        lv_obj_t *img = lv_img_create(btn);
        if (ai->dsc.data != NULL) lv_img_set_src(img, &ai->dsc);
        else                       lv_img_set_src(img, ai->icon);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(img);
    }
    lvgl_unlock();
}

static void home_draw(struct launcher *l)
{
    int i, col, row, x, y, base, pages;
    char h[24];

    ui_buttons_clear(l->g);
    pages = (l->napp + APPS_PER_PAGE - 1) / APPS_PER_PAGE;
    if (pages < 1) pages = 1;
    if (l->page >= pages) l->page = pages - 1;
    if (l->page < 0) l->page = 0;
    base = l->page * APPS_PER_PAGE;
    for (i = 0; i < APPS_PER_PAGE && (base + i) < l->napp; i++)
    {
        col = i % 2; row = i / 2;
        x = 11 + col * 153; y = 106 + row * 108;
        boton_icono(l, ID_APP + base + i, &l->apps[base + i], x, y, 145, 100);
    }
    if (0 == l->napp) hint_set(l, "no apps in /apps");
    else if (pages > 1) { snprintf(h, sizeof(h), "< %d/%d >", l->page + 1, pages); hint_set(l, h); }
    else hint_set(l, "");
}

static void pantalla_home(struct launcher *l)
{
    l->pagina = P_HOME;
    l->page = 0;
    scan_apps(l);
    chip_set(l, "Launcher");
    home_draw(l);
}

static void pantalla_switch(struct launcher *l)
{
    int i, n, y, activos;
    struct app_entry *t;
    char label[48];

    l->pagina = P_SWITCH;
    ui_buttons_clear(l->g);
    chip_set(l, "Apps");
    hint_set(l, "");

    t = appmgr_table();
    n = appmgr_count();
    y = 106;
    activos = 0;
    for (i = 0; i < n; i++)
    {
        if (APP_FREE == t[i].state) continue;
        activos++;
        if (APP_BACKGROUND == t[i].state)
            snprintf(label, sizeof(label), "%s (bg)", t[i].name);
        else if (APP_CLOSING == t[i].state)
            snprintf(label, sizeof(label), "%s (closing)", t[i].name);
        else
            snprintf(label, sizeof(label), "%s", t[i].name);
        ui_button(l->g, ID_RESUME + i, label, 11, y, 245, 52,
                     t[i].color ? t[i].color : C_CARD);
        ui_button(l->g, ID_CLOSE + i, "X", 264, y, 45, 52, 0xC0392B);
        y += 60;
        if (y > 400) break;
    }
    if (0 == activos) chip_set(l, "no active apps");
}

static void mostrar_switch(struct launcher *l)
{
    int i, n = appmgr_count();
    struct app_entry *t = appmgr_table();
    for (i = 0; i < n; i++)
    {
        if (APP_FREE != t[i].state) { pantalla_switch(l); return; }
    }
    chip_set(l, "no active apps");
}

static void procesar(struct launcher *l, ui_event_t *ev)
{
    if (UI_EVENT_SWIPE == ev->tipo)
    {
        if (P_HOME == l->pagina)
        {
            if (UI_DIR_UP == ev->dir) mostrar_switch(l);
            else if (UI_DIR_LEFT == ev->dir) { l->page++; home_draw(l); }
            else if (UI_DIR_RIGHT == ev->dir) { l->page--; home_draw(l); }
        }
        else
        {
            /* Switcher: swipe up returns to home. */
            if (UI_DIR_UP == ev->dir) pantalla_home(l);
        }
        return;
    }
    if (UI_EVENT_BUTTON != ev->tipo) return;

    if (ev->id >= ID_APP && ev->id < ID_APP + l->napp)
    {
        lanzar_app(l, ev->id - ID_APP);
        return;
    }
    if (ev->id >= ID_RESUME && ev->id < ID_RESUME + APPMGR_MAX)
    {
        int slot = ev->id - ID_RESUME;
        if (appmgr_get_state(slot) != APP_FREE &&
            appmgr_get_state(slot) != APP_CLOSING)
        {
            appmgr_take_foreground(slot);
            ocultar_ui(l);
        }
        return;
    }
    if (ev->id >= ID_CLOSE && ev->id < ID_CLOSE + APPMGR_MAX)
    {
        int slot = ev->id - ID_CLOSE;
        if (appmgr_get_state(slot) != APP_FREE)
        {
            appmgr_request_close(slot);
            pantalla_switch(l);
        }
        return;
    }
}

static int read_screen_ms(void)
{
    void *f;
    char buf[16];
    int n, mins = SCREEN_DEFAULT_MIN;
    int i = 0, v = 0;

    f = fl_fopen(SCREEN_FILE, "r");
    if (NULL != f)
    {
        n = fl_fread(buf, 1, (int)sizeof(buf) - 1, f);
        fl_fclose(f);
        if (n > 0)
        {
            buf[n] = '\0';
            while (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n') i++;
            while (buf[i] >= '0' && buf[i] <= '9') { v = v * 10 + (buf[i] - '0'); i++; }
            if (v > 0) mins = v;
        }
    }
    if (mins < 1) mins = 1;
    if (mins > 120) mins = 120;
    return mins * 60 * 1000;
}

static void vigia(ulong arg)
{
    struct launcher *l = (struct launcher *)arg;
    int d;

    d = (int)getdev("LVGL0");
    if (SYSERR != d) control(d, LVGL_CTRL_IDLE, l->screen_ms, 0);
    while (!l->cerrar)
    {
        if (!l->oculto && l->e != NULL) statusbar_refresh(l->e);
        sleep(VIGIA_MS);
    }
    if (SYSERR != d) control(d, LVGL_CTRL_IDLE, 0, 0);
    send(l->vig_tid, OK);
}

int main(int argc, char *argv[])
{
    struct launcher *l;
    ui_event_t ev;

    (void)argc; (void)argv;
    printf("launcher: /apps manager (tid=%d)\r\n", gettid());

    l = (struct launcher *)psram_malloc(sizeof(struct launcher));
    if (NULL == l) { exit(1); }
    memset(l, 0, sizeof(struct launcher));
    l->vig_tid = SYSERR;

    l->g = ui_create_ex(UI_CHROME_NONE);
    if (NULL == l->g) { psram_free(l); exit(1); }

    lvgl_lock();
    lv_obj_set_style_bg_color(ui_root(l->g), lv_color_hex(C_FONDO), 0);
    lv_obj_set_style_bg_grad_dir(ui_root(l->g), LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(ui_root(l->g), LV_OPA_COVER, 0);
    l->chip = NULL;
    l->hint = lv_label_create(ui_root(l->g));
    lv_label_set_text(l->hint, "");
    lv_obj_set_style_text_color(l->hint, lv_color_hex(C_HINT), 0);
    lv_obj_set_pos(l->hint, 8, 450);
    lvgl_unlock();

    l->e = statusbar_create(ui_root(l->g), 8, 8, 304);
    l->screen_ms = read_screen_ms();
    pantalla_home(l);

    l->vig_tid = create((void *)vigia, VIGIA_STK, getprio(gettid()),
                        "launch-vig", 1, (long)l);
    if (SYSERR != l->vig_tid) ready(l->vig_tid, RESCHED_NO);

    for (;;)
    {
        if (!l->oculto && hay_app_running()) ocultar_ui(l);
        if (l->oculto && !hay_app_running())
        {
            mostrar_ui(l);
            if (P_SWITCH == l->pagina) pantalla_switch(l);
        }
        while (ui_poll(l->g, &ev))
        {
            if (!l->oculto) procesar(l, &ev);
        }
        sleep(40);
    }
    return 0;
}
