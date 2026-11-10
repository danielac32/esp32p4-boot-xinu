/**
 * Galeria de fotos (vintage).
 * Fuente unica de imagenes: /img (JPG en la SD).
 * Miniaturas/full rapidos: usa /img565/thumb/<base>.565 y /img565/<base>.565
 * si existen (preconvertidos en sd/img565/ del proyecto).
 *
 * Gestos:
 *   Rejilla: toque = abrir, swipe izq/der = pagina.
 *   Grande:  swipe izq/der = anterior/siguiente, swipe arriba o abajo = rejilla.
 *   Ambas:   swipe desde BORDE inferior hacia arriba = fondo (launcher).
 */
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <thread.h>
#include <memory.h>
#include <timer.h>
#include <lvgl.h>
#include <fat_filelib.h>
#include <appmgr.h>
#include "gestos.h"

void exit(int codigo);

#define APP_NAME  "Photos"
#define APP_PATH  "/apps/photos/app.elf"
#define APP_COLOR 0xa67c52

#define PHOTO_SRC "/img"

#define MAXF      96
#define FN        72

#define TH_W      96
#define TH_H      72
#define TH_SZ     (TH_W * TH_H * 2)

#define FU_W      320
#define FU_H      240
#define FU_SZ     (FU_W * FU_H * 2)

#define GRID_COLS 3
#define GRID_ROWS 3
#define PER_PAGE  (GRID_COLS * GRID_ROWS)

#define CELL_W    100
#define CELL_H    112
#define GRID_X    5
#define GRID_Y    74

#define MODE_GRID 0
#define MODE_FULL 1

#define ID_CELL   100

/* Paleta vintage */
#define C_BG      0x1c1510
#define C_CARD    0xf3e7cf
#define C_FRAME   0x5a3e2b
#define C_ACCENT  0xa67c52
#define C_GOLD    0xd4a017
#define C_TEXT    0x2e2218
#define C_CREAM   0xf7f0e1
#define C_DIM     0x8c7a66
#define C_PH_BG   0xd8ccb8

struct state
{
    ui_input_t *ui;
    int slot;
    int mode;
    int n;
    int cur;
    int page;
    char files[MAXF][FN];
    uint8_t *thumb_px;
    uint8_t *full_px;
    uint8_t thumb_ok[PER_PAGE];
    lv_img_dsc_t thumb_dsc[PER_PAGE];
    lv_img_dsc_t full_dsc;
    lv_obj_t *thumb_img[PER_PAGE];
    lv_obj_t *full_img;
    lv_obj_t *msg;
};

