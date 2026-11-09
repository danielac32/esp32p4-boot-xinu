/**
 * @file lvglport.c
 *
 * Puerto de LVGL v8 (la libreria vendorizada en lvgl/) al Xinu del ESP32-P4.
 *
 * El device no dibuja nada: presta cuatro cosas y las serializa.
 *
 *   - la pantalla: el flush de LVGL sale por control(LCD0, LCD_CTRL_BLIT),
 *     que es quien toma el monitor del bus SPI2 y escribe en el ST7796;
 *   - el dedo: la lectura del FT6336 va por control(TOUCH0, TOUCH_CTRL_READ),
 *     que toma el monitor del bus I2C0 compartido;
 *   - el reloj: LVGL no tiene reloj propio (LV_TICK_CUSTOM 0), y lo avanza la
 *     hebra residente `lvglui` (lvglTask abajo) desde millis() de Xinu;
 *   - el refresco: la misma hebra llama a lv_timer_handler() cada 5 ms, asi
 *     que una app java no tiene que hacer nada para que la pantalla se
 *     actualice --antes lo hacia cada app con LVGL.tick(), y por eso la UI se
 *     congelaba mientras la app trabajaba.
 *
 * El por que del monitor propio: LVGL no es reentrante.  Su estado global --el
 * arbol de objetos, el pool de lv_mem, las colas de refresco-- supone un unico
 * hilo llamando a la API.  Aqui pueden llamar a la vez varias hebras java (el
 * workspace y sus apps en segundo plano), asi que toda la API se envuelve con
 * lvgl_lock()/lvgl_unlock(); quien necesite hacer una rafaga de llamadas y ver
 * el resultado junto las envuelve el, no cada control() por separado.
 *
 * La memoria: los dos buffers de dibujo y el heap interno de LVGL salen de
 * PSRAM (lv_conf.h y lvglInit abajo).  El .bss de este puerto enlaza en L2MEM,
 * que es tambien donde `create' saca las pilas de las hebras; dejar ahi 51 KB
 * de buffers habria costado una hebra java de presupuesto.
 *
 * Requiere gpio, i2c, lcd y touch ya inicializados (orden del devtab en
 * system/configurations/xinu.conf: ... LCD0, TOUCH0, LVGL0).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <kernel.h>
#include <device.h>
#include <monitor.h>
#include <memory.h>
#include <timer.h>
#include <string.h>
#include <lcd.h>
#include <touch.h>
#include <lvgl.h>
#include "fatfs/fat_filelib.h"      /* fl_fopen/fl_fwrite del BMP */

#include "lvgl/lvgl.h"

/* Franjas horizontales de 320 px: con dos buffers LVGL puede renderizar la
 * siguiente mientras el SPI2 vacia la anterior.  40 filas x 320 px x 2 B =
 * 25.600 B por buffer; 51.200 B en total, de PSRAM.  Bajar de aqui no ahorra
 * flush (la pantalla sigue necesitando sus 12 franjas) y subir solo engorda
 * el pico de PSRAM. */
#define DRAW_LINES 40

/* Periodo del bucle de la hebra de UI, en ms del kernel.  No es el periodo de
 * refresco de LVGL (LV_DISP_DEF_REFR_PERIOD, 30 ms) sino cada cuanto se lo
 * pregunta: pasar mas seguido solo cuesta un lv_timer_handler() que devuelve
 * sin pintar.  Con 5 ms el puntero se muestrea a 30 ms de reloj LVGL, y en un
 * gesto de 300 ms eso son ~8 muestras --lo justo para que la galeria y el
 * launcher distingan un arrastre de un toque--. */
#define LVGL_TASK_MS   5

/* 8 KB para la hebra de UI: dentro de lv_timer_handler() cae el refresco
 * completo --la decodificacion sjpg (tjpgd) y el blit por SPI2--.  El pico
 * medido de la hebra java que hacia eso antes (LVGL.tick(), con la VM encima)
 * fue de 5.956 B, asi que aqui hay margen sin pedir los 16 KB de una tarea
 * JVM.  La pila sale del heap de L2MEM, el mismo de las hebras java. */
#define LVGL_TASK_STK  8192

static monitor lvglmon = SYSERR;
static int lvglReady = 0;

