/**
 * @file     xsh_touchpanel.c
 *
 * Shell command (touchpanel): read the touch device and paint each contact
 * point on the panel, coordinates written next to the finger.  Everything
 * goes through device descriptors: /dev/touch0 with read() for the points
 * and /dev/lcd0 with control() for the drawing.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <kernel.h>
#include <device.h>
#include <touch.h>
#include <lcd.h>

#define MAXPTS        5                 /* FT6x36 report block capacity   */
#define POLL_MS       20
#define MARK_HALF     8                 /* cross arm, from the contact    */

static const uint16_t pointcolor[MAXPTS] = {
    C_YELLOW, C_CYAN, C_MAGENTA, C_GREEN, C_RED
};

/** Where the last marker was drawn, so the next frame can erase it. */
struct slot
{
    int used;
    int x, y;
    int lx, ly;
    char text[14];
};

static void fillRect(int lcd, int x, int y, int w, int h, uint16_t color)
{
    struct lcd_rect r;

    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    r.color = color;
    control(lcd, LCD_CTRL_RECT, (long)&r, 0);
}

static void drawText(int lcd, const char *s, int x, int y, uint16_t fg,
                     uint16_t bg)
{
    struct lcd_text t;

    t.s = s;
    t.x = x;
    t.y = y;
    t.fg = fg;
    t.bg = bg;
    control(lcd, LCD_CTRL_TEXT, (long)&t, 0);
}

/** Cross centred on the contact, clipped to the panel. */
static void drawMark(int lcd, int x, int y, uint16_t color)
{
    int x0 = (x - MARK_HALF < 0) ? 0 : x - MARK_HALF;
    int x1 = (x + MARK_HALF >= LCD_W) ? LCD_W - 1 : x + MARK_HALF;
    int y0 = (y - MARK_HALF < 0) ? 0 : y - MARK_HALF;
    int y1 = (y + MARK_HALF >= LCD_H) ? LCD_H - 1 : y + MARK_HALF;

    fillRect(lcd, x0, y, x1 - x0 + 1, 2, color);
    fillRect(lcd, x, y0, 2, y1 - y0 + 1, color);
}

/** Place the label beside the mark, flipping to the left near the edge. */
static void labelpos(int x, int y, const char *s, int *lx, int *ly)
{
    int w = 8 * (int)strlen(s);

    *lx = x + MARK_HALF + 4;
    if (*lx + w >= LCD_W)
    {
        *lx = x - MARK_HALF - 4 - w;
    }
    if (*lx < 0)
    {
        *lx = 0;
    }

    *ly = y - LCD_TEXT_H / 2;
    if (*ly < 0)
    {
        *ly = 0;
    }
    if (*ly + LCD_TEXT_H >= LCD_H)
    {
        *ly = LCD_H - LCD_TEXT_H - 1;
    }
}

static void eraseSlot(int lcd, struct slot *sl)
{
    if (!sl->used)
    {
        return;
    }
    drawMark(lcd, sl->x, sl->y, C_BLACK);
    drawText(lcd, sl->text, sl->lx, sl->ly, C_BLACK, C_BLACK);
    sl->used = FALSE;
}

static void paintSlot(int lcd, struct slot *sl, int i, const touch_point_t *p)
{
    char text[14];

    sprintf(text, "%u,%u", (unsigned)p->x, (unsigned)p->y);

    /* Same spot as last frame: leave the marker alone, SPI is the slow
     * part of this loop. */
    if (sl->used && sl->x == p->x && sl->y == p->y)
    {
        return;
    }

    eraseSlot(lcd, sl);

    drawMark(lcd, p->x, p->y, pointcolor[i]);
    labelpos(p->x, p->y, text, &sl->lx, &sl->ly);
    drawText(lcd, text, sl->lx, sl->ly, pointcolor[i], C_BLACK);

    sl->used = TRUE;
    sl->x = p->x;
    sl->y = p->y;
    strlcpy(sl->text, text, sizeof(sl->text));
}

/**
 * @ingroup shell
 *
 * Shell command (touchpanel [SECONDS]).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_touchpanel(int nargs, char *args[])
{
    struct slot slots[MAXPTS];
    touch_point_t pts[MAXPTS];
    int touchfd;
    int lcdfd;
    int seconds = 60;
    int rounds;
    int n;
    int i;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s [SECONDS]\n\n", args[0]);
        printf("Description:\n");
        printf("\tPolls /dev/touch0 every %d ms for SECONDS (default 60),\n",
               POLL_MS);
        printf("\tdrawing each contact on the panel with its coordinates.\n");
        printf("\tPress and drag: the marker follows the finger.\n");
        return OK;
    }

    if (nargs == 2)
    {
        seconds = atoi(args[1]);
    }
    else if (nargs > 2)
    {
        fprintf(stderr, "%s: too many arguments\n", args[0]);
        return SYSERR;
    }

    if (seconds <= 0)
    {
        fprintf(stderr, "%s: invalid duration\n", args[0]);
        return SYSERR;
    }

    touchfd = getdev("/dev/touch0");
    if (SYSERR == touchfd)
    {
        fprintf(stderr, "%s: /dev/touch0 is not mounted\n", args[0]);
        return SYSERR;
    }

    lcdfd = getdev("/dev/lcd0");
    if (SYSERR == lcdfd)
    {
        fprintf(stderr, "%s: /dev/lcd0 is not mounted\n", args[0]);
        return SYSERR;
    }

    if (control(touchfd, TOUCH_CTRL_TYPE, 0, 0) == TOUCH_NONE)
    {
        fprintf(stderr, "%s: no touch controller detected\n", args[0]);
        close(touchfd);
        return SYSERR;
    }

    memset(slots, 0, sizeof(slots));
    control(lcdfd, LCD_CTRL_FILL, C_BLACK, 0);
    drawText(lcdfd, "toca el cristal", 8, 8, C_WHITE, C_BLACK);
    printf("%s: %d s, suelta con un dedo\n", args[0], seconds);

    rounds = seconds * 1000 / POLL_MS;
    while (rounds-- > 0)
    {
        n = read(touchfd, pts, MAXPTS);
        if (n < 0)
        {
            n = 0;
        }

        for (i = 0; i < MAXPTS; i++)
        {
            if (i < n)
            {
                paintSlot(lcdfd, &slots[i], i, &pts[i]);
            }
            else
            {
                eraseSlot(lcdfd, &slots[i]);
            }
        }

        sleep(POLL_MS);
    }

    close(touchfd);
    printf("%s: fin\n", args[0]);
    return OK;
}
