/**
 * @file musica.c
 *
 * Reproductor de MP3.  Lista /music, reproduce con previo/play-pausa/siguiente
 * y espectro.  La decodificacion/entrega corre en una tarea aparte, asi que la
 * musica sigue sonando al volver del reproductor a la lista y tambien en segundo
 * plano.  Al cerrar la app se detiene esa tarea de forma cooperativa.
 * Sin coma flotante, estado en PSRAM.
 */
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <timer.h>
#include <memory.h>
#include <device.h>
#include <thread.h>
#include <audio.h>
#include <mp3.h>
#define FAT_INLINE inline
#include <fat_filelib.h>
#include <lvgl.h>
#include <appmgr.h>
#include "gestos.h"

void exit(int codigo);

#define CARPETA    "/music"
#define MAX_TEMAS  24
#define LARGO_NOM  64
#define TRAMO      1024
#define BANDAS     8

#define P_LISTA 0
#define P_REPRO 1

#define ID_TEMA    1            /* 1 + indice */
#define ID_PREV    40
#define ID_PLAY    41
#define ID_NEXT    42
#define ID_SALIR   43
#define ID_VOLVER  44
#define ID_VOL_DN  45
#define ID_VOL_UP  46

#define CMD_NONE   0
#define CMD_PLAY   1            /* cmd_track = indice */
#define CMD_PAUSE  2
#define CMD_RESUME 3
#define CMD_STOP   4

#define C_FONDO  0x1B1408
#define C_PANEL  0x2A1E10
#define C_TEXTO  0xE8D8B0
#define C_FILA   0xE8D8B0
#define C_ACT    0xFFB302
#define C_BTN    0x3A2A14
#define C_VERDE  0x9BD14F
#define C_ROJO   0xC0392B

#define X_BARRA    20
#define PASO_BARRA 36
#define AN_BARRA   26
#define Y_BASE     300
#define AL_MAX     110
#define AL_MIN     4

struct musica
{
    ui_input_t *g;
    int pantalla;
    char nombres[MAX_TEMAS][LARGO_NOM];
    int total;
    volatile int tema;
    volatile int sonando;
    int audio;
    short *pcm;
    short *mix;
    int altura[BANDAS];
    int pico[BANDAS];
    unsigned tic_espectro;
    unsigned tic_reloj;
    lv_obj_t *titulo;
    lv_obj_t *tiempo;
    lv_obj_t *info;
    lv_obj_t *panel;
    lv_obj_t *baras[BANDAS];
    lv_obj_t *hint;
    lv_obj_t *ltit;
    lv_obj_t *vol_lbl;
    int vol;
    int slot;
    volatile int cmd;
    volatile int cmd_track;
    volatile int quit;
    volatile int repro_alive;
    tid_typ repro_tid;
    tid_typ main_tid;
};

static int acaba_mp3(const char *n)
{
    int l = (int)strlen(n);
    if (l < 4) return 0;
    n += l - 4;
    return (n[0]=='.' && (n[1]=='m'||n[1]=='M') && (n[2]=='p'||n[2]=='P') && (n[3]=='3'));
}

static void nombre_corto(char *d, const char *s, int m)
{
    int i, n = (int)strlen(s);
    if (n > 4) n -= 4;
    if (n >= m) n = m - 1;
    for (i = 0; i < n; i++) d[i] = ('.' == s[i]) ? ' ' : s[i];
    d[n] = '\0';
}

static void listado(struct musica *m)
{
    FL_DIR dir; fl_dirent ent; int n = 0;
    m->total = 0;
    if (NULL == fl_opendir(CARPETA, &dir)) { printf("musica: no %s\r\n", CARPETA); return; }
    while (0 == fl_readdir(&dir, &ent))
    {
        if (ent.is_dir || !acaba_mp3(ent.filename)) continue;
        if (n >= MAX_TEMAS) break;
        memcpy(m->nombres[n], ent.filename, LARGO_NOM);
        m->nombres[n][LARGO_NOM - 1] = '\0'; n++;
    }
    fl_closedir(&dir);
    m->total = n;
    printf("musica: %d pistas\r\n", n);
}

