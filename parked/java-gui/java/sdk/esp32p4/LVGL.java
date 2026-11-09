package esp32p4;

/**
 * esp32p4.LVGL — UI grafica (LVGL v8) desde Java.
 *
 * El kernel ya dejo LVGL inicializado (device LVGL0: pantalla sobre LCD0 y
 * dedo sobre TOUCH0), asi que lo unico que hace init() es vaciar la pantalla
 * y preparar el espacio de handles de esta app.
 *
 * Modelo: cada widget es un int (handle).  Se crea con las fabricas
 * label/button/rect/img y se manipula con setText/setPos/setSize/setColor.
 * El bucle de la app llama tick() cada ~16 ms y pregunta pollEvent() por los
 * toques:
 *
 *   while (true) {
 *       LVGL.tick();
 *       if (LVGL.pollEvent() == 1) {
 *           int w = LVGL.eventWidget();
 *           if (w == okBtn) { ... }
 *       }
 *       HW.delay(16);
 *   }
 *
 * Un boton solo dispara LV_EVENT_CLICKED (pulsar y soltar encima), que es lo
 * que devuelve pollEvent(); no hay eventos de arrastre ni de pulsacion larga.
 *
 * Restricciones de esta VM: no hay GC ni hilos UI.  LVGL no es reentrante y
 * el device lo serializa con un monitor, pero aun asi la UI de una app se
 * maneja desde una sola tarea.  Reusar los widgets en vez de recrearlos en
 * cada frame.
 */
public class LVGL {

    /** Vacía la pantalla y prepara los handles de esta app. */
    public static native void init();

    /** Crea un screen nuevo, lo carga y devuelve su handle (0 si fallo). */
    public static native int screen();

    /** Limpia el screen activo (0xRRGGBB) y olvida sus handles. */
    public static native void clear(int color);

    /** Crea un label. Devuelve handle (0 si fallo). */
    public static native int label(int parent, String text, int x, int y);

    /** Crea un boton con texto centrado y evento CLICKED. Devuelve handle. */
    public static native int button(int parent, String text, int x, int y,
                                    int w, int h);

    /** Crea un rectangulo de color. Devuelve handle. */
    public static native int rect(int parent, int x, int y, int w, int h,
                                  int color);

    /** Cambia el texto de un label (o del label hijo de un boton). */
    public static native void setText(int w, String text);

    /** Formatea en C (sin asignar Strings en Java): fmt con %d, a, b. */
    public static native void setTextFmt(int w, String fmt, int a, int b);

    /** Mueve un widget. */
    public static native void setPos(int w, int x, int y);

    /** Redimensiona un widget. */
    public static native void setSize(int w, int width, int height);

    /** Color de texto (fg) y fondo (bg), 0xRRGGBB. */
    public static native void setColor(int w, int fg, int bg);

    /** Esquinas redondeadas: 14 en tarjetas, 11 en pastillas, 0 en cuadrados. */
    public static native void setRadius(int w, int r);

    /** Quita degradado, borde, sombra y contorno del tema de LVGL. */
    public static native void setFlat(int w);

    /** Relleno interior en px, para que el texto no toque el borde. */
    public static native void setPad(int w, int pad);

    /**
     * Cuerpo del texto: 14 (el por defecto) o 26 (titulos).  Cualquier otro
     * valor no cambia nada.  La hereda el label hijo de un boton, asi que
     * setFont(boton, 26) tambien agranda su texto.
     */
    public static native void setFont(int w, int px);

    /**
     * Avanza el tick de LVGL y ejecuta el refresco (lv_timer_handler).
     *
     * Las apps ya no lo necesitan: la tarea residente "lvglui" del kernel
     * mueve el reloj y el refresco unos 200 fps.  Se conserva para casos sin
     * UI residente y para forzar un frame inmediatamente.
     */
    public static native void tick();

