/**
 * Snake - juego simple (LVGL). Gestos: swipe = direccion, tap = reiniciar,
 * swipe desde borde inferior hacia arriba = fondo (cooperativo).
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

#define APP_NAME "Snake"
#define APP_PATH "/apps/snake/app.elf"
#define APP_COLOR 0x4f7d3a
#define COLS 12
#define ROWS 16
#define MAXN (COLS*ROWS)
#define CX 10
#define CY 84
#define BID_UP 1
#define BID_DN 2
#define BID_LT 3
#define BID_RT 4
#define CW 25
#define CH 20

struct cell { short x, y; };
struct state {
    ui_input_t *ui; lv_obj_t *grid[ROWS][COLS]; lv_obj_t *msg;
    struct cell body[MAXN]; int len, dir, fx, fy, alive, score, slot, tick;
};

static unsigned rnd(unsigned *s){ *s = *s * 1103515245u + 12345u; return (*s>>16)&0x7fff; }

static void place_food(struct state *s){
    unsigned seed = (unsigned)millis();
    int k, ok;
    for(k=0;k<200;k++){
        int x = (int)(rnd(&seed) % COLS), y = (int)(rnd(&seed) % ROWS), i; ok=1;
        for(i=0;i<s->len;i++) if(s->body[i].x==x && s->body[i].y==y){ ok=0; break; }
        if(ok){ s->fx=x; s->fy=y; return; }
    }
    s->fx=0; s->fy=0;
}

static void draw_msg(struct state *s){
    char buf[40]; snprintf(buf,sizeof(buf), s->alive?"Puntos: %d":"Game over %d (tap)", s->score);
    lvgl_lock(); lv_label_set_text(s->msg, buf); lvgl_unlock();
}
static void draw_full(struct state *s){
    int r,c,i;
    lvgl_lock();
    for(r=0;r<ROWS;r++) for(c=0;c<COLS;c++)
        lv_obj_set_style_bg_color(s->grid[r][c], lv_color_hex(0x203018), 0);
    lv_obj_set_style_bg_color(s->grid[s->fy][s->fx], lv_color_hex(0xd94a3a), 0);
    for(i=0;i<s->len;i++)
        lv_obj_set_style_bg_color(s->grid[s->body[i].y][s->body[i].x],
            lv_color_hex(i==0?0xc8f07a:APP_COLOR), 0);
    lvgl_unlock();
    draw_msg(s);
}

static void reset(struct state *s){
    s->len=3; s->dir=1; s->alive=1; s->score=0; s->tick=0;
    s->body[0].x=COLS/2; s->body[0].y=ROWS/2;
    s->body[1].x=s->body[0].x-1; s->body[1].y=s->body[0].y;
    s->body[2].x=s->body[0].x-2; s->body[2].y=s->body[0].y;
    place_food(s); draw_full(s);
}

static void step(struct state *s){
    int i, nx=s->body[0].x, ny=s->body[0].y, ox=s->body[0].x, oy=s->body[0].y;
    struct cell tail;
    if(!s->alive) return;
    if(s->dir==0) ny--; else if(s->dir==1) nx++; else if(s->dir==2) ny++; else nx--;
    if(nx<0||nx>=COLS||ny<0||ny>=ROWS){ s->alive=0; draw_msg(s); return; }
    for(i=0;i<s->len;i++) if(s->body[i].x==nx && s->body[i].y==ny){ s->alive=0; draw_msg(s); return; }
    tail = s->body[s->len-1];
    for(i=s->len;i>0;i--) s->body[i]=s->body[i-1];
    s->body[0].x=nx; s->body[0].y=ny;
    lvgl_lock();
    lv_obj_set_style_bg_color(s->grid[oy][ox], lv_color_hex(APP_COLOR), 0); /* old head -> body */
    lv_obj_set_style_bg_color(s->grid[ny][nx], lv_color_hex(0xc8f07a), 0); /* new head */
    if(nx==s->fx && ny==s->fy){
        if(s->len<MAXN-1) s->len++; s->score++;
        place_food(s);
        lv_obj_set_style_bg_color(s->grid[s->fy][s->fx], lv_color_hex(0xd94a3a), 0);
    } else {
        lv_obj_set_style_bg_color(s->grid[tail.y][tail.x], lv_color_hex(0x203018), 0);
    }
    lvgl_unlock();
    draw_msg(s);
}

