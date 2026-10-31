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

int bootTask(int dev){
    int lcddev = getdev("/dev/lcd0");
    if (SYSERR == lcddev)
    {
        fprintf(stderr, "lcd: /dev/lcd0 is not mounted\n");
        return SYSERR;
    }
    drawBootScreen(lcddev);
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
        printf("           | rot <0|2|4|6> | bl on|off]\n\n");
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

    fprintf(stderr, "lcd: unknown subcommand '%s'\n", args[1]);
    fprintf(stderr, "Try 'lcd --help' for more information\n");
    return SYSERR;
}
