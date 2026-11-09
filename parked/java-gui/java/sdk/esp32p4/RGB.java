package esp32p4;

/**
 * esp32p4.RGB — LED RGB.
 *
 * Existe para que el codigo de las apps del S3 compile tambien aqui.  En el
 * ESP32-P4 (Waveshare WIFI6-Touch-LCD-3.5) no hay LED RGB discreto, asi que el
 * cuerpo nativo (device/jvm/native_hw.c) solo deja la traza por consola.  Si
 * algun dia se manda color de verdad, sera al backlight o a un neopixel en un
 * GPIO, no a esta firma.
 */
public class RGB {

    /** Enciende los canales indicados; 0 apaga ese canal. */
    public static native void set(int r, int g, int b);

    /** Apaga los tres canales (recuerda el ultimo color). */
    public static native void off();

    /** Devuelve el ultimo color como 0x00RRGGBB. */
    public static native int get();
}
