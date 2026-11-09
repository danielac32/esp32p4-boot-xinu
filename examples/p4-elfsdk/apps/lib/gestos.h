/**
 * @file gestos.h
 *
 * Libreria de gestos para las apps ELF del P4.  Es codigo DE APP, no del
 * kernel: se compila con las mismas banderas que la app y se enlaza con 'ld -r'
 * junto a ella, de modo que solo puede llamar a lo que system/exports.c
 * publica y NO puede tener estaticas escribibles de fichero (el compilador las
 * accede con una ancla local y una relocacion R_RISCV_LO12_S, y el cargador no
 * la aplica).  Todo el estado vive en un bloque de PSRAM colgado del handle.
 *
 * Que hace por ti:
 *   - un fondo a pantalla completa que mide el VECTOR real del dedo (donde se
 *     apoyo hasta donde se levanto) y lo convierte en direccion;
 *   - un anillo de eventos: la app pregunta con gestos_leer() desde su bucle;
 *   - botones planos con id;
 *   - y, SOLO SI SE PIDEN en gestos_nueva_partes(), tres adornos visuales:
 *     la flecha del centro (dos lv_line, sin fuentes: la por defecto de este
 *     arbol es lv_font_unscii_16 y no trae los glifos LV_SYMBOL_*), que gira en
 *     vivo con el dedo; la pastilla de arriba con la direccion; y una linea de
 *     nota.  Una app que pinta su propia cabecera (el launcher) no los pide y
 *     no carga con objetos que nunca va a ver.
 *
 * Modelo de hebras (esto es lo que no se puede cambiar): la UI la bombea la
 * hebra residente 'lvglui' del kernel, asi que la app NO llama a
 * lv_timer_handler().  Los callbacks de la libreria corren en lvglui con el
 * candado ya cogido y lo unico que hacen es apilar el evento; la app lo saca
 * desde su propia hebra, que es la unica donde su printf va a la sesion que
 * lanzo el 'elf'.
 *
 * Dos reglas para no morir:
 *   1. Las funciones de esta cabecera toman lvgl_lock(), que NO es recursivo
 *      (es lock() de un semaforo en device/lvgl/lvglport.c): no llames a
 *      ninguna desde dentro de un callback LVGL.
 *   2. exit() de una app es elf_exit() = kill(gettid()), o sea mata la hebra
 *      que lo llama: llamalo desde el bucle de main, nunca desde un callback,
 *      o lo que matas es la UI residente y su PSRAM queda sin liberar.
 *
 * Uso minimo (asi queda cualquier app):
 *
 *   gestos_t *g = gestos_nueva();
 *   if (NULL == g) return 1;
 *   gestos_boton(g, 1, "SUMAR", 20, 400, 130, 55, 0x2ECC71);
 *   for (;;) {
 *       gev_t ev;
 *       if (!gestos_leer(g, &ev)) { sleep(50); continue; }
 *       if (ev.tipo == GE_ARRASTRE) printf("%s\r\n", gestos_dir_texto(ev.dir));
 *   }
 */
#ifndef APPLIB_GESTOS_H
#define APPLIB_GESTOS_H

#include "lvgl/lvgl.h"

/** El estado de la libreria, en PSRAM; opaco para la app. */
typedef struct gestos gestos_t;

/* --- lo que devuelve gestos_leer() ----------------------------------------- */
#define GE_NADA      0        /* anillo vacio                                  */
#define GE_ARRASTRE  1        /* el dedo recorrio >= GE_LIMO px: hay direccion */
#define GE_TOQUE     2        /* pulsacion corta sin recorrido                 */
#define GE_LARGA     3        /* dedo quieto el tiempo de LVGL                 */
#define GE_BOTON     4        /* CLICKED en un boton de gestos_boton()         */

/* --- direcciones: el signo del eje dominante del vector -------------------- */
#define GE_ARRIBA 0
#define GE_ABAJO  1
#define GE_IZQ    2
#define GE_DER    3
#define GE_SINDIR 4

typedef struct
{
    int tipo;                 /* GE_* de arriba                                */
    int dir;                  /* GE_ARRIBA..GE_DER (valido en GE_ARRASTRE)     */
    int id;                   /* GE_BOTON: el id que paso a gestos_boton()     */
    int x1, y1, x2, y2;       /* vector del dedo; en GE_TOQUE solo vale x1,y1  */
    int dx, dy;               /* x2-x1 y y2-y1                                 */
} gev_t;

/* --- que adornos se montan ------------------------------------------------ */

/* El fondo y el anillo de eventos son la libreria y van SIEMPRE.  Estas
 * banderas eligen el mobiliario visual; cada una es independiente:
 *
 *   GE_PASTILLA  pastilla de arriba con el nombre de la direccion (panel+aviso)
 *   GE_FLECHA    flecha grande que gira en vivo con el dedo (asta+cabeza)
 *   GE_NOTA      linea de texto libre de la app (gestos_nota)
 *
 * Si una parte no se monto, sus funciones son inofensivas: gestos_nota(),
 * gestos_rotulo(), gestos_flecha(), gestos_pintar() y gestos_panel() lo miran
 * y no hacen nada, en vez de tocar un puntero nulo. */
#define GE_PASTILLA 0x01
#define GE_FLECHA   0x02
#define GE_NOTA     0x04
#define GE_ADORNOS  (GE_PASTILLA | GE_FLECHA | GE_NOTA)

/**
 * Abre la pantalla de gestos con TODO el mobiliario: es equivalente a
 * gestos_nueva_partes(GE_ADORNOS).  Es la que usan las apps que quieren el
 * aviso visual de direccion (musica) y las de ejemplo.
 *
 * Devuelve NULL y ya avisa por la consola si LVGL no esta inicializado o si no
 * queda PSRAM.
 */