/* ---- tiempo de pantalla -----------------------------------------------
 *
 * El apagado por inactividad vive aqui, en la hebra que ve TODOS los toques:
 * lvgl_touch_cb() lo llama lv_timer_handler() sea cual sea la app que este en
 * frente.  En workspace no se puede hacer: el launcher solo recibe los eventos
 * de los widgets que el creo, asi que con una app al frente no veria ni el
 * dedo que hay que contar ni el que despierta la pantalla.
 *
 * La politica (cuantos ms) si la manda la app residente con
 * control(LVGL0, LVGL_CTRL_IDLE, ms, 0) -> LVGL.idleOff(ms) de java; 0 deja la
 * pantalla encendida para siempre, que es como arranca el kernel para que la
 * shell y las pruebas no se queden a oscuras.
 *
 * El backlight es un GPIO (LCD_BL en include/board.h, 28) apagado por el
 * device lcd; no hay brillo: 0 o 1. */

static uint32_t idleOffMs;        /* 0 = no apagar nunca */
static uint32_t lastTouchMs;      /* millis() del ultimo dedo visto */
static int screenOff;             /* 1 con el backlight apagado */
static int wakeSwallow;           /* el dedo que encendio no es de nadie */

/* Apagar: solo el backlight y el SPI; la hebra sigue bombeando LVGL para que
 * la UI no se congele y el tacto se pueda seguir leyendo. */
static void screenBlank(void)
{
    control(LCD0, LCD_CTRL_BACKLIGHT, 0, 0);
    screenOff = 1;
    kprintf("[lvgl] pantalla apagada: %u ms sin toques\r\n", idleOffMs);
}

/* Encender.  El primer dedo se marca para tragarselo (ver lvgl_touch_cb) y el
 * screen se re-invalida entero: mientras estuve apagado lvgl_flush_cb tiro los
 * buffers sin mandarlos, asi que el panel tiene lo de hace dos minutos. */
static void screenWake(void)
{
    control(LCD0, LCD_CTRL_BACKLIGHT, 1, 0);
    screenOff = 0;
    wakeSwallow = 1;
    lastTouchMs = millis();
    lv_obj_invalidate(lv_scr_act());
    kprintf("[lvgl] pantalla despierta: dedo tragado\r\n");
}

static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;
static lv_color_t *buf1;
static lv_color_t *buf2;

/* Espejo del fotograma completo: 320x480x2 B en PSRAM.  LVGL trabaja con dos
 * franjas de 320xDRAW_LINES (25.600 B), o sea que en ningun sitio de la placa
 * hay una imagen de lo que se esta viendo: por eso las apps "no abren" no se
 * pueden diagnosticar desde fuera.  Aqui se copia cada area invalidada en el
 * momento del flush y examples/p4shot.py la lee por telnet.  El coste es un
 * memcpy de 25.600 B por franja, baladi delante del SPI2.
 *
 * No es static a proposito: `nm compile/xinu.elf | grep lvgl_shadow` tiene que
 * dar la variable para que el lector sepa donde esta el lienzo. */
uint16_t *lvgl_shadow;

static uint32_t last_tick;

/* Acceso al monitor desde otros modulos (las natives java de esp32p4.LVGL). */
void lvgl_lock(void)   { if (lvglmon != SYSERR) lock(lvglmon); }
void lvgl_unlock(void) { if (lvglmon != SYSERR) unlock(lvglmon); }

/**
 * @ingroup lvgl
 *
 * Flush: LVGL deja el area renderizada en su buffer y aqui se manda tal cual
 * al panel por el device lcd, que es dueno del SPI2 (byte alto de cada pixel
 * primero, como pide el ST7796).  No hay copia intermedia: b.img apunta al
 * buffer de LVGL en PSRAM y el driver lo lee por el CPU.
 */
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                          lv_color_t *px)
{
    struct lcd_blit b;

    if (screenOff)
    {
        /* Nadie mira: se da el buffer por vaciado sin gastar el SPI2. */
        lv_disp_flush_ready(drv);
        return;
    }

    b.x = area->x1;
    b.y = area->y1;
    b.w = area->x2 - area->x1 + 1;
    b.h = area->y2 - area->y1 + 1;
    b.img = (const uint16_t *) px;

    /* Fila a fila: el buffer de LVGL es un rectangulo compacto de b.w pixeles
     * y el lienzo no, asi que no vale con un memcpy del area entera. */
    if (lvgl_shadow != 0) {
        uint32_t y;
        for (y = 0; y < (uint32_t) b.h; y++) {
            memcpy(&lvgl_shadow[((uint32_t) b.y + y) * LCD_W + b.x],
                   &px[y * (uint32_t) b.w],
                   (uint32_t) b.w * sizeof(lv_color_t));
        }
    }

    control(LCD0, LCD_CTRL_BLIT, (long) &b, 0);
    lv_disp_flush_ready(drv);
}