static void espectro_calc(struct musica *m, int frames)
{
    short *mix = m->mix; const short *p = m->pcm;
    int nivel[BANDAS]; int i,b,largo,ancho,suma,e,h,pic;
    for (i = 0; i < frames; i++) mix[i] = (short)(((int)p[2*i] + (int)p[2*i+1]) >> 1);
    largo = frames;
    for (b = 0; b < BANDAS; b++) {
        if (largo < 4) { nivel[b]=0; continue; }
        suma=0; for (i=0;i<largo;i++) suma += mix[i]<0 ? -(int)mix[i] : (int)mix[i];
        nivel[b]=suma/largo; ancho=largo>>1;
        for (i=0;i<ancho;i++) mix[i]=(short)(((int)mix[2*i]+(int)mix[2*i+1])>>1);
        largo=ancho;
    }
    for (i=0;i<BANDAS;i++) {
        int k = BANDAS-1-i;
        e = nivel[k]; if (k+1 < BANDAS && nivel[k+1] < e) e -= nivel[k+1];
        pic = m->pico[i]; if (e>pic) pic=e; else pic -= pic>>4; if (pic<64) pic=64; m->pico[i]=pic;
        h = AL_MIN + (int)((long)e * (AL_MAX-AL_MIN) / pic);
        if (h>AL_MAX) h=AL_MAX; if (h < m->altura[i]-3) h = m->altura[i]-3; m->altura[i]=h;
    }
}

static void espectro_pintar(struct musica *m)
{
    int i,h; unsigned c;
    /* Evita contender por el lock global cuando no se esta en el reproductor. */
    if (P_REPRO != m->pantalla) return;
    lvgl_lock();
    if (P_REPRO != m->pantalla) { lvgl_unlock(); return; }
    for (i=0;i<BANDAS;i++) {
        h = m->altura[i];
        c = (h*100/AL_MAX < 70) ? C_VERDE : ((h*100/AL_MAX < 88) ? C_ACT : C_ROJO);
        if (m->baras[i]) {
            lv_obj_set_size(m->baras[i], AN_BARRA, h);
            lv_obj_set_pos(m->baras[i], X_BARRA + i*PASO_BARRA, Y_BASE - h);
            lv_obj_set_style_bg_color(m->baras[i], lv_color_hex(c), 0);
        }
    }
    lvgl_unlock();
}

static void espectro_cero(struct musica *m)
{
    int i; for (i=0;i<BANDAS;i++){ m->altura[i]=AL_MIN; m->pico[i]=0; }
    espectro_pintar(m);
}

static void set_txt(lv_obj_t *o, const char *t)
{
    if (!o) return; lvgl_lock(); lv_label_set_text(o,t); lvgl_unlock();
}

static void info_pintar(struct musica *m)
{
    char t[64], nom[36];
    unsigned ya, todo;
    int tema;

    /* Lee estado mp3 fuera del lock LVGL; actualiza labels bajo lock y
     * revalidando pantalla/punteros para no tocar objetos ya borrados. */
    if (P_REPRO != m->pantalla) return;
    tema = m->tema;
    if (tema < 0) return;
    ya = mp3_ms_actual(); todo = mp3_ms_total();
    nombre_corto(nom, m->nombres[tema], 28);

    lvgl_lock();
    if (P_REPRO == m->pantalla && m->titulo)
    {
        lv_label_set_text(m->titulo, nom);
        snprintf(t,sizeof(t), "%u:%02u / %u:%02u  %dk",
                 ya/60000U,(ya/1000U)%60U,todo/60000U,(todo/1000U)%60U,mp3_kbps());
        if (m->tiempo) lv_label_set_text(m->tiempo, t);
        snprintf(t,sizeof(t), "track %d/%d  %dHz", tema+1, m->total, mp3_hz());
        if (m->info) lv_label_set_text(m->info, t);
    }
    lvgl_unlock();
}

static void callar(struct musica *m)
{
    if (m->sonando) { control(m->audio, AUDIO_CTRL_STREAM, 0, 0); m->sonando = 0; }
    { int i; for (i=0;i<BANDAS;i++){ m->altura[i]=AL_MIN; m->pico[i]=0; } }
}