static void ir_fondo(struct state *s){
    ui_event_t ev; int st;
    if(s->slot<0) return;
    ui_show(s->ui,0); appmgr_set_state(s->slot, APP_BACKGROUND);
    for(;;){
        st=appmgr_get_state(s->slot);
        if(st==APP_RUNNING){ ui_show(s->ui,1); return; }
        if(st==APP_CLOSING||st==APP_FREE){ appmgr_unregister(s->slot); ui_destroy(s->ui); psram_free(s); exit(0); }
        while(ui_poll(s->ui,&ev)){}
        sleep(50);
    }
}

int main(int argc,char *argv[]){
    struct state *s; ui_event_t ev; int r,c; (void)argc;(void)argv;
    s=psram_malloc(sizeof(*s)); if(!s) exit(1); memset(s,0,sizeof(*s)); s->slot=-1;
    s->ui=ui_create_ex(UI_CHROME_NONE); if(!s->ui){ psram_free(s); exit(1); }
    s->slot=appmgr_register(APP_NAME, APP_PATH, APP_COLOR, gettid());
    lvgl_lock();
    s->msg=lv_label_create(ui_root(s->ui));
    lv_obj_set_style_text_color(s->msg, lv_color_hex(0xE8D8B0), 0);
    lv_obj_align(s->msg, LV_ALIGN_TOP_LEFT, 10, 58);
    for(r=0;r<ROWS;r++) for(c=0;c<COLS;c++){
        s->grid[r][c]=lv_obj_create(ui_root(s->ui));
        lv_obj_set_size(s->grid[r][c], CW-2, CH-2);
        lv_obj_set_pos(s->grid[r][c], CX+c*CW, CY+r*CH);
        lv_obj_set_style_radius(s->grid[r][c], 3, 0);
        lv_obj_set_style_border_width(s->grid[r][c], 0, 0);
        lv_obj_clear_flag(s->grid[r][c], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s->grid[r][c], LV_OBJ_FLAG_CLICKABLE);
    }
    ui_button(s->ui,BID_LT,"<", 10,416,65,50,0x3b6b2a);
    ui_button(s->ui,BID_UP,"^", 85,416,65,50,0x3b6b2a);
    ui_button(s->ui,BID_DN,"v",160,416,65,50,0x3b6b2a);
    ui_button(s->ui,BID_RT,">",235,416,65,50,0x3b6b2a);
    lvgl_unlock();
    reset(s);
    for(;;){
        if(s->slot>=0 && appmgr_get_state(s->slot)==APP_CLOSING){ appmgr_unregister(s->slot); ui_destroy(s->ui); psram_free(s); exit(0); }
        while(ui_poll(s->ui,&ev)){
            if(ev.tipo==UI_EVENT_SWIPE){
                if(ev.edge==UI_EDGE_BOTTOM && ev.dir==UI_DIR_UP){ ir_fondo(s); }
                else if(ev.dir==UI_DIR_UP && s->dir!=2) s->dir=0;
                else if(ev.dir==UI_DIR_RIGHT && s->dir!=3) s->dir=1;
                else if(ev.dir==UI_DIR_DOWN && s->dir!=0) s->dir=2;
                else if(ev.dir==UI_DIR_LEFT && s->dir!=1) s->dir=3;
            } else if(ev.tipo==UI_EVENT_BUTTON){
                if(ev.id==BID_UP && s->dir!=2) s->dir=0;
                else if(ev.id==BID_RT && s->dir!=3) s->dir=1;
                else if(ev.id==BID_DN && s->dir!=0) s->dir=2;
                else if(ev.id==BID_LT && s->dir!=1) s->dir=3;
            } else if(ev.tipo==UI_EVENT_TAP){ reset(s); }
        }
        if(++s->tick >= 6){ s->tick=0; step(s); }
        sleep(40);
    }
    return 0;
}