/**
 * @ingroup lvgl
 *
 * Dedo: el FT6336 se pregunta por sondeo (TP_INT esta cableado pero sin usar),
 * y TOUCH_CTRL_READ devuelve 0 puntos cuando no hay toque --ese es exactamente
 * el RELEASED que LVGL necesita para cerrar el gesto, no hay que filtrar nada
 * mas.  Las coordenadas ya vienen en el sistema del panel (320x480 en
 * vertical), igual que las del driver de pantalla.
 */
static void lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    touch_point_t pt;

    (void)drv;

    if (control(TOUCH0, TOUCH_CTRL_READ, (long) &pt, 1) > 0)
    {
        if (screenOff || wakeSwallow)
        {
            /* Dedo sobre la pantalla apagada: enciende, pero se traga.  Si no
             * se tragase, el primer toque del dia caeria encima del widget que
             * hubiera en ese punto --el dock de la galeria esta justo abajo--,
             * y al soltarlo contaria como CLICKED.  Se traga tambien todo lo
             * que quede presionado despues de encender, hasta que se levanta
             * el dedo. */
            if (screenOff)
            {
                screenWake();
            }
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
        lastTouchMs = millis();
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = pt.x;
        data->point.y = pt.y;
    }
    else
    {
        /* Sin dedo: aqui se levanta el candado del despertar. */
        wakeSwallow = 0;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/**
 * @ingroup lvgl
 *
 * La hebra residente de la UI: reloj de LVGL + refresco + puntero, en un solo
 * critico.  Sin ella la interfaz solo avanza cuando una app java llama a
 * LVGL.tick(), y eso tiene dos consecuencias que se ven en el panel: el
 * indev se muestrea al ritmo del bucle de la app --un arrastre de dedo deja de
 * ser un arrastre--, y mientras la app hace su trabajo la pantalla esta
 * congelada.  Con la hebra, LVGL refresca siempre y las apps solo crean
 * widgets y leen la cola de eventos.
 *
 * El critico abarca tick + handler porque system/lock.c reentra para el dueno
 * (solo sube `count'), asi que el lv_timer_handler() de abajo puede llamar a
 * control(LCD0, LCD_CTRL_BLIT) y a las rutas de ficheros de lvglfs.c sin
 * abrir el monitor del device dos veces.
 *
 * El bucle no duerme dentro del critico: si no, una app quedaba bloqueada en
 * lvgl_lock() durante los 5 ms del sleep().
 */
thread lvglTask(void)
{
    while (TRUE)
    {
        if (lvglReady)
        {
            uint32_t now;

            lvgl_lock();
            now = millis();
            if (now != last_tick)
            {
                lv_tick_inc(now - last_tick);
                last_tick = now;
            }
            lv_timer_handler();
            /* Cuenta atras del apagado.  Se comprueba aqui, con el monitor
             * cogido y despues del refresco, para que no haya dos hebras
             * moviendo el backlight a la vez. */
            if (idleOffMs != 0 && !screenOff &&
                (uint32_t)(now - lastTouchMs) >= idleOffMs)
            {
                screenBlank();
            }
            lvgl_unlock();
        }
        sleep(LVGL_TASK_MS);
    }

    return 0;
}

/**
 * @ingroup lvgl
 *
 * Inicializa LVGL y registra el driver de pantalla y el de puntero.  Se llama
 * una sola vez, en el arranque del kernel (devtab); si falla la reserva de
 * PSRAM el device queda sin registrar y las natives devuelven "no hay UI" en
 * lugar de correr por punteros nulos.
 */
devcall lvglInit(device *devptr)
{
    uint32_t pixels = (uint32_t)LCD_W * DRAW_LINES;
    tid_typ tid;

    (void)devptr;

    lvglmon = moncreate();
    if (SYSERR == lvglmon)
    {
        return SYSERR;
    }

    buf1 = (lv_color_t *)psram_malloc(pixels * sizeof(lv_color_t));
    buf2 = (lv_color_t *)psram_malloc(pixels * sizeof(lv_color_t));
    if (buf1 == 0 || buf2 == 0)
    {
        kprintf("[lvgl] sin PSRAM para los buffers de dibujo\r\n");
        return SYSERR;
    }

    /* El lienzo del espejo: 307.200 B, de sobra en los 31,4 MB libres.  Si no
     * hay, no pasa nada funcional: simplemente no habra capturas. */
    lvgl_shadow = (uint16_t *) psram_malloc((uint32_t)LCD_W * LCD_H *
                                            sizeof(uint16_t));
    if (lvgl_shadow == 0)
    {
        kprintf("[lvgl] sin PSRAM para el espejo: no habra capturas\r\n");
    }
    else
    {
        memset(lvgl_shadow, 0, (uint32_t)LCD_W * LCD_H * sizeof(uint16_t));
        kprintf("[lvgl] espejo de fotograma en 0x%08x\r\n",
                (unsigned)(uintptr_t)lvgl_shadow);
    }

    lv_init();

    /* El decodificador de JPEG (sjpg) y cualquier lv_img_set_src() con ruta
     * necesitan una letra de filesystem registrada; la nuestra ('S', la SD)
     * se monta despues de lv_init porque lv_fs_init() --que lv_init llama--
     * es quien vacia la lista de drivers. */
    lvgl_fs_register();

    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, pixels);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_W;
    disp_drv.ver_res = LCD_H;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    /* El screen que lv_init deja creado viene con el fondo claro del tema, y
     * el primer refresco de la hebra de UI lo vuelca al panel: eso es el
     * "flash blanco" que se ve entre el splash de arranque y el launcher.  Se
     * pinta del gris oscuro del workspace aqui mismo, antes de que corra ni un
     * lv_timer_handler(), para que el panel nunca salga en blanco. */
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);

    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_cb;

    /* Los valores que trae lv_indev_drv_init() (gesto a los 50 px acumulados y
     * con >= 3 px por lectura) hacen que deslizar el dedo NO cuente como gesto
     * salvo que se arrastre rapido y largo: LVGL limpia la suma acumulada cuando
     * el desplazamiento de una lectura esta por debajo del minimo, asi que un
     * arrastre suave nunca llega al limite.  Cada lectura va ligada al refresco
     * del display (LV_DISP_DEF_REFR_PERIOD), de modo que 3 px por lectura son
     * unos 190 px/s de dedo.  El panel tiene 320 px de ancho y el muestreo de
     * TOUCH0 es en vivo, asi que se baja la velocidad minima a 1 px y el
     * recorrido a 30 px.  30 y no menos, para que un toque con el dedo temblando
     * no se lea como swipe. */
    indev_drv.gesture_min_velocity = 1;
    indev_drv.gesture_limit = 30;

    lv_indev_drv_register(&indev_drv);

    last_tick = millis();
    lvglReady = 1;

    /* La hebra que lleva el reloj y el refresco desde ahora.  Se lanza aqui,
     * en la inicializacion del devtab: create() es legal porque nullMain ya
     * paso por createinit() y memoryInit() cuando inicia los devices, y la
     * pila sale del mismo heap de L2MEM que usaran las hebras java. */
    tid = create(lvglTask, LVGL_TASK_STK, INITPRIO, "lvglui", 0);
    if (SYSERR == tid)
    {
        kprintf("[lvgl] sin hebra de UI: la pantalla no refrescara sola\r\n");
    }
    else
    {
        ready(tid, RESCHED_NO);
    }

    kprintf("[lvgl] %d.%d.%d listo (%dx%d, buffers %u x %u B en PSRAM)\r\n",
            lv_version_major(), lv_version_minor(), lv_version_patch(),
            LCD_W, LCD_H, (unsigned)pixels, (unsigned)sizeof(lv_color_t));

    /* Los codigos que el SDK java tiene como constantes numericas
     * (esp32p4/LVGL.java: CLICKED, GESTURE y GEST_*).  Se imprimen aqui porque
     * si una actualizacion de LVGL desplaza el enum, las apps dejarian de
     * reconocer sus botones y sus swipe sin ningun otro aviso. */
    kprintf("[lvgl] evt CLICKED=%d GESTURE=%d dir L=%d R=%d T=%d B=%d\r\n",
            (int) LV_EVENT_CLICKED, (int) LV_EVENT_GESTURE,
            (int) LV_DIR_LEFT, (int) LV_DIR_RIGHT,
            (int) LV_DIR_TOP, (int) LV_DIR_BOTTOM);
    return OK;
}