    /**
     * Tiempo de pantalla: apaga el backlight cuando pasan {@code ms}
     * milisegundos sin que nadie toque el panel.  El primer dedo lo vuelve a
     * encender y ese toque NO se entrega a ninguna app (despertar no es
     * pulsar).  Con 0 se apaga nunca --asi arranca el kernel, para que la shell
     * y las pruebas no se queden a oscuras--.
     *
     * Lo lleva la hebra residente del panel, que ve los toques de todas las
     * apps: basta con que lo ponga una (el launcher) al arrancar.
     */
    public static native void idleOff(int ms);

    /**
     * Devuelve 1 si habia un evento de ESTA tarea y lo consume; 0 si no hay.
     * Cada tarea solo ve los eventos de los widgets que ella creo: el launcher
     * y la app en primer plano comparten el panel, no la cola.
     */
    public static native int pollEvent();

    /** Handle del ultimo evento consumido (el que devolvio pollEvent()==1). */
    public static native int eventWidget();

    /** Codigo del ultimo evento consumido: CLICKED, GESTURE o 0 si no hubo. */
    public static native int eventCode();

    /**
     * Direccion del ultimo gesto consumido (GEST_LEFT/RIGHT/TOP/BOTTOM) o 0.
     * Solo tiene valor cuando eventCode() == GESTURE.
     */
    public static native int eventGesture();

    /** Handle del screen cargado ahora mismo, o 0 si no esta registrado. */
    public static native int activeScreen();

    /** Carga un screen registrado (el propio, o el de una app en segundo plano). */
    public static native void loadScreen(int screen);

    /* Codigos de evento que devuelve eventCode().  Son los valores numericos
     * de lv_event_code_t en LVGL 8.3 (lvgl/src/core/lv_event.h); el device los
     * imprime en el arranque, asi que el log de la placa los verifica solo. */
    public static final int CLICKED = 7;    /**< LV_EVENT_CLICKED                   */
    public static final int GESTURE = 12;   /**< LV_EVENT_GESTURE                   */

    /* Direcciones que devuelve eventGesture() (lv_dir_t, lv_area.h). */
    public static final int GEST_NONE   = 0; /**< LV_DIR_NONE                       */
    public static final int GEST_LEFT   = 1; /**< LV_DIR_LEFT                       */
    public static final int GEST_RIGHT  = 2; /**< LV_DIR_RIGHT                      */
    public static final int GEST_TOP    = 4; /**< LV_DIR_TOP                        */
    public static final int GEST_BOTTOM = 8; /**< LV_DIR_BOTTOM                     */

    /** Borra un widget y libera su reserva de pixeles. */
    public static native void del(int w);

    /**
     * Muestra una imagen de la tarjeta SD.  Devuelve handle, o 0 si el
     * fichero no existe o no cabe.  Dos formatos segun la extension:
     *
     *   "/img/a.jpg"  JPEG, lo decodifica LVGL (sjpg).  w y h se IGNORAN: el
     *                 tamano lo da la cabecera del JPEG.  Ojo: la decodificacion
     *                 reserva la imagen entera en PSRAM (ancho x alto x 3 B),
     *                 asi que las fotos hay que reducirlas al tamano del panel.
     *   "/img/a.raw"  RGB565 crudo de w*h*2 bytes, sin cabecera.  Se carga
     *                entero a PSRAM; es la via barata para iconos.
     *
     * Ruta absoluta de la tarjeta.  El alto real del widget se conoce despues
     * del primer tick(): ver getWidth/getHeight.
     */
    public static native int img(int parent, String path, int x, int y, int w,
                                 int h);

    /** Ancho de un widget en px (0 hasta el primer refresco). */
    public static native int getWidth(int w);

    /** Alto de un widget en px (0 hasta el primer refresco). */
    public static native int getHeight(int w);

    /** Ancho de la pantalla activa en px. */
    public static native int screenWidth();

    /** Alto de la pantalla activa en px. */
    public static native int screenHeight();
}
