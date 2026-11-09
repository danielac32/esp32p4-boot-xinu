/**
 * @file musica.c
 *
 * Reproductor de MP3.  Lista /music, reproduce con previo/play-pausa/siguiente
 * y espectro.  Gesto abajo->arriba en el reproductor vuelve a la lista (la
 * musica sigue sonando).  Sin coma flotante, estado en PSRAM.
 */
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <timer.h>
#include <memory.h>
#include <device.h>
#include <audio.h>
#include <mp3.h>
#define FAT_INLINE inline
#include <fat_filelib.h>
#include <lvgl.h>
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
    gestos_t *g;
    int pantalla;
    char nombres[MAX_TEMAS][LARGO_NOM];
    int total;
    int tema;
    int sonando;
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
};

static int acaba_mp3(const char *n)
{
    int l = (int)strlen(n);
    const char *x;
    if (l < 5) return 0;
    x = n + l - 4;
    return '.' == x[0] && ('m' == x[1] || 'M' == x[1]) &&
           ('p' == x[2] || 'P' == x[2]) && '3' == x[3];
}

static void nombre_corto(char *dst, const char *src, int max)
{
    int l = (int)strlen(src);
    if (acaba_mp3(src)) l -= 4;
    if (l > max) { memcpy(dst, src, max - 2); dst[max-2]='.'; dst[max-1]='.'; dst[max]='\0'; }
    else { memcpy(dst, src, l); dst[l] = '\0'; }
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
    if (P_REPRO != m->pantalla) return;
    lvgl_lock();
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

static void sonar(struct musica *m)
{
    if (m->tema < 0 || m->audio < 0 || m->sonando) return;
    if (SYSERR == control(m->audio, AUDIO_CTRL_STREAM, 1, 0)) return;
    m->sonando = 1;
    m->tic_espectro = millis(); m->tic_reloj = m->tic_espectro;
    gestos_boton_texto(m->g, ID_PLAY, "||");
}

static void pausar(struct musica *m)
{
    if (!m->sonando) return;
    control(m->audio, AUDIO_CTRL_STREAM_WAIT, 0, 0);
    control(m->audio, AUDIO_CTRL_STREAM, 0, 0);
    m->sonando = 0; espectro_cero(m);
    gestos_boton_texto(m->g, ID_PLAY, ">");
}

static void callar(struct musica *m)
{
    if (m->sonando) { control(m->audio, AUDIO_CTRL_STREAM, 0, 0); m->sonando = 0; }
    { int i; for (i=0;i<BANDAS;i++){ m->altura[i]=AL_MIN; m->pico[i]=0; } }
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
static void saltar(struct musica *m, int dest);

static void avanzar(struct musica *m)
{
    unsigned ahora = millis();
    int n = mp3_leer(m->pcm, TRAMO);
    if (n <= 0) {
        callar(m); gestos_boton_texto(m->g, ID_PLAY, ">");
        if (m->tema >= 0 && m->tema + 1 < m->total) saltar(m, m->tema + 1);
        return;
    }
    if (!entregar(m, n * 4)) { pausar(m); return; }
    if (ahora - m->tic_espectro >= 42) { m->tic_espectro = ahora; espectro_calc(m,n); espectro_pintar(m); }
    if (ahora - m->tic_reloj >= 250) { m->tic_reloj = ahora; info_pintar(m); }
}

static void set_txt(lv_obj_t *o, const char *t)
{
    if (!o) return; lvgl_lock(); lv_label_set_text(o,t); lvgl_unlock();
}

static void limpiar_repro(struct musica *m)
{
    int i;
    lvgl_lock();
    if (m->titulo) { lv_obj_del(m->titulo); m->titulo=NULL; }
    if (m->tiempo) { lv_obj_del(m->tiempo); m->tiempo=NULL; }
    if (m->info)   { lv_obj_del(m->info);   m->info=NULL; }
    if (m->hint)   { lv_obj_del(m->hint);   m->hint=NULL; }
    if (m->panel)  { lv_obj_del(m->panel);  m->panel=NULL; }
    for (i=0;i<BANDAS;i++) m->baras[i]=NULL;
    lvgl_unlock();
}

static void info_pintar(struct musica *m)
{
    char t[64], nom[36];
    if (P_REPRO != m->pantalla) return;
    if (m->tema >= 0) {
        unsigned ya=mp3_ms_actual(), todo=mp3_ms_total();
        nombre_corto(nom, m->nombres[m->tema], 28);
        set_txt(m->titulo, nom);
        snprintf(t,sizeof(t), "%u:%02u / %u:%02u  %dk",
                 ya/60000U,(ya/1000U)%60U,todo/60000U,(todo/1000U)%60U,mp3_kbps());
        set_txt(m->tiempo, t);
        snprintf(t,sizeof(t), "track %d/%d  %dHz", m->tema+1, m->total, mp3_hz());
        set_txt(m->info, t);
    }
}

static void pantalla_lista(struct musica *m)
{
    char rot[48]; int i;
    m->pantalla = P_LISTA;
    gestos_botones_limpiar(m->g);
    limpiar_repro(m);
    m->ltit = NULL;

    lvgl_lock();
    m->ltit = lv_label_create(gestos_raiz(m->g));
    snprintf(rot,sizeof(rot),"MUSIC  %d tracks", m->total);
    lv_label_set_text(m->ltit, m->total ? rot : "MUSIC");
    lv_obj_set_style_text_color(m->ltit, lv_color_hex(C_TEXTO), 0);
    lv_obj_align(m->ltit, LV_ALIGN_TOP_LEFT, 10, 8);
    lvgl_unlock();

    gestos_boton(m->g, ID_SALIR, "X", 276, 6, 36, 32, C_ROJO);
    for (i=0; i<m->total && i<MAX_TEMAS; i++) {
        char num[40]; unsigned c = (i==m->tema)?C_ACT:C_FILA;
        nombre_corto(rot, m->nombres[i], 24);
        snprintf(num,sizeof(num), "%02d  %s", i+1, rot);
        gestos_boton(m->g, ID_TEMA+i, num, 10, 44 + i*42, 300, 38, c);
    }
}

static void pantalla_repro(struct musica *m)
{
    int i;
    m->pantalla = P_REPRO;
    gestos_botones_limpiar(m->g);
    if (m->ltit) { lvgl_lock(); lv_obj_del(m->ltit); m->ltit=NULL; lvgl_unlock(); }
    limpiar_repro(m);

    lvgl_lock();
    m->titulo = lv_label_create(gestos_raiz(m->g));
    lv_label_set_text(m->titulo,"-"); lv_obj_set_style_text_color(m->titulo,lv_color_hex(C_TEXTO),0);
    lv_obj_set_width(m->titulo,260); lv_obj_align(m->titulo, LV_ALIGN_TOP_LEFT, 10, 10);
    m->tiempo = lv_label_create(gestos_raiz(m->g));
    lv_label_set_text(m->tiempo,""); lv_obj_set_style_text_color(m->tiempo,lv_color_hex(C_TEXTO),0);
    lv_obj_align(m->tiempo, LV_ALIGN_TOP_LEFT, 10, 34);
    m->info = lv_label_create(gestos_raiz(m->g));
    lv_label_set_text(m->info,""); lv_obj_set_style_text_color(m->info,lv_color_hex(C_TEXTO),0);
    lv_obj_align(m->info, LV_ALIGN_TOP_LEFT, 10, 56);
    m->panel = lv_obj_create(gestos_raiz(m->g));
    lv_obj_set_size(m->panel,304,130); lv_obj_align(m->panel, LV_ALIGN_TOP_LEFT, 8,170);
    lv_obj_set_style_bg_color(m->panel,lv_color_hex(C_PANEL),0);
    lv_obj_set_style_bg_grad_dir(m->panel,LV_GRAD_DIR_NONE,0);
    lv_obj_set_style_border_width(m->panel,0,0); lv_obj_set_style_radius(m->panel,8,0);
    lv_obj_clear_flag(m->panel, LV_OBJ_FLAG_SCROLLABLE); lv_obj_clear_flag(m->panel, LV_OBJ_FLAG_CLICKABLE);
    for (i=0;i<BANDAS;i++) {
        m->baras[i]=lv_obj_create(gestos_raiz(m->g));
        lv_obj_set_size(m->baras[i],AN_BARRA,AL_MIN);
        lv_obj_set_pos(m->baras[i], X_BARRA+i*PASO_BARRA, Y_BASE-AL_MIN);
        lv_obj_set_style_bg_color(m->baras[i],lv_color_hex(C_VERDE),0);
        lv_obj_set_style_bg_grad_dir(m->baras[i],LV_GRAD_DIR_NONE,0);
        lv_obj_set_style_border_width(m->baras[i],0,0); lv_obj_set_style_radius(m->baras[i],2,0);
        lv_obj_clear_flag(m->baras[i],LV_OBJ_FLAG_CLICKABLE);
    }
    m->hint = lv_label_create(gestos_raiz(m->g));
    lv_label_set_text(m->hint,"swipe up = list");
    lv_obj_set_style_text_color(m->hint,lv_color_hex(C_TEXTO),0);
    lv_obj_align(m->hint, LV_ALIGN_TOP_LEFT, 10, 458);
    lvgl_unlock();

    gestos_boton(m->g, ID_PREV, "|<", 20, 330, 84, 64, C_BTN);
    gestos_boton(m->g, ID_PLAY, m->sonando ? "||" : ">", 118, 330, 84, 64, C_ACT);
    gestos_boton(m->g, ID_NEXT, ">|", 216, 330, 84, 64, C_BTN);
    gestos_boton(m->g, ID_VOLVER, "Back", 20, 410, 140, 44, C_BTN);
    gestos_boton(m->g, ID_SALIR, "X", 276, 6, 36, 32, C_ROJO);
    info_pintar(m);
}

static void saltar(struct musica *m, int dest)
{
    if (dest < 0) dest = 0; if (dest >= m->total) return;
    callar(m); gestos_boton_texto(m->g, ID_PLAY, ">");
    if (cargar(m,dest)) { if (P_REPRO==m->pantalla) info_pintar(m); sonar(m); }
}

static void elegir(struct musica *m, int i)
{
    callar(m);
    if (!cargar(m,i)) return;
    pantalla_repro(m);
    sonar(m);
}

static void procesar(struct musica *m, gev_t *ev, int *fuera)
{
    if (GE_ARRASTRE == ev->tipo) {
        if (P_REPRO == m->pantalla && GE_ARRIBA == ev->dir) pantalla_lista(m);
        return;
    }
    if (GE_BOTON != ev->tipo) return;
    if (ID_SALIR == ev->id) { *fuera = 1; return; }
    if (ID_VOLVER == ev->id) { pantalla_lista(m); return; }
    if (P_LISTA == m->pantalla) {
        if (ev->id >= ID_TEMA && ev->id < ID_TEMA + m->total) elegir(m, ev->id - ID_TEMA);
        return;
    }
    switch (ev->id) {
    case ID_PREV: saltar(m, m->tema-1); break;
    case ID_PLAY: if (m->sonando) pausar(m); else sonar(m); break;
    case ID_NEXT: saltar(m, m->tema+1); break;
    default: break;
    }
}

static void salir_app(struct musica *m)
{
    callar(m); mp3_cerrar();
    if (m->pcm) psram_free(m->pcm);
    if (m->mix) psram_free(m->mix);
    if (m->g) gestos_cerrar(m->g);
}

int main(int argc, char *argv[])
{
    struct musica *m; gev_t ev; int fuera = 0;
    (void)argc; (void)argv;
    m = (struct musica *)psram_malloc(sizeof(struct musica));
    if (!m) exit(1);
    memset(m,0,sizeof(*m)); m->tema=-1; m->audio=SYSERR;
    m->g = gestos_nueva_partes(0);
    if (!m->g) { psram_free(m); exit(1); }
    m->pcm = (short *)psram_malloc(TRAMO * 2 * (int)sizeof(short));
    m->mix = (short *)psram_malloc(TRAMO * (int)sizeof(short));
    if (!m->pcm || !m->mix) { salir_app(m); psram_free(m); exit(1); }
    m->audio = getdev("/dev/audio0");
    if (m->audio >= 0) control(m->audio, AUDIO_CTRL_VOLUME, 80, 0);
    listado(m);
    pantalla_lista(m);
    while (!fuera) {
        while (gestos_leer(m->g, &ev)) { procesar(m,&ev,&fuera); if (fuera) break; }
        if (m->sonando) avanzar(m); else sleep(30);
    }
    salir_app(m); psram_free(m); exit(0); return 0;
}
