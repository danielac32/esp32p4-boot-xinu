package esp32p4;

/**
 * esp32p4.IO — consola y utilidades de texto.
 *
 * print/println escriben en la consola del kernel (y de ahi al USB-CDC o al
 * telnet que haya montado la tarea).  readChar bloquea hasta una tecla.
 * strLen/strAt/printBuf permiten recorrer e imprimir texto que la app ya
 * tiene, sin necesidad de la clase String completa de un JDK.
 *
 * El cuerpo C esta en device/jvm/native_io.c.
 */
public class IO {

    /** Escribe {@code s} sin salto de linea. */
    public static native void print(String s);

    /** Escribe {@code s} con salto de linea. */
    public static native void println(String s);

    /** Espera una tecla y devuelve su codigo (-1 al final de entrada). */
    public static native int readChar();

    /** Longitud de {@code s} en caracteres. */
    public static native int strLen(String s);

    /** Codigo del caracter i de {@code s} (-1 fuera de rango). */
    public static native int strAt(String s, int i);

    /** Imprime {@code len} codigos del buffer {@code buf} como texto. */
    public static native void printBuf(int[] buf, int len);
}