/**
 * @ingroup lvgl
 *
 * Operaciones del device.  Ninguna llama a lv_init() otra vez: la libreria
 * no esta pensada para reinicializarse en caliente, y un reset de la UI se
 * hace tirando el arbol de objetos (LVGL_CTRL_INIT), no la libreria.
 */
devcall lvglControl(device *devptr, int func, long arg1, long arg2)
{
    (void)devptr;
    (void)arg1;
    (void)arg2;

    if (!lvglReady)
    {
        return SYSERR;
    }

    lock(lvglmon);
    switch (func)
    {
    case LVGL_CTRL_INIT:
        /* Pantalla vacia: borra todos los objetos del screen activo.  Los
         * handles que reparten las natives quedan colgando y ellas los
         * descartan al re-crear su interfaz. */
        lv_obj_clean(lv_scr_act());
        unlock(lvglmon);
        return OK;

    case LVGL_CTRL_TICK:
    {
        uint32_t now = millis();

        if (now != last_tick)
        {
            lv_tick_inc(now - last_tick);
            last_tick = now;
        }
        unlock(lvglmon);
        return OK;
    }

    case LVGL_CTRL_TASK:
        /* Refresco si toca, animaciones y lectura del puntero. */
        lv_timer_handler();
        unlock(lvglmon);
        return OK;

    case LVGL_CTRL_READY:
        unlock(lvglmon);
        return OK;

    case LVGL_CTRL_IDLE:
        /* Politica de pantalla: arg1 = ms de inactividad antes de apagar el
         * backlight; 0 = no apagar nunca.  Se reinicia la cuenta aqui, si no,
         * una app que lo activara tarde se encontraria con que el kernel lleva
         * mas tiempo del pedido sin dedo y la pantalla se va al instante. */
        idleOffMs = (uint32_t) arg1;
        lastTouchMs = millis();
        if (idleOffMs == 0 && screenOff)
        {
            screenWake();
        }
        unlock(lvglmon);
        return OK;

    default:
        unlock(lvglmon);
        return SYSERR;
    }
}

