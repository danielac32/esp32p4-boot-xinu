package esp32p4;

/**
 * esp32p4.HW — capa fina de hardware del ESP32-P4.
 *
 * Un Java normal no "programa hardware": esto es lo que una app usa por
 * debajo para dormir y medir el tiempo.  Cada metodo es native; el cuerpo C
 * vive en device/jvm/native_hw.c.
 *
 * delay() cede la CPU (es el sleep() del kernel), asi que una app que llama
 * delay(16) en su bucle deja trabajar a las demas.
 *
 * estado en esta placa: delay() y millis() van al kernel; gpioWrite/gpioRead y
 * sound() todavia son traza por consola (tarea abierta: enlazarlos con los
 * devices gpio0 y audio0 del P4).
 */
public class HW {

    /** Espera {@code ms} milisegundos (cede la CPU). */
    public static native void delay(int ms);

    /** Milisegundos desde el arranque del kernel. */
    public static native int millis();

    /** Escribe {@code val} (0/1) en el GPIO {@code pin}. */
    public static native void gpioWrite(int pin, int val);

    /** Lee el GPIO {@code pin} (0/1). */
    public static native int gpioRead(int pin);

    /** Tono de {@code freq} Hz durante {@code ms} ms (traza: sin audio aun). */
    public static native void sound(int freq, int ms);

    /**
     * Cadena del build del kernel, con su contador #NNNN: identifica la imagen
     * que esta corriendo ahora mismo, que no siempre es la ultima compilada.
     */
    public static native String version();

    /** Frecuencia en Hz del reloj del kernel (el timer, no el nucleo). */
    public static native int clkFreq();

    /* Los dos montones del P4.  L2MEM es el SRAM interno: de el salen las pilas
     * de todas las hebras y el heap del kernel, y es el que se queda corto.
     * PSRAM es el heap grande: estado de la VM, pixmaps y JPEG.
     *   total        el rango completo del monton, en bytes
     *   free         lo libre, en bytes
     *   maxBlock     el bloque libre mas grande contiguo, en bytes: lo que de
     *                verdad puede pedir una reserva grande (fragmentacion) */

    /** Bytes totales del heap de L2MEM. */
    public static native int memTotal();

    /** Bytes libres del heap de L2MEM. */
    public static native int memFree();

    /** Mayor bloque libre contiguo de L2MEM, en bytes. */
    public static native int memMaxBlock();

    /** Bytes totales del heap de PSRAM. */
    public static native int psramTotal();

    /** Bytes libres del heap de PSRAM. */
    public static native int psramFree();

    /** Mayor bloque libre contiguo de PSRAM, en bytes. */
    public static native int psramMaxBlock();
}
