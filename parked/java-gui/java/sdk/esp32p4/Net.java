package esp32p4;

/**
 * esp32p4.Net — estado de la conexion del P4.
 *
 * Solo lectura por ahora: la WiFi la sube el kernel (o el comando "wifi up" de
 * la shell) y una app solo necesita saber si tiene IP y cual.  El cuerpo C esta
 * en device/jvm/native_net.c.
 *
 * Conectar, escanear o abrir sockets desde java todavia no esta (tarea
 * abierta #79).
 */
public class Net {

    /** IP actual como String, o null si no hay conexion. */
    public static native String ip();

    /** 1 si hay IP asignada. */
    public static native int isConnected();
}
