/**
 * @file estado.h
 *
 * Banda de estado para las apps ELF del P4: IP, WiFi y bateria.  Es codigo DE
 * APP, como gestos.[ch]: se enlaza con 'ld -r' junto al main, solo llama a lo
 * que publica system/exports.c y no tiene ni una estatica escribible de fichero
 * (el cargador no aplica R_RISCV_LO12_S).  Todo el estado cuelga de un handle
 * reservado en PSRAM.
 *
 * Que dibuja, con la estetica vintage del arbol (baquelita, laton y papel):
 *
 *   +--------------------------------------+
 *   | _-=-#  192.168.1.100                 |   enlace + IP
 *   | [###-] 100%                EN LINEA  |   bateria + estado
 *   +--------------------------------------+
 *
 * Los dos iconos son GEOMETRIA, no fuente: la por defecto de este arbol es
 * lv_font_unscii_16 (0x20..0x7F) y la tabla de exports publica funciones, no
 * los datos de una fuente, asi que ni LV_SYMBOL_* ni montserrat estan al
 * alcance de una app.  Las rallitas del WiFi son cuatro lv_obj y la bateria es
 * un cuerpo con borde, un pico y un relleno cuyo ancho es el porcentaje.
 *
 * De donde salen los datos (todo por la tabla de exports):
 *   - bateria:  control(getdev("/dev/power0"), POWER_CTRL_PERCENT) -> 0..100.
 *   - enlace:   espethLinkIsUp(), que es SDIO adjunto + STA asociado.
 *   - IP:       netLookup(getdev("ETH0")) y netaddrsprintf sobre su campo ip.
 *   - senal:    wifiSignalRead() (RPC 294 al C6) y wifiSignalQuality().
 *
 * Dos cosas que hay que saber antes de llamarla:
 *
 *   1. estado_refrescar() se gatea SOLO con millis(): la bateria, el enlace y
 *      la IP se releen cada ES_DATO_MS, y el RSSI cada ES_RSSI_MS.  Se puede
 *      llamar en cada vuelta del bucle sin coste; lo que no se puede es llamar
 *      al RPC en cada vuelta, porque bloquea hasta un segundo esperando al C6
 *      y durante ese segundo los gestos se apilan en el anillo de gestos.c.
 *      Por eso el periodo del RSSI es diez veces el de los datos baratos.
 *
 *   2. Las funciones de aqui toman lvgl_lock(), que NO es recursivo.  No las
 *      llames con el candado ya cogido ni desde un callback de LVGL.
 *
 * Uso (el launcher lo hace asi):
 *
 *   estado_t *e = estado_nueva(gestos_raiz(g), 8, 8, 304);
 *   for (;;) {
 *       estado_refrescar(e);
 *       if (!gestos_leer(g, &ev)) { sleep(50); continue; }
 *       ...
 *   }
 *   estado_cerrar(e);      // ANTES de gestos_cerrar(): la caja es hija del fondo
 */
#ifndef APPLIB_ESTADO_H
#define APPLIB_ESTADO_H

#include "lvgl/lvgl.h"

/** El estado de la banda, en PSRAM; opaco para la app. */
typedef struct estado estado_t;

/* Paleta vintage: la misma que apps/musica, para que el sistema entero tenga
 * la misma cara.  Son RGB888; el panel los convierte a RGB565 al pintar. */
#define ES_CAJA    0x2A1E10   /* la baquelita de la caja                   */
#define ES_LATON   0xB08D57   /* el borde de la caja                       */
#define ES_PAPEL   0xE8D8B0   /* el texto y el contorno de la bateria      */
#define ES_VERDE   0x9BD14F   /* enlace arriba, bateria sana               */
#define ES_AMBAR   0xFFB302   /* bateria entre 21 y 40                     */
#define ES_ROJO    0xC0392B   /* sin enlace, o bateria por debajo de 21    */
#define ES_APAGADO 0x4A3B28   /* las rallitas que no tocan                 */

/** Alto de la banda: dos lineas de 17 px mas el aire de la caja. */
#define ES_ALTO 62

/* Periodos de relectura.  El del RSSI es largo a proposito: es el unico que
 * hace un RPC y puede bloquear hasta 1000 ms (wifiSignalRead espera ese plazo).
 * Los otros tres datos son baratos: un control() al PMIC por I2C, un flag del
 * driver espeth y un recorrido de la tabla netiftab. */
#define ES_DATO_MS  1000U
#define ES_RSSI_MS  10000U

/**
 * Crea la banda colgada de `padre`, con su esquina superior izquierda en (x,y)
 * y `w` de ancho (el alto es ES_ALTO).  Lee las tres fuentes y la pinta antes
 * de devolver, asi que la pantalla ya sale con datos y no con un hueco.
 * Devuelve NULL si no queda PSRAM.  Con w=304 la IP mas larga (15 caracteres a
 * 16 px) entra justita; por debajo de 280 se recorta con puntos suspensivos.
 */
estado_t *estado_nueva(lv_obj_t *padre, int x, int y, int w);

/**
 * Relee lo que toque por reloj y repinta SOLO si algo cambio.  No duerme: el
 * sleep del bucle lo pone la app.  El RSSI solo se pide con el enlace arriba.
 */
void estado_refrescar(estado_t *e);

/**
 * Borra la caja y suelta la PSRAM del handle.  Hay que llamarlo ANTES de
 * gestos_cerrar(), porque la caja es hija del fondo que aquel borra.
 */
void estado_cerrar(estado_t *e);

#endif /* APPLIB_ESTADO_H */