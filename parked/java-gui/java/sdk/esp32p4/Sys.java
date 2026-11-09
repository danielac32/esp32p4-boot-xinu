package esp32p4;

/**
 * esp32p4.Sys — servicios del kernel desde Java.
 *
 * Permite a una app lanzar otra .class como tarea, matarla, dormir o ceder la
 * CPU y consultar su tid.  Se apoya en que la VM es reentrante: cada tarea
 * java tiene su propia instancia del interprete.  El cuerpo C esta en
 * device/jvm/native_sys.c.
 *
 * runHere() es lo que usa el workspace para abrir una app sin perder su
 * propio estado: guarda el interprete de quien llama, ejecuta la otra clase
 * hasta que retorna, y lo restaura.
 */
public class Sys {

    /**
     * Lanza /java/&lt;cls&gt;.class como tarea, con {@code args} en su
     * main(String[]), y espera a que termine.  Devuelve 1 si ok.
     */
    public static native int run(String cls, String[] args);

    /**
     * Ejecuta /java/&lt;cls&gt;.class en la tarea ACTUAL (sin crear tarea
     * nueva): el estado JVM se guarda y se restaura, asi que el llamador
     * reanuda intacto.  Bloqueante.  Devuelve 1 si ok.
     */
    public static native int runHere(String cls, String[] args);

    /** Mata la tarea {@code tid}.  Devuelve 1 si ok. */
    public static native int kill(int tid);

    /**
     * Lanza /java/&lt;cls&gt;.class como tarea y devuelve su tid
     * inmediatamente, sin esperar.  Es la via del workspace: necesita seguir
     * vivo para pintar el banner y atender el dedo mientras la app corre.
     * Sin argumentos; 0 si no hay hueco en la tabla de tareas.
     *
     * Para cerrar una app lanzada asi NO usar kill(): matar a otra hebra
     * mientras tiene el monitor de LVGL deja el bus colgado.  Se usa
     * send(tid, codigo) y la app se sale ella sola al contestar, que es cuando
     * el kernel le limpia la interfaz (lvgl_forget_task).
     */
    public static native int start(String cls);

    /** Manda {@code value} (&gt;0) a la tarea {@code tid}.  Devuelve 1 si ok. */
    public static native int send(int tid, int value);

    /** Saca el mensaje pendiente de esta tarea, o -1 si no hay ninguno. */
    public static native int recv();

    /** 1 si la tarea {@code tid} sigue existiendo. */
    public static native int alive(int tid);

    /**
     * Tid de la hebra que lanzo esta.  Como el launcher crea cada app desde su
     * propia hebra, el padre de una app es el workspace: por ahi puede pedirle
     * que la pase a segundo plano sin que el kernel lleve la cuenta de quien
     * manda.
     */
    public static native int parent();

    /** Pico de pila observado por esta tarea, en bytes. */
    public static native int stackPeak();

    /** Tamano de la pila de esta tarea, en bytes. */
    public static native int stackTotal();

    /* Codigos de mensaje entre el workspace y una app (Sys.send/recv).
     * Un launcher y una app son dos tareas distintas y se hablan por el canal
     * de mensajes de Xinu.  Xinu admite UN mensaje sin leer por hebra, asi que
     * el que manda tiene que reintentar o aceptar el rechazo. */
    public static final int MSG_SHOW = 1;   /**< vuelve al frente             */
    public static final int MSG_QUIT = 2;   /**< cierra y libera la UI        */
    public static final int MSG_HIDE = 3;   /**< la app pide pasar a atras    */
    public static final int MSG_FREE = 4;   /**< suelta sus widgets, sigue viva */

    /* MSG_FREE es el precio de tener varias apps vivas a la vez.  La tabla de
     * handles de LVGL (device/jvm/native_lvgl.c) es UNA sola para el launcher y
     * todas las apps, con WIDGET_MAX sitios, y LVGL.clear() solo borra los
     * widgets de la tarea que llama: una app minimizada seguia ocupando los
     * suyos y a la siguiente app le faltaban (la galeria pintaba 4 miniaturas
     * de 6 y cortaba porque LVGL.button() devolvio 0).  Con FREE la app ocultada
     * se queda solo con su screen --que clear() respeta por ser KIND_SCREEN-- y
     * lo pinta todo otra vez cuando el launcher le manda SHOW. */

    /** Duerme la tarea actual {@code ms} ms (cede la CPU). */
    public static native int sleep(int ms);

    /** Cede la CPU.  Devuelve 1. */
    public static native int yield();

    /** Tid de la tarea actual. */
    public static native int tid();
}
