import esp32p4.LVGL;
import esp32p4.HW;

/*
 * JpgTest — prueba de la decodificacion JPEG (LVGL + sjpg) en el P4.
 *
 * Carga dos JPEG de la SD y mide cuanto cuesta que acaben en el panel:
 *   - img() solo lee la cabecera (abre el fichero por el backend 'S' de
 *     device/lvgl/lvglfs.c y pregunta el tamanno; no decodifica);
 *   - el pixel data se produce en el refresco de lv_timer_handler(), o sea
 *     dentro de tick().  Como LVGL refresca a 30 ms de su reloj, los tick()
 *     hay que espaciarlos con HW.delay() para que pase tiempo real.
 *
 * El numero que importa es "tick mas caro": es el refresco que decodifica y
 * manda la franja por SPI2.
 *
 * Compilar:  make
 * Subir:     make send      (deja /java/JpgTest.class)
 * Ejecutar:  en la consola  java JpgTest
 */
public class JpgTest {

    /** Refresca hasta que w tenga tamanno y informa del coste. */
    static void pintar(String etiqueta, int w) {
        int f, t0, t1, dt, max;

        max = 0;
        t0 = HW.millis();
        for (f = 0; f < 60 && LVGL.getWidth(w) == 0; f++) {
            t1 = HW.millis();
            LVGL.tick();
            dt = HW.millis() - t1;
            if (dt > max) {
                max = dt;
            }
            HW.delay(16);
        }
        /* Dos refresco mas, ya con la imagen colocada: si el decodificador
         * trabaja, aqui es donde se ve. */
        for (f = 0; f < 4; f++) {
            t1 = HW.millis();
            LVGL.tick();
            dt = HW.millis() - t1;
            if (dt > max) {
                max = dt;
            }
            HW.delay(16);
        }
        dt = HW.millis() - t0;
        System.out.println("JpgTest: " + etiqueta + " tamanno=" +
                           LVGL.getWidth(w) + "x" + LVGL.getHeight(w) +
                           " total=" + dt + "ms tickMasCaro=" + max + "ms");
    }

    public static void main(String[] args) {
        int scr, cap, foto, small, f, t0, dt;

        LVGL.init();
        scr = LVGL.screen();
        LVGL.clear(0x101010);
        cap = LVGL.label(scr, "jpeg sjpg", 8, 4);

        /* El grande primero, solo, para que su refresco no se mezcle con el
         * de la pequena. */
        t0 = HW.millis();
        foto = LVGL.img(scr, "/test/foto.jpg", 10, 30, 0, 0);
        dt = HW.millis() - t0;
        System.out.println("JpgTest: foto handle=" + foto + " img()=" + dt +
                           "ms");
        pintar("foto", foto);

        t0 = HW.millis();
        small = LVGL.img(scr, "/test/small.jpg", 10, 440, 0, 0);
        dt = HW.millis() - t0;
        System.out.println("JpgTest: small handle=" + small + " img()=" + dt +
                           "ms");
        pintar("small", small);

        LVGL.setTextFmt(cap, "foto=%d small=%d", foto, small);
        pintar("cap+foto+small tras setText", scr);

        LVGL.del(foto);
        LVGL.del(small);
        for (f = 0; f < 10; f++) {
            LVGL.tick();
            HW.delay(16);
        }
        System.out.println("JpgTest: fin");
    }
}