gestos_t *gestos_nueva(void);

/**
 * Lo mismo, montando solo las partes pedidas.  Con 0 queda la pantalla de
 * gestos pura: fondo que mide el dedo, anillo de eventos y botones.  Es lo que
 * quiere una app con cabecera propia que no necesita pastilla ni flecha (el
 * launcher), y ahorra crear y pintar objetos que nunca se ven.
 */
gestos_t *gestos_nueva_partes(int partes);

/**
 * Saca el siguiente evento apilado.  1 = lo escribe en *ev; 0 = no hay nada.
 * No duerme ni toca LVGL: la app pone su sleep() en el bucle.
 */
int gestos_leer(gestos_t *g, gev_t *ev);

/**
 * Boton plano (sin el degradado gris del tema) con rotulo negro encima.  Al
 * pulsarlo llega un GE_BOTON con `id`.  Devuelve el lv_obj para quien lo quiera
 * seguir tuneando (el rotulo es su unico hijo: lv_obj_get_child(b, 0)).
 * Maximo GE_BOTONES=32 controles vivos entre todas las pantallas.
 *
 * El arrastre funciona tambien encima del boton: el boton se lleva los
 * PRESSED/PRESSING/RELEASED porque LVGL entrega los eventos al objeto que
 * recibe el toque, y la libreria monta ahi el mismo seguimiento del dedo.  Si
 * el recorrido supera GE_LIMO el gesto es GE_ARRASTRE y NO llega GE_BOTON, así
 * que pasar pagina con el dedo empezando en un icono no pulsa el icono.
 */
lv_obj_t *gestos_boton(gestos_t *g, int id, const char *txt, int x, int y,
                       int w, int h, unsigned int rgb);

/**
 * Lo mismo, pero colgando del contenedor que diga `padre` en vez del fondo.
 * Es lo que permite una lista con scroll: las filas son botones dentro de un
 * objeto alto que se mueve con lv_obj_set_y(), y la ventana que lo recorta es
 * otro objeto fijo.  El arrastre y el GE_BOTON se comportan igual que arriba
 * porque los callbacks van pegados al boton, no al padre.
 *
 * Un boton registrado aqui es hijo de `padre`: NO borres `padre` con
 * lv_obj_del() teniendo filas vivas, o `gestos_botones_limpiar()` volveria a
 * borrar objetos que ya no estan.  Se limpia primero con esa funcion y despues
 * se esconde o se borra el contenedor.
 */
lv_obj_t *gestos_boton_en(gestos_t *g, int id, const char *txt, lv_obj_t *padre,
                          int x, int y, int w, int h, unsigned int rgb);

/** Reetiqueta el boton con ese id.  Con esto un launcher de paginas crea los
 *  botones una sola vez y solo cambia los rotulos al pasar de pagina. */
void gestos_boton_texto(gestos_t *g, int id, const char *txt);

/**
 * Borra TODOS los botones y devuelve el cupo de GE_BOTONES; la flecha, la
 * pastilla y la nota siguen donde estaban y el arrastre se sigue midiendo igual.
 * Hace falta cuando la app tiene mas pantallas que controles caben en una (una
 * lista de canciones y despues los tres botones del reproductor): se llama al
 * cambiar de pantalla y se crean los nuevos.  No hay que llamarlo antes de
 * gestos_cerrar(), que se lleva el fondo con los hijos.
 */
void gestos_botones_limpiar(gestos_t *g);

/** El contenedor de todo: ahi cuelga la app sus objetos (encima de la flecha). */
lv_obj_t *gestos_raiz(gestos_t *g);

/** Muestra u oculta toda la UI (la raiz).  Sirve para mandar la app a segundo
 *  plano sin destruirla: oculta deja de dibujar y de recibir toques. */
void gestos_show(gestos_t *g, int on);

/** Rotulo grande de la pastilla y linea de nota; "" esconde la nota.  Si la
 *  parte no se monto (GE_PASTILLA / GE_NOTA), la llamada no hace nada. */
void gestos_rotulo(gestos_t *g, const char *txt);
void gestos_nota(gestos_t *g, const char *txt);

/** on=0 esconde la flecha (un launcher no la quiere); el arrastre se sigue
 *  apilando igual.  gestos_pintar() obliga a la flecha a mirar a `dir`.  Sin
 *  GE_FLECHA montada son no-ops. */
void gestos_flecha(gestos_t *g, int on);
void gestos_pintar(gestos_t *g, int dir);

/** on=0 esconde la pastilla de arriba.  La quiere fuera la app que dibuja su
 *  propia cabecera (el launcher pone ahi la banda de estado de estado.h, y la
 *  pastilla por defecto cae exactamente encima); el arrastre se sigue midiendo
 *  y apilando igual, solo que no se pinta en vivo.  La direccion sigue llegando
 *  en ev.dir y se puede rotular con gestos_dir_texto()/gestos_dir_color().
 *  Sin GE_PASTILLA montada es no-op. */
void gestos_panel(gestos_t *g, int on);

/** "de abajo HACIA ARRIBA" y compania, y el color fijo de cada direccion. */
const char *gestos_dir_texto(int dir);
unsigned int gestos_dir_color(int dir);

/**
 * Borra los objetos y suelta la PSRAM del handle.  Hay que llamarlo ANTES de
 * exit(): kill() suelta la PSRAM del app, pero los lv_line de la flecha
 * guardan punteros a los puntos que viven dentro de este handle.
 */
void gestos_cerrar(gestos_t *g);

#endif /* APPLIB_GESTOS_H */