static void sonar(struct musica *m)
{
    if (m->tema < 0 || m->audio < 0 || m->sonando) return;
    if (SYSERR == control(m->audio, AUDIO_CTRL_STREAM, 1, 0)) return;
    m->sonando = 1;
    m->tic_espectro = millis(); m->tic_reloj = m->tic_espectro;
    ui_button_set_label(m->g, ID_PLAY, "||");
}

static void pausar(struct musica *m)
{
    if (!m->sonando) return;
    control(m->audio, AUDIO_CTRL_STREAM_WAIT, 0, 0);
    control(m->audio, AUDIO_CTRL_STREAM, 0, 0);
    m->sonando = 0; espectro_cero(m);
    ui_button_set_label(m->g, ID_PLAY, ">");
}

static int cargar(struct musica *m, int i)
{
    char ruta[96];
    if (i < 0 || i >= m->total) return 0;
    snprintf(ruta, sizeof(ruta), "%s/%s", CARPETA, m->nombres[i]);
    if (OK != mp3_abrir(ruta)) { printf("musica: no abre %s\r\n", ruta); m->tema=-1; return 0; }
    m->tema = i;
    printf("musica: %s %dHz %dk\r\n", ruta, mp3_hz(), mp3_kbps());
    return 1;
}

static int entregar(struct musica *m, int bytes)
{
    const char *p = (const char *)m->pcm; int quedan = bytes, r=0;
    while (quedan > 0) {
        int s = write(m->audio, p, (uint)quedan);
        if (s <= 0) { if (++r > 8) return 0; sleep(2); continue; }
        r=0; p+=s; quedan-=s;
    }
    return 1;
}

static void info_pintar(struct musica *m);
static void saltar_cmd(struct musica *m, int dest);

static void avanzar(struct musica *m)
{
    unsigned ahora = millis();
    int n = mp3_leer(m->pcm, TRAMO);
    if (n <= 0) {
        callar(m); ui_button_set_label(m->g, ID_PLAY, ">");
        if (m->tema >= 0 && m->tema + 1 < m->total) saltar_cmd(m, m->tema + 1);
        return;
    }
    if (!entregar(m, n * 4)) { pausar(m); return; }
    if (ahora - m->tic_espectro >= 42) { m->tic_espectro = ahora; espectro_calc(m,n); espectro_pintar(m); }
    if (ahora - m->tic_reloj >= 250) { m->tic_reloj = ahora; info_pintar(m); }
}

static void saltar_cmd(struct musica *m, int dest)
{
    if (dest < 0) dest = 0; if (dest >= m->total) return;
    callar(m); ui_button_set_label(m->g, ID_PLAY, ">");
    if (cargar(m,dest)) { if (P_REPRO==m->pantalla) info_pintar(m); sonar(m); }
}

/* Tarea de reproduccion: decodifica y entrega audio aunque la UI cambie
 * (lista/reproductor) o la app este en segundo plano. */
static int repro_task(struct musica *m)
{
    int cmd;
    m->repro_alive = 1;
    while (!m->quit)
    {
        cmd = m->cmd;
        if (CMD_NONE != cmd)
        {
            int tr = m->cmd_track;
            m->cmd = CMD_NONE;
            if (CMD_PLAY == cmd)
            {
                callar(m);
                if (cargar(m, tr))
                {
                    if (P_REPRO == m->pantalla) info_pintar(m);
                    sonar(m);
                }
            }
            else if (CMD_PAUSE == cmd)
            {
                pausar(m);
            }
            else if (CMD_RESUME == cmd)
            {
                sonar(m);
            }
            else if (CMD_STOP == cmd)
            {
                callar(m); mp3_cerrar();
            }
        }
        if (m->quit) break;
        if (m->sonando)
        {
            int k;
            /* Mantener el anillo lo mas lleno posible para sobrevivir a
             * bloqueos de CPU/FS al cargar otra app. entregar() -> audioWrite
             * bloquea cuando el anillo se llena (marca el ritmo). */
            for (k = 0; k < 12 && m->sonando && !m->quit && CMD_NONE == m->cmd; k++)
            {
                avanzar(m);
            }
        }
        else
        {
            sleep(30);
        }
    }
    callar(m);
    mp3_cerrar();
    m->repro_alive = 0;
    send(m->main_tid, 1);
    return 0;
}

