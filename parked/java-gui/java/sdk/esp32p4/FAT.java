package esp32p4;

/**
 * esp32p4.FAT — tarjeta microSD (FAT32).
 *
 * La API sigue el estilo de java.io (RandomAccessFile / File) adaptado a esta
 * VM: no hay clase File ni class loader, asi que un fichero abierto es un int
 * (handle) devuelto por open().  El cuerpo C esta en device/jvm/native_fat.c
 * y va directo a fl_* de fatfs/, que ya tiene su monitor reentrante.
 *
 * Las rutas son absolutas desde la raiz de la tarjeta ("/apps/calc.txt"); una
 * ruta relativa se resuelve contra el directorio de trabajo de la shell.
 *
 * Una app no deberia tener la SD abierta a la vez que la shell o el httpd con
 * el mismo fichero: la FAT tiene monitor, pero no bloqueos entre tareas.
 */
public class FAT {

    /* ---- RandomAccessFile ----------------------------------------- */

    /** Abre un fichero. mode: "r","w","a","r+","w+","a+". Devuelve handle (>0) o 0. */
    public static native int open(String path, String mode);

    /** Cierra el handle. Devuelve 1 si ok. */
    public static native int close(int h);

    /** Lee hasta {@code len} bytes en {@code buf}; devuelve los leidos. */
    public static native int read(int h, byte[] buf, int len);

    /** Escribe hasta {@code len} bytes de {@code buf}; devuelve los escritos. */
    public static native int write(int h, byte[] buf, int len);

    /** Posiciona el cursor en {@code off}. Devuelve 1 si ok. */
    public static native int seek(int h, int off);

    /** Posicion actual del cursor. */
    public static native int tell(int h);

    /** 1 si el cursor esta al final. */
    public static native int eof(int h);

    /** Lee una linea (sin el '\\n'); "" al final. */
    public static native String readLine(int h);

    /** Escribe el texto {@code s}; devuelve los bytes escritos. */
    public static native int writeString(int h, String s);

    /* ---- atajos (toda la ruta) ------------------------------------ */

    /** Lee todo el fichero como texto ("" si no existe). */
    public static native String readAll(String path);

    /** Crea/sobrescribe {@code path} con {@code s}. Devuelve 1 si ok. */
    public static native int writeAll(String path, String s);

    /** Anade {@code s} al final de {@code path}. Devuelve 1 si ok. */
    public static native int appendString(String path, String s);

    /* ---- estilo File ---------------------------------------------- */

    /** 1 si el fichero/directorio existe. */
    public static native int exists(String path);

    /** 1 si {@code path} es un directorio. */
    public static native int isDirectory(String path);

    /** Tamano en bytes (0 si no existe). */
    public static native int length(String path);

    /** Borra un fichero. Devuelve 1 si ok. */
    public static native int delete(String path);

    /** Crea el directorio {@code path}. Devuelve 1 si ok. */
    public static native int mkdir(String path);

    /** Borra el directorio {@code path}. Devuelve 1 si ok. */
    public static native int rmdir(String path);

    /** Lista los nombres de {@code dir} (array vacio si no existe). */
    public static native String[] list(String dir);
}