static int str_ieq(const char *a, const char *b)
{
    while (*a && *b)
    {
        char ca = *a;
        char cb = *b;

        if (ca >= 'A' && ca <= 'Z')
        {
            ca = (char)(ca + 32);
        }
        if (cb >= 'A' && cb <= 'Z')
        {
            cb = (char)(cb + 32);
        }
        if (ca != cb)
        {
            return 0;
        }
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

static int is_img(const char *s)
{
    int n = (int)strlen(s);
    int i = n - 1;

    while (i >= 0 && s[i] != '.')
    {
        i--;
    }
    if (i < 0)
    {
        return 0;
    }
    i++;
    return str_ieq(s + i, "565")
        || str_ieq(s + i, "jpg")
        || str_ieq(s + i, "jpeg")
        || str_ieq(s + i, "bmp")
        || str_ieq(s + i, "png")
        || str_ieq(s + i, "bin");
}

static int is565(const char *s)
{
    int n = (int)strlen(s);

    if (n < 5)
    {
        return 0;
    }
    return (s[n - 4] == '.' && s[n - 3] == '5' && s[n - 2] == '6' && s[n - 1] == '5');
}

static void base_noext(char *out, int cap, const char *path)
{
    int n = (int)strlen(path);
    int i = n - 1;
    int slash = -1;
    int dot = n;

    while (i >= 0)
    {
        if (path[i] == '/' && slash < 0)
        {
            slash = i;
        }
        if (path[i] == '.' && dot == n)
        {
            dot = i;
        }
        if (slash >= 0 && dot != n)
        {
            break;
        }
        i--;
    }
    if (slash < dot)
    {
        int len = dot - (slash + 1);

        if (len >= cap)
        {
            len = cap - 1;
        }
        memcpy(out, path + slash + 1, (size_t)len);
        out[len] = '\0';
    }
    else
    {
        strncpy(out, path, (size_t)(cap - 1));
        out[cap - 1] = '\0';
    }
}

static int file_exists(const char *path)
{
    void *f = fl_fopen(path, "r");

    if (f != NULL)
    {
        fl_fclose(f);
        return 1;
    }
    return 0;
}

static int read_all(const char *path, uint8_t *dst, int size)
{
    void *f = fl_fopen(path, "r");
    int got = 0;

    if (f == NULL)
    {
        return 0;
    }
    while (got < size)
    {
        int n = fl_fread(dst + got, 1, 4096, f);

        if (n <= 0)
        {
            break;
        }
        got += n;
    }
    fl_fclose(f);
    if (got < size)
    {
        memset(dst + got, 0, (size_t)(size - got));
    }
    return got;
}

/* Resuelve fuente .565: para /img/<base>.jpg busca /img565/(thumb/)<base>.565 */
static void resolve_src(char *out, int cap, const char *path, int want_thumb)
{
    char base[48];
    char cand[96];

    base_noext(base, sizeof(base), path);
    if (want_thumb)
    {
        snprintf(cand, sizeof(cand), "/img565/thumb/%s.565", base);
        if (file_exists(cand))
        {
            strncpy(out, cand, (size_t)(cap - 1));
            out[cap - 1] = '\0';
            return;
        }
        snprintf(cand, sizeof(cand), "/sd/img565/thumb/%s.565", base);
        if (file_exists(cand))
        {
            strncpy(out, cand, (size_t)(cap - 1));
            out[cap - 1] = '\0';
            return;
        }
    }
    else
    {
        snprintf(cand, sizeof(cand), "/img565/%s.565", base);
        if (file_exists(cand))
        {
            strncpy(out, cand, (size_t)(cap - 1));
            out[cap - 1] = '\0';
            return;
        }
        snprintf(cand, sizeof(cand), "/sd/img565/%s.565", base);
        if (file_exists(cand))
        {
            strncpy(out, cand, (size_t)(cap - 1));
            out[cap - 1] = '\0';
            return;
        }
    }
    /* Hermano .565 junto al archivo original */
    {
        int n = (int)strlen(path);

        if (n + 2 < cap)
        {
            char tmp[80];
            int i;

            strncpy(tmp, path, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';
            for (i = (int)strlen(tmp) - 1; i > 0 && tmp[i] != '/'; i--)
            {
                if (tmp[i] == '.')
                {
                    tmp[i] = '\0';
                    break;
                }
            }
            snprintf(cand, sizeof(cand), "%s.565", tmp);
            if (file_exists(cand))
            {
                strncpy(out, cand, (size_t)(cap - 1));
                out[cap - 1] = '\0';
                return;
            }
        }
    }
    /* Sin .565: usar el original (jpg/png) como ultimo recurso */
    strncpy(out, path, (size_t)(cap - 1));
    out[cap - 1] = '\0';
}

static void scan_dir(struct state *s, const char *dir)
{
    FL_DIR d;
    fl_dirent e;

    if (NULL == fl_opendir(dir, &d))
    {
        return;
    }
    while (0 == fl_readdir(&d, &e) && s->n < MAXF)
    {
        if (e.is_dir)
        {
            continue;
        }
        if (e.filename[0] == '.')
        {
            continue;
        }
        if (is_img(e.filename))
        {
            snprintf(s->files[s->n], FN, "%s/%s", dir, e.filename);
            s->n++;
        }
    }
    fl_closedir(&d);
}

static void scan(struct state *s)
{
    s->n = 0;
    /* Fuente unica: /img en la SD */
    scan_dir(s, PHOTO_SRC);
}

static void ir_fondo(struct state *s);
static void show_grid(struct state *s);
static void show_full(struct state *s);

static void load_thumb(struct state *s, int slot, const char *path)
{
    char src[96];
    uint8_t *buf = s->thumb_px + (slot * TH_SZ);

    s->thumb_ok[slot] = 0;
    memset(buf, 0, (size_t)TH_SZ);
    resolve_src(src, sizeof(src), path, 1);
    if (is565(src))
    {
        int got = read_all(src, buf, TH_SZ);

        if (got > 0)
        {
            s->thumb_ok[slot] = 1;
            s->thumb_dsc[slot].header.always_zero = 0;
            s->thumb_dsc[slot].header.w = TH_W;
            s->thumb_dsc[slot].header.h = TH_H;
            s->thumb_dsc[slot].header.cf = LV_IMG_CF_TRUE_COLOR;
            s->thumb_dsc[slot].data_size = TH_SZ;
            s->thumb_dsc[slot].data = buf;
        }
    }
}

static void show_grid(struct state *s)
{
    int i;
    int base;
    int pages;
    char info[64];

    s->mode = MODE_GRID;
    ui_buttons_clear(s->ui);
    for (i = 0; i < PER_PAGE; i++)
    {
        s->thumb_img[i] = NULL;
        s->thumb_ok[i] = 0;
    }
    if (s->full_img != NULL)
    {
        lvgl_lock();
        lv_obj_add_flag(s->full_img, LV_OBJ_FLAG_HIDDEN);
        lvgl_unlock();
    }

    pages = (s->n + PER_PAGE - 1) / PER_PAGE;
    if (pages < 1)
    {
        pages = 1;
    }
    if (s->page >= pages)
    {
        s->page = pages - 1;
    }
    if (s->page < 0)
    {
        s->page = 0;
    }
    base = s->page * PER_PAGE;

    for (i = 0; i < PER_PAGE; i++)
    {
        int idx = base + i;
        int col = i % GRID_COLS;
        int row = i / GRID_COLS;
        int x = GRID_X + (col * (CELL_W + 4));
        int y = GRID_Y + (row * (CELL_H + 6));
        char bname[32];
        lv_obj_t *btn;

        if (idx >= s->n)
        {
            break;
        }
        base_noext(bname, sizeof(bname), s->files[idx]);
        if ((int)strlen(bname) > 8)
        {
            bname[8] = '\0';
        }
        /* Tarjeta estilo polaroid vintage */
        btn = ui_button(s->ui, ID_CELL + i, bname, x, y, CELL_W, CELL_H, C_CARD);
        if (btn != NULL)
        {
            load_thumb(s, i, s->files[idx]);
            lvgl_lock();
            lv_obj_set_style_border_width(btn, 3, 0);
            lv_obj_set_style_border_color(btn, lv_color_hex(C_FRAME), 0);
            lv_obj_set_style_radius(btn, 4, 0);
            lv_obj_set_style_shadow_width(btn, 0, 0);
            lv_obj_set_style_pad_all(btn, 0, 0);
            /* Area de foto (marco interior) */
            {
                lv_obj_t *mat = lv_obj_create(btn);

                lv_obj_set_size(mat, 92, 68);
                lv_obj_align(mat, LV_ALIGN_TOP_MID, 0, 5);
                lv_obj_set_style_bg_color(mat, lv_color_hex(C_PH_BG), 0);
                lv_obj_set_style_bg_opa(mat, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(mat, 1, 0);
                lv_obj_set_style_border_color(mat, lv_color_hex(C_ACCENT), 0);
                lv_obj_set_style_radius(mat, 2, 0);
                lv_obj_set_style_pad_all(mat, 0, 0);
                lv_obj_clear_flag(mat, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_clear_flag(mat, LV_OBJ_FLAG_CLICKABLE);
                if (s->thumb_ok[i])
                {
                    s->thumb_img[i] = lv_img_create(mat);
                    lv_img_set_src(s->thumb_img[i], &s->thumb_dsc[i]);
                    lv_obj_align(s->thumb_img[i], LV_ALIGN_CENTER, 0, 0);
                    lv_obj_clear_flag(s->thumb_img[i], LV_OBJ_FLAG_CLICKABLE);
                }
            }
            /* Nombre en la base de la tarjeta */
            {
                lv_obj_t *lb = lv_obj_get_child(btn, 0);

                if (lb != NULL)
                {
                    lv_obj_set_style_text_color(lb, lv_color_hex(C_TEXT), 0);
                    lv_obj_align(lb, LV_ALIGN_BOTTOM_MID, 0, -6);
                }
            }
            lvgl_unlock();
        }
    }

    {
        int okc = 0;

        for (i = 0; i < PER_PAGE && (base + i) < s->n; i++)
        {
            if (s->thumb_ok[i])
            {
                okc++;
            }
        }
        if (s->n <= 0)
        {
            snprintf(info, sizeof(info), "Sin imagenes en %s", PHOTO_SRC);
        }
        else
        {
            snprintf(info, sizeof(info), "%d fotos  pag %d/%d  prev:%d",
                     s->n, s->page + 1, pages, okc);
        }
    }
    lvgl_lock();
    lv_label_set_text(s->msg, info);
    lvgl_unlock();
}

static void show_full(struct state *s)
{
    char src[96];
    char info[72];
    char bname[40];

    s->mode = MODE_FULL;
    ui_buttons_clear(s->ui);
    for (int i = 0; i < PER_PAGE; i++)
    {
        s->thumb_img[i] = NULL;
    }

    if (s->cur < 0)
    {
        s->cur = 0;
    }
    if (s->cur >= s->n)
    {
        s->cur = s->n - 1;
    }
    if (s->n <= 0)
    {
        show_grid(s);
        return;
    }

    resolve_src(src, sizeof(src), s->files[s->cur], 0);
    base_noext(bname, sizeof(bname), s->files[s->cur]);
    snprintf(info, sizeof(info), "Foto %d/%d  %s  |  ^v rejilla  <> nav",
             s->cur + 1, s->n, bname);

    lvgl_lock();
    if (s->full_img == NULL)
    {
        s->full_img = lv_img_create(ui_root(s->ui));
        lv_obj_clear_flag(s->full_img, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(s->full_img, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_set_style_pad_all(s->full_img, 0, 0);
    lv_obj_set_style_border_width(s->full_img, 2, 0);
    lv_obj_set_style_border_color(s->full_img, lv_color_hex(C_FRAME), 0);
    lv_obj_align(s->full_img, LV_ALIGN_TOP_MID, 0, 130);
    lv_obj_clear_flag(s->full_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s->full_img);
    lv_label_set_text(s->msg, info);
    lvgl_unlock();

    if (is565(src))
    {
        read_all(src, s->full_px, FU_SZ);
        s->full_dsc.header.always_zero = 0;
        s->full_dsc.header.w = FU_W;
        s->full_dsc.header.h = FU_H;
        s->full_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        s->full_dsc.data_size = FU_SZ;
        s->full_dsc.data = s->full_px;
        lvgl_lock();
        lv_img_set_src(s->full_img, &s->full_dsc);
        lv_obj_align(s->full_img, LV_ALIGN_TOP_MID, 0, 130);
        lv_obj_invalidate(s->full_img);
        lvgl_unlock();
    }
    else
    {
        /* JPG/otro por LVGL FS: puede ser lento en este HW */
        char lvpath[96];

        snprintf(lvpath, sizeof(lvpath), "S:%s", s->files[s->cur]);
        lvgl_lock();
        lv_label_set_text(s->msg, "cargando (jpg lento)...");
        lvgl_unlock();
        lvgl_lock();
        lv_img_set_src(s->full_img, lvpath);
        lv_label_set_text(s->msg, info);
        lvgl_unlock();
    }
}

static void ir_fondo(struct state *s)
{
    ui_event_t ev;
    int st;

    if (s->slot < 0)
    {
        return;
    }
    ui_show(s->ui, 0);
    appmgr_set_state(s->slot, APP_BACKGROUND);
    for (;;)
    {
        st = appmgr_get_state(s->slot);
        if (st == APP_RUNNING)
        {
            ui_show(s->ui, 1);
            return;
        }
        if (st == APP_CLOSING || st == APP_FREE)
        {
            appmgr_unregister(s->slot);
            if (s->thumb_px != NULL)
            {
                psram_free(s->thumb_px);
            }
            if (s->full_px != NULL)
            {
                psram_free(s->full_px);
            }
            ui_destroy(s->ui);
            psram_free(s);
            exit(0);
        }
        while (ui_poll(s->ui, &ev))
        {
        }
        sleep(50);
    }
}

int main(int argc, char *argv[])
{
    struct state *s;
    ui_event_t ev;

    (void)argc;
    (void)argv;

    s = (struct state *)psram_malloc(sizeof(*s));
    if (s == NULL)
    {
        exit(1);
    }
    memset(s, 0, sizeof(*s));
    s->slot = -1;

    s->thumb_px = (uint8_t *)psram_malloc(TH_SZ * PER_PAGE);
    if (s->thumb_px == NULL)
    {
        psram_free(s);
        exit(1);
    }
    memset(s->thumb_px, 0, (size_t)(TH_SZ * PER_PAGE));

    s->full_px = (uint8_t *)psram_malloc(FU_SZ);
    if (s->full_px == NULL)
    {
        psram_free(s->thumb_px);
        psram_free(s);
        exit(1);
    }
    memset(s->full_px, 0, (size_t)FU_SZ);

    s->ui = ui_create_ex(UI_CHROME_NONE);
    if (s->ui == NULL)
    {
        psram_free(s->full_px);
        psram_free(s->thumb_px);
        psram_free(s);
        exit(1);
    }
    s->slot = appmgr_register(APP_NAME, APP_PATH, APP_COLOR, gettid());

    lvgl_lock();
    lv_obj_set_style_bg_color(ui_root(s->ui), lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(ui_root(s->ui), LV_OPA_COVER, 0);
    {
        lv_obj_t *bar = lv_obj_create(ui_root(s->ui));

        lv_obj_set_size(bar, 320, 30);
        lv_obj_set_pos(bar, 0, 36);
        lv_obj_set_style_bg_color(bar, lv_color_hex(C_FRAME), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    }
    {
        lv_obj_t *line = lv_obj_create(ui_root(s->ui));

        lv_obj_set_size(line, 320, 2);
        lv_obj_set_pos(line, 0, 66);
        lv_obj_set_style_bg_color(line, lv_color_hex(C_GOLD), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(line, 0, 0);
        lv_obj_set_style_radius(line, 0, 0);
        lv_obj_set_style_pad_all(line, 0, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    }
    s->msg = lv_label_create(ui_root(s->ui));
    lv_obj_set_style_text_color(s->msg, lv_color_hex(C_CREAM), 0);
    lv_obj_set_width(s->msg, 308);
    lv_label_set_long_mode(s->msg, LV_LABEL_LONG_DOT);
    lv_obj_align(s->msg, LV_ALIGN_TOP_LEFT, 8, 42);
    lvgl_unlock();

    scan(s);
    show_grid(s);

    for (;;)
    {
        if (s->slot >= 0 && appmgr_get_state(s->slot) == APP_CLOSING)
        {
            appmgr_unregister(s->slot);
            psram_free(s->full_px);
            psram_free(s->thumb_px);
            ui_destroy(s->ui);
            psram_free(s);
            exit(0);
        }

        while (ui_poll(s->ui, &ev))
        {
            if (ev.tipo == UI_EVENT_BUTTON)
            {
                if (s->mode == MODE_GRID && ev.id >= ID_CELL && ev.id < ID_CELL + PER_PAGE)
                {
                    int idx = (s->page * PER_PAGE) + (ev.id - ID_CELL);

                    if (idx < s->n)
                    {
                        s->cur = idx;
                        show_full(s);
                    }
                }
            }
            else if (ev.tipo == UI_EVENT_SWIPE)
            {
                if (s->mode == MODE_FULL)
                {
                    /* En vista grande: arriba/abajo = volver a rejilla */
                    if (ev.dir == UI_DIR_UP || ev.dir == UI_DIR_DOWN)
                    {
                        s->page = s->cur / PER_PAGE;
                        show_grid(s);
                    }
                    else if (ev.dir == UI_DIR_LEFT)
                    {
                        if (s->n > 0)
                        {
                            s->cur = (s->cur + 1) % s->n;
                            show_full(s);
                        }
                    }
                    else if (ev.dir == UI_DIR_RIGHT)
                    {
                        if (s->n > 0)
                        {
                            s->cur = (s->cur + s->n - 1) % s->n;
                            show_full(s);
                        }
                    }
                }
                else if (s->mode == MODE_GRID)
                {
                    /* Rejilla: borde inferior arriba = fondo/launcher */
                    if (ev.edge == UI_EDGE_BOTTOM && ev.dir == UI_DIR_UP)
                    {
                        ir_fondo(s);
                    }
                    else
                    {
                        int pages = (s->n + PER_PAGE - 1) / PER_PAGE;

                        if (ev.dir == UI_DIR_LEFT)
                        {
                            if (s->page + 1 < pages)
                            {
                                s->page++;
                                show_grid(s);
                            }
                        }
                        else if (ev.dir == UI_DIR_RIGHT)
                        {
                            if (s->page > 0)
                            {
                                s->page--;
                                show_grid(s);
                            }
                        }
                    }
                }
            }
        }
        sleep(40);
    }
    return 0;
}