static void limpiar_repro(struct musica *m)
{
    int i;
    lvgl_lock();
    if (m->titulo)     { lv_obj_del(m->titulo);     m->titulo=NULL; }
    if (m->tiempo)     { lv_obj_del(m->tiempo);     m->tiempo=NULL; }
    if (m->info)       { lv_obj_del(m->info);       m->info=NULL; }
    if (m->hint)    { lv_obj_del(m->hint);    m->hint=NULL; }
    if (m->vol_lbl) { lv_obj_del(m->vol_lbl); m->vol_lbl=NULL; }
    for (i=0;i<BANDAS;i++) {
        if (m->baras[i]) { lv_obj_del(m->baras[i]); m->baras[i]=NULL; }
        m->altura[i]=AL_MIN; m->pico[i]=0;
    }
    if (m->panel)  { lv_obj_del(m->panel);  m->panel=NULL; }
    lvgl_unlock();
}

static void pantalla_lista(struct musica *m)
{
    char rot[48]; int i;
    /* Cambiar pantalla ANTES de borrar: el hilo de reproduccion deja de pintar. */
    m->pantalla = P_LISTA;
    sleep(2); /* cede para que el hilo de reproduccion vea el cambio */
    ui_buttons_clear(m->g);
    limpiar_repro(m);
    m->ltit = NULL;

    lvgl_lock();
    m->ltit = lv_label_create(ui_root(m->g));
    snprintf(rot,sizeof(rot),"MUSIC  %d tracks", m->total);
    lv_label_set_text(m->ltit, m->total ? rot : "MUSIC");
    lv_obj_set_style_text_color(m->ltit, lv_color_hex(C_TEXTO), 0);
    lv_obj_align(m->ltit, LV_ALIGN_TOP_LEFT, 10, 8);
    lvgl_unlock();

    ui_button(m->g, ID_SALIR, "X", 276, 6, 36, 32, C_ROJO);
    for (i=0; i<m->total && i<MAX_TEMAS; i++) {
        char num[40]; unsigned c = (i==m->tema)?C_ACT:C_FILA;
        nombre_corto(rot, m->nombres[i], 24);
        snprintf(num,sizeof(num), "%02d  %s", i+1, rot);
        ui_button(m->g, ID_TEMA+i, num, 10, 44 + i*42, 300, 38, c);
    }
}

