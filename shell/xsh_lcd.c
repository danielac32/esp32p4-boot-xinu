/*
 * @file xsh_lcd.c
 *
 * Shell command: drive the ST7796 panel through the LCD device, resolving it
 * by path so "/dev/lcd0" is the only name the user has to know.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <device.h>
#include <lcd.h>
#include <lvgl.h>                   /* lvglShotBmp: volcado del espejo */
#include "lvgl/lvgl.h"              /* API LVGL: bootTask dibuja con widgets */
#include <memory.h>
#include <platform.h>
#include <shell.h>
#include <version.h>

/** Text grid of the panel: 40 columns by 30 rows of 8x16 glyphs. */
#define SCREEN_COLS (LCD_W / LCD_TEXT_W)
#define SCREEN_ROWS (LCD_H / LCD_TEXT_H)

/* Kernel entry point (loader/platforms/esp32p4/start.S), same base main.c
 * uses to report the code size. */
extern void _start(void);

struct colorname
{
    const char *name;
    uint16_t    rgb;
};

static const struct colorname colors[] = {
    { "black",    C_BLACK    },
    { "white",    C_WHITE    },
    { "red",      C_RED      },
    { "green",    C_GREEN    },
    { "blue",     C_BLUE     },
    { "yellow",   C_YELLOW   },
    { "cyan",     C_CYAN     },
    { "magenta",  C_MAGENTA  },
    { "gray",     C_GRAY     },
    { "darkblue", C_DARKBLUE },
};

static int colorlookup(const char *s, uint16_t *out)
{
    size_t i;

    for (i = 0; i < sizeof(colors) / sizeof(colors[0]); i++)
    {
        if (0 == strcmp(s, colors[i].name))
        {
            *out = colors[i].rgb;
            return OK;
        }
    }
    return SYSERR;
}

/**
 * Draw one string on the given text row, wrapping onto the rows below when it
 * is wider than the panel.
 * @return the next free row
 */
static int drawLine(int dev, int row, const char *s, uint16_t fg)
{
    struct lcd_text text;
    char chunk[SCREEN_COLS + 1];
    int n;

    text.x = 0;
    text.fg = fg;
    text.bg = C_BLACK;

    while (*s != '\0' && row < SCREEN_ROWS)
    {
        for (n = 0; s[n] != '\0' && n < SCREEN_COLS; n++)
        {
            chunk[n] = s[n];
        }
        chunk[n] = '\0';
        text.s = chunk;
        text.y = row * LCD_TEXT_H;
        if (SYSERR == control(dev, LCD_CTRL_TEXT, (long)&text, 0))
        {
            return row;
        }
        s += n;
        row++;
    }
    return row;
}

/**
 * Draw a '\n'-separated block, which is the shape the banner strings in
 * <shell.h> come in.
 * @return the next free row
 */
static int drawBlock(int dev, int row, const char *s, uint16_t fg)
{
    char line[80];
    int n;

    while (*s != '\0' && row < SCREEN_ROWS)
    {
        for (n = 0; s[n] != '\0' && s[n] != '\n'
                 && n < (int)sizeof(line) - 1; n++)
        {
            line[n] = s[n];
        }
        line[n] = '\0';
        if (n > 0)
        {
            row = drawLine(dev, row, line, fg);
        }
        s += n;
        if (*s == '\n')
        {
            s++;
        }
    }
    return row;
}

/**
 * Reproduce the serial boot banner on the panel, phosphor colours on black:
 * build stamp, the Xinu art, and the memory layout the kernel printed at
 * startup, followed by the mounted devices.
 */