/* ------------------------------------------------------------------ */
/* Volcado del espejo a BMP                                            */

/**
 * Escribe lvgl_shadow como BMP de 24 bits (BGR, de abajo arriba, que es el
 * orden en que el formato guarda las filas).
 *
 * No usa el device LCD ni LVGL: es una foto del espejo, y por eso se puede
 * llamar aunque el panel este apagado.  El unico cuidado es no escribir en la
 * SD mientras otra hebra la esta usando: la FAT de Xinu ya serializa por su
 * cuenta, y aqui solo se abre/escribe/cierra.
 *
 * Devuelve OK, o SYSERR si no hay espejo, no hay ruta o la SD no abre.
 */
int lvglShotBmp(const char *ruta)
{
    static uint8_t cab[54];
    static uint8_t fila[LCD_W * 3];
    void    *f;
    uint32_t total;
    int      x, y;

    if (NULL == lvgl_shadow || NULL == ruta || '\0' == ruta[0])
    {
        return SYSERR;
    }

    f = fl_fopen(ruta, "w");
    if (NULL == f)
    {
        return SYSERR;
    }

    /* Cabecera BITMAPFILEHEADER + BITMAPINFOHEADER, little endian a mano (en
     * el kernel no se asume que un struct de C quede empaquetado). */
    total = 54UL + (uint32_t)LCD_W * (uint32_t)LCD_H * 3UL;
    memset(cab, 0, sizeof(cab));
    cab[0]  = 'B';
    cab[1]  = 'M';
    cab[2]  = (uint8_t)(total);
    cab[3]  = (uint8_t)(total >> 8);
    cab[4]  = (uint8_t)(total >> 16);
    cab[5]  = (uint8_t)(total >> 24);
    cab[10] = 54;                       /* offset de los pixeles             */
    cab[14] = 40;                       /* tamano del BITMAPINFOHEADER       */
    cab[18] = (uint8_t)(LCD_W);
    cab[19] = (uint8_t)(LCD_W >> 8);
    cab[22] = (uint8_t)(LCD_H);
    cab[23] = (uint8_t)(LCD_H >> 8);
    cab[26] = 1;                        /* planos                            */
    cab[28] = 24;                       /* bits por pixel                    */

    if (fl_fwrite(cab, 1, sizeof(cab), f) != (int)sizeof(cab))
    {
        fl_fclose(f);
        return SYSERR;
    }

    for (y = LCD_H - 1; y >= 0; y--)
    {
        const uint16_t *p = &lvgl_shadow[(uint32_t)y * LCD_W];

        for (x = 0; x < LCD_W; x++)
        {
            uint16_t v = p[x];

            /* RGB565 -> BGR888: la fila del BMP va en B, G, R. */
            fila[x * 3 + 0] = (uint8_t)((v & 0x001FU) << 3);
            fila[x * 3 + 1] = (uint8_t)(((v >> 5) & 0x003FU) << 2);
            fila[x * 3 + 2] = (uint8_t)(((v >> 11) & 0x001FU) << 3);
        }
        if (fl_fwrite(fila, 1, sizeof(fila), f) != (int)sizeof(fila))
        {
            fl_fclose(f);
            return SYSERR;
        }
    }

    fl_fclose(f);
    return OK;
}