static void pantalla_repro(struct musica *m)
{
    int i;
    m->pantalla = P_REPRO;
    ui_buttons_clear(m->g);
    if (m->ltit) { 
        lvgl_lock(); 
        lv_obj_del(m->ltit); 
        m->ltit=NULL; 
        lvgl_unlock(); 
    }
    limpiar_repro(m);

    lvgl_lock();
    m->titulo = lv_label_create(ui_root(m->g));
    lv_label_set_text(m->titulo,"-"); 
    lv_obj_set_style_text_color(m->titulo,lv_color_hex(C_TEXTO),0);
    /* Una sola linea acotada: no choca con la X (x=276) ni solapa el tiempo. */
    lv_obj_set_size(m->titulo, 250, 20);
    lv_label_set_long_mode(m->titulo, LV_LABEL_LONG_DOT);
    lv_obj_align(m->titulo, LV_ALIGN_TOP_LEFT, 10, 10);
    m->tiempo = lv_label_create(ui_root(m->g));
    lv_label_set_text(m->tiempo,""); 
    lv_obj_set_style_text_color(m->tiempo,lv_color_hex(C_TEXTO),0);
    lv_obj_align(m->tiempo, LV_ALIGN_TOP_LEFT, 10, 34);
    m->info = lv_label_create(ui_root(m->g));
    lv_label_set_text(m->info,""); 
    lv_obj_set_style_text_color(m->info,lv_color_hex(C_TEXTO),0);
    lv_obj_align(m->info, LV_ALIGN_TOP_LEFT, 10, 56);
    /* Volumen (botones), arriba de la animacion del espectro. */
    {
        char vt[16];
        m->vol_lbl = lv_label_create(ui_root(m->g));
        snprintf(vt, sizeof(vt), "VOL %d", m->vol);
        lv_label_set_text(m->vol_lbl, vt);
        lv_obj_set_style_text_color(m->vol_lbl, lv_color_hex(C_TEXTO), 0);
        lv_obj_align(m->vol_lbl, LV_ALIGN_TOP_LEFT, 128, 138);
    }
    m->panel = lv_obj_create(ui_root(m->g));
    lv_obj_set_size(m->panel,304,130); 
    lv_obj_align(m->panel, LV_ALIGN_TOP_LEFT, 8,170);
    lv_obj_set_style_bg_color(m->panel,lv_color_hex(C_PANEL),0);
    lv_obj_set_style_bg_grad_dir(m->panel,LV_GRAD_DIR_NONE,0);
    lv_obj_set_style_border_width(m->panel,0,0); 
    lv_obj_set_style_radius(m->panel,8,0);
    lv_obj_clear_flag(m->panel, LV_OBJ_FLAG_SCROLLABLE); 
    lv_obj_clear_flag(m->panel, LV_OBJ_FLAG_CLICKABLE);
    for (i=0;i<BANDAS;i++) {
        m->baras[i]=lv_obj_create(ui_root(m->g));
        lv_obj_set_size(m->baras[i],AN_BARRA,AL_MIN);
        lv_obj_set_pos(m->baras[i], X_BARRA+i*PASO_BARRA, Y_BASE-AL_MIN);
        lv_obj_set_style_bg_color(m->baras[i],lv_color_hex(C_VERDE),0);
        lv_obj_set_style_bg_grad_dir(m->baras[i],LV_GRAD_DIR_NONE,0);
        lv_obj_set_style_border_width(m->baras[i],0,0); 
        lv_obj_set_style_radius(m->baras[i],2,0);
        lv_obj_clear_flag(m->baras[i],LV_OBJ_FLAG_CLICKABLE);
    }
    m->hint = lv_label_create(ui_root(m->g));
    lv_label_set_text(m->hint,"");
    lv_obj_set_style_text_color(m->hint,lv_color_hex(C_TEXTO),0);
    lv_obj_align(m->hint, LV_ALIGN_TOP_LEFT, 10, 458);
    lvgl_unlock();

    ui_button(m->g, ID_VOL_DN, "VOL-", 20, 130, 90, 36, C_BTN);
    ui_button(m->g, ID_VOL_UP, "VOL+", 210, 130, 90, 36, C_BTN);
    ui_button(m->g, ID_PREV, "|<", 20, 330, 84, 64, C_BTN);
    ui_button(m->g, ID_PLAY, m->sonando ? "||" : ">", 118, 330, 84, 64, C_ACT);
    ui_button(m->g, ID_NEXT, ">|", 216, 330, 84, 64, C_BTN);
    /* Sin boton Back: el gesto de abajo hacia arriba vuelve a la lista. */
    ui_button(m->g, ID_SALIR, "X", 276, 6, 36, 32, C_ROJO);
    info_pintar(m);
}

static void pedir(struct musica *m, int cmd, int track)
{
    m->cmd_track = track;
    m->cmd = cmd;
}

static void elegir(struct musica *m, int i)
{
    if (i < 0 || i >= m->total) return;
    pantalla_repro(m);
    pedir(m, CMD_PLAY, i);
}

static void saltar(struct musica *m, int dest)
{
    if (dest < 0) dest = 0; if (dest >= m->total) return;
    pedir(m, CMD_PLAY, dest);
}

static void ir_fondo(struct musica *m, int *fuera)
{
    ui_event_t ev;
    int s;

    if (m->slot < 0)
    {
        return;
    }
    ui_show(m->g, 0);
    appmgr_set_state(m->slot, APP_BACKGROUND);
    while (!*fuera)
    {
        s = appmgr_get_state(m->slot);
        if (APP_RUNNING == s)
        {
            ui_show(m->g, 1);
            return;
        }
        if (APP_CLOSING == s || APP_FREE == s)
        {
            *fuera = 1;
            return;
        }
        while (ui_poll(m->g, &ev)) { ; }
        sleep(60);
    }
}