static void drawBootScreen(int dev)
{
    char line[SCREEN_COLS + 1];
    int row;
    int i;

    control(dev, LCD_CTRL_FILL, C_BLACK, 0);

    row = drawLine(dev, 0, VERSION, C_AMBER);
    row = drawBlock(dev, row + 1, SHELL_BANNER_NONVT100, C_GREEN);
    row = drawLine(dev, row + 1, SHELL_START, C_WHITE);

    sprintf(line, "%lu bytes physical.",
            (unsigned long)((uintptr_t)platform.maxaddr -
                            (uintptr_t)platform.minaddr));
    row = drawLine(dev, row + 1, line, C_CYAN);
    sprintf(line, "%lu bytes Xinu code.",
            (unsigned long)((uintptr_t)&_etext - (uintptr_t)_start));
    row = drawLine(dev, row, line, C_CYAN);
    sprintf(line, "%lu bytes stack.",
            (unsigned long)((uintptr_t)memheap - (uintptr_t)&_end));
    row = drawLine(dev, row, line, C_CYAN);
    sprintf(line, "%lu bytes heap.",
            (unsigned long)((uintptr_t)platform.maxaddr -
                            (uintptr_t)memheap));
    row = drawLine(dev, row, line, C_CYAN);

    row = drawLine(dev, row + 1, "Mounted devices", C_YELLOW);
    for (i = 0; i < NDEVS && row < SCREEN_ROWS; i++)
    {
        if (devtab[i].name == NULL)
        {
            continue;
        }
        sprintf(line, "/dev/%-11s id: %d", devtab[i].name, devtab[i].num);
        row = drawLine(dev, row, line, C_GRAY);
    }
}

/**
 * @ingroup shell
 *
 * Shell command (lcd).
 * @param nargs  number of arguments in args array
 * @param args   array of arguments
 * @return OK for success, SYSERR for syntax error
 */

int bootTask(void)
{
    lv_obj_t *scr, *root, *lbl;
    char line[80];
    int i, y;

    kprintf("[boot] inicio\r\n");
    if (SYSERR == control(LVGL0, LVGL_CTRL_READY, 0, 0))
    {
        kprintf("[boot] LVGL no listo, omito\r\n");
        return OK;
    }

    lvgl_lock();
    scr = lv_scr_act();

    /* Contenedor propio: NO limpiar lv_scr_act() (rompe p4demoTask/launcher).
     * Creamos un panel, lo mostramos un instante y borramos solo ese panel. */
    root = lv_obj_create(scr);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, 320, 480);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_make(0, 0, 0), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    y = 2;
    lbl = lv_label_create(root);
    lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, LV_PART_MAIN);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(lbl, 316);
    lv_label_set_text(lbl, "Xinu-P4.2 / ESP32-P4  " VERSION);
    lv_obj_set_style_text_color(lbl, lv_color_make(255, 180, 0), LV_PART_MAIN);
    lv_obj_set_pos(lbl, 2, y);
    y += 12;

    lbl = lv_label_create(root);
    lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, LV_PART_MAIN);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, 316);
    lv_label_set_text(lbl, SHELL_START);
    lv_obj_set_style_text_color(lbl, lv_color_make(0, 220, 0), LV_PART_MAIN);
    lv_obj_set_pos(lbl, 2, y);
    y += 22;

    sprintf(line, "phys %luk  code %luk\nstk  %luk  heap %luk",
            (unsigned long)(((uintptr_t)platform.maxaddr - (uintptr_t)platform.minaddr) / 1024UL),
            (unsigned long)(((uintptr_t)&_etext - (uintptr_t)_start) / 1024UL),
            (unsigned long)(((uintptr_t)memheap - (uintptr_t)_end) / 1024UL),
            (unsigned long)(((uintptr_t)platform.maxaddr - (uintptr_t)memheap) / 1024UL));
    lbl = lv_label_create(root);
    lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, LV_PART_MAIN);
    lv_label_set_text(lbl, line);
    lv_obj_set_style_text_color(lbl, lv_color_make(0, 220, 220), LV_PART_MAIN);
    lv_obj_set_pos(lbl, 2, y);
    y += 22;

    lbl = lv_label_create(root);
    lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, LV_PART_MAIN);
    lv_label_set_text(lbl, "Mounted devices:");
    lv_obj_set_style_text_color(lbl, lv_color_make(255, 255, 0), LV_PART_MAIN);
    lv_obj_set_pos(lbl, 2, y);
    y += 10;

    for (i = 0; i < NDEVS && y < 468; i++)
    {
        if (devtab[i].name == NULL) { continue; }
        sprintf(line, "/dev/%-9s %2d", devtab[i].name, devtab[i].num);
        lbl = lv_label_create(root);
        lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, LV_PART_MAIN);
        lv_label_set_text(lbl, line);
        lv_obj_set_style_text_color(lbl, lv_color_make(180, 180, 180), LV_PART_MAIN);
        lv_obj_set_pos(lbl, 2, y);
        y += 9;
    }

    lvgl_unlock();
    kprintf("[boot] dibujando (LVGL)...\r\n");

    /* Tiempo de lectura del banner.  Tres resched() no bastaban: 'lvglui' se
     * duerme entre pasadas y un frame completo son decenas de milisegundos de
     * cable, asi que root se borraba antes de llegar a pintarse.  sleep() cede
     * la CPU y deja que la hebra de la UI haga sus flush. */
    sleep(1500);

    lvgl_lock();
    lv_obj_del(root);
    lvgl_unlock();

    kprintf("[boot] listo\r\n");
    return 0;
}