static void procesar(struct musica *m, ui_event_t *ev, int *fuera)
{
    if (UI_EVENT_SWIPE == ev->tipo) {
        if (UI_DIR_UP == ev->dir) {
            /* Arriba en la lista de pistas devuelve al launcher, pero solo si
             * el gesto arranca en el borde inferior; en la pantalla de
             * reproduccion, arriba vuelve a la lista. */
            if (P_REPRO == m->pantalla) pantalla_lista(m);
            else if (UI_EDGE_BOTTOM == ev->edge) ir_fondo(m, fuera);
        }
        return;
    }
    if (UI_EVENT_BUTTON != ev->tipo) return;
    if (ID_SALIR == ev->id) { *fuera = 1; return; }
    if (ID_VOLVER == ev->id) { pantalla_lista(m); return; }
    if (P_LISTA == m->pantalla) {
        if (ev->id >= ID_TEMA && ev->id < ID_TEMA + m->total) elegir(m, ev->id - ID_TEMA);
        return;
    }
    switch (ev->id) {
    case ID_PREV: saltar(m, m->tema-1); break;
    case ID_PLAY: pedir(m, m->sonando ? CMD_PAUSE : CMD_RESUME, m->tema); break;
    case ID_NEXT: saltar(m, m->tema+1); break;
    case ID_VOL_DN:
    case ID_VOL_UP:
        if (ID_VOL_DN == ev->id) m->vol -= 10; else m->vol += 10;
        if (m->vol < 0) m->vol = 0;
        if (m->vol > 100) m->vol = 100;
        if (m->audio >= 0) control(m->audio, AUDIO_CTRL_VOLUME, m->vol, 0);
        if (m->vol_lbl) {
            char vt[16];
            snprintf(vt, sizeof(vt), "VOL %d", m->vol);
            lvgl_lock();
            lv_label_set_text(m->vol_lbl, vt);
            lvgl_unlock();
        }
        break;
    default: break;
    }
}

static void parar_repro(struct musica *m)
{
    if (!m->repro_alive) return;
    m->quit = 1;
    /* espera cooperativa a que la tarea de audio termine */
    while (m->repro_alive)
    {
        sleep(20);
    }
    /* consume el mensaje de fin si llego */
    recvclr();
}

static void salir_app(struct musica *m)
{
    if (m->slot >= 0)
    {
        appmgr_unregister(m->slot);
        m->slot = SYSERR;
    }
    parar_repro(m);
    if (m->pcm) psram_free(m->pcm);
    if (m->mix) psram_free(m->mix);
    if (m->g) ui_destroy(m->g);
}

int main(int argc, char *argv[])
{
    struct musica *m;
    ui_event_t ev; 
    int fuera = 0;

    m = (struct musica *)psram_malloc(sizeof(struct musica));
    if (!m) exit(1);
    memset(m,0,sizeof(*m));
    m->tema=-1; 
    m->audio=SYSERR; 
    m->slot=SYSERR; 
    m->repro_tid=SYSERR;
    m->vol = 70;
    m->main_tid = gettid();
    m->g = ui_create_ex(UI_CHROME_NONE);
    if (!m->g) { 
        psram_free(m); 
        exit(1); 
    }
    m->slot = appmgr_register("Musica", "/apps/musica/app.elf", 0x9BD14F, gettid());
    m->pcm = (short *)psram_malloc(TRAMO * 2 * (int)sizeof(short));
    m->mix = (short *)psram_malloc(TRAMO * (int)sizeof(short));
    if (!m->pcm || !m->mix) { 
        salir_app(m); 
        psram_free(m); 
        exit(1); 
    }
    m->audio = getdev("/dev/audio0");
    if (m->audio >= 0) control(m->audio, AUDIO_CTRL_VOLUME, 80, 0);
    listado(m);
    pantalla_lista(m);
    {
        /* Misma prioridad que la UI: prioridad menor la hacia morir de hambre
         * al cargar otra app (el audio se cortaba). audioWrite marca el ritmo
         * cuando el anillo se llena, asi no acapara CPU. */
        m->repro_tid = create((void *)repro_task, 20480, getprio(gettid()), "musrepro", 1, (long)m);
    }
    if (SYSERR != m->repro_tid) ready(m->repro_tid, RESCHED_NO);

    while (!fuera) {
        while (ui_poll(m->g, &ev)) { 
            procesar(m,&ev,&fuera);
            if (fuera) break; 
        }
        if (!fuera && m->slot >= 0 && APP_CLOSING == appmgr_get_state(m->slot)) fuera = 1;
        if (fuera) break;
        sleep(15);
    }
    salir_app(m); 
    psram_free(m); 
    exit(0); 
    return 0;
}