shellcmd xsh_lcd(int nargs, char *args[])
{
    int             dev;
    int             rc;
    struct lcd_rect rect;
    struct lcd_text text;
    char            line[64];
    char           *p;
    int             i;

    if (nargs >= 2 && 0 == strcmp(args[1], "--help"))
    {
        printf("Usage: lcd [demo | boot | fill <color> | rect <x> <y> <w> <h> <color>\n");
        printf("           | text <x> <y> <fg> <bg> <words...>\n");
        printf("           | rot <0|2|4|6> | bl on|off | speed [n] | clock [hz]\n");
        printf("           | shot [ruta.bmp]\n\n");
        printf("Description:\n");
        printf("\tDraw on the ST7796 panel (/dev/lcd0).\n");
        printf("\tWith no argument (or 'demo'), draws the bring-up pattern.\n");
        printf("\tboot redraws the serial boot banner on the panel.\n");
        printf("\tThe panel is a boot display, not a console: the shell stays on\n");
        printf("\tthe serial line and the boot log is painted once at startup.\n");
        printf("\trot picks the portrait orientation; redraw\n");
        printf("\t(demo/text/rect) after changing it, the panel only remaps what\n");
        printf("\tis already in its RAM.\n");
        printf("Colors: black white red green blue yellow cyan magenta gray darkblue\n");
        return OK;
    }

    /* Resolve the panel by path: getdev() falls back to the namespace. */
    dev = getdev("/dev/lcd0");
    if (SYSERR == dev)
    {
        fprintf(stderr, "lcd: /dev/lcd0 is not mounted\n");
        return SYSERR;
    }

    if (1 == nargs || 0 == strcmp(args[1], "demo"))
    {
        printf("lcd: drawing the bring-up pattern...\n");
        if (SYSERR == control(dev, LCD_CTRL_DEMO, 0, 0))
        {
            fprintf(stderr, "lcd: panel not available\n");
            return SYSERR;
        }
        printf("lcd: done\n");
        return OK;
    }

    if (0 == strcmp(args[1], "boot"))
    {
        printf("lcd: drawing the boot banner...\n");
        drawBootScreen(dev);
        printf("lcd: done\n");
        return OK;
    }

    if (0 == strcmp(args[1], "fill"))
    {
        uint16_t rgb;

        if (3 != nargs || SYSERR == colorlookup(args[2], &rgb))
        {
            fprintf(stderr, "lcd: usage: lcd fill <color>\n");
            return SYSERR;
        }
        return control(dev, LCD_CTRL_FILL, (long)rgb, 0);
    }

    if (0 == strcmp(args[1], "rect"))
    {
        if (7 != nargs || SYSERR == colorlookup(args[6], &rect.color))
        {
            fprintf(stderr, "lcd: usage: lcd rect <x> <y> <w> <h> <color>\n");
            return SYSERR;
        }
        rect.x = atoi(args[2]);
        rect.y = atoi(args[3]);
        rect.w = atoi(args[4]);
        rect.h = atoi(args[5]);
        return control(dev, LCD_CTRL_RECT, (long)&rect, 0);
    }

    if (0 == strcmp(args[1], "text"))
    {
        if (7 > nargs)
        {
            fprintf(stderr,
                    "lcd: usage: lcd text <x> <y> <fg> <bg> <words...>\n");
            return SYSERR;
        }
        text.x = atoi(args[2]);
        text.y = atoi(args[3]);
        if (SYSERR == colorlookup(args[4], &text.fg)
            || SYSERR == colorlookup(args[5], &text.bg))
        {
            fprintf(stderr, "lcd: unknown color\n");
            return SYSERR;
        }

        /* The tokenizer split the string on spaces; join it back. */
        p = line;
        for (i = 6; i < nargs; i++)
        {
            size_t len = strlen(args[i]);

            if ((size_t)(p - line) + len + 1 >= sizeof(line))
            {
                break;
            }
            if (i > 6)
            {
                *p++ = ' ';
            }
            memcpy(p, args[i], len);
            p += len;
        }
        *p = '\0';
        text.s = line;
        rc = control(dev, LCD_CTRL_TEXT, (long)&text, 0);
        printf("lcd: wrote \"%s\" at (%d,%d)\n", line, text.x, text.y);
        return rc;
    }

    if (0 == strcmp(args[1], "clock"))
    {
        long hz = (nargs >= 3) ? atol(args[2]) : 0;

        return control(dev, LCD_CTRL_CLOCK, hz, 0);
    }

    if (0 == strcmp(args[1], "speed"))
    {
        int iters = (nargs >= 3) ? atoi(args[2]) : 10;

        return control(dev, LCD_CTRL_SPEED, (long)iters, 0);
    }

    if (0 == strcmp(args[1], "rot"))
    {
        int rot;

        if (3 != nargs)
        {
            fprintf(stderr, "lcd: usage: lcd rot <0|2|4|6>\n");
            return SYSERR;
        }
        rot = atoi(args[2]);
        if (SYSERR == control(dev, LCD_CTRL_ORIENT, rot, 0))
        {
            fprintf(stderr,
                    "lcd: rotation %d is not a portrait orientation "
                    "(use 0, 2, 4 or 6)\n", rot);
            return SYSERR;
        }
        printf("lcd: now at rotation %d; redraw to fit\n", rot);
        return OK;
    }

    if (0 == strcmp(args[1], "bl"))
    {
        if (3 != nargs)
        {
            fprintf(stderr, "lcd: usage: lcd bl on|off\n");
            return SYSERR;
        }
        if (0 == strcmp(args[2], "on"))
        {
            return control(dev, LCD_CTRL_BACKLIGHT, 1, 0);
        }
        if (0 == strcmp(args[2], "off"))
        {
            return control(dev, LCD_CTRL_BACKLIGHT, 0, 0);
        }
        fprintf(stderr, "lcd: usage: lcd bl on|off\n");
        return SYSERR;
    }

    /* Foto de lo que LVGL tiene pintado: el espejo del fotograma, no el panel.
     * Sirve para mirar la interfaz desde un PC sin tener la placa delante. */
    if (0 == strcmp(args[1], "shot"))
    {
        const char *ruta = (nargs >= 3) ? args[2] : "/shot.bmp";

        if (SYSERR == lvglShotBmp(ruta))
        {
            fprintf(stderr, "lcd: no pude escribir %s\n", ruta);
            return SYSERR;
        }
        printf("lcd: foto -> %s\n", ruta);
        return OK;
    }

    fprintf(stderr, "lcd: unknown subcommand '%s'\n", args[1]);
    fprintf(stderr, "Try 'lcd --help' for more information\n");
    return SYSERR;
}
