import esp32p4.HW;
import esp32p4.LVGL;
import esp32p4.Sys;

/*
 * monitor — el estado de los dos montones del P4, fresco cada segundo.
 *
 * El ESP32-P4 tiene dos heaps y esta app los enseña como los enseña un
 * monitor de sistema: barra de uso, libres y el bloque libre mas grande.  Ese
 * ultimo numero es el importante: si `libre' es grande pero `mayor bloque' es
 * chico, el monton esta fragmentado y una reserva gorda (un pixmap, un JPEG)
 * falla aunque haya sitio.
 *
 *   L2MEM   SRAM interno: pilas de todas las hebras + heap del kernel.  Es el
 *           que se queda corto.
 *   PSRAM   heap grande: estado de la VM, pixmaps y JPEG.
 *   pila    la de ESTA hebra (Sys.stackPeak es el maximo alcanzado, no el
 *           consumo actual).
 *
 * Las celdas de abajo son el historico de las ultimas MUESTRAS lecturas de L2:
 * la altura es lo libre.  Abrir y cerrar apps se ve ahi.
 *
 * Refresca solo textos y tamanos: no crea ni borra widgets en el bucle, para no
 * comerse la tabla de handles.  El reloj y el refresco los lleva la hebra
 * residente "lvglui", asi que nunca llamo a LVGL.tick().  Gesto arriba =
 * segundo plano.
 *
 * Compilar:  make
 * Subir:     make send
 */
public class monitor {

    static final int BG      = 0x0E1116;
    static final int CARD    = 0x1A222C;
    static final int GLASS   = 0x2A3542;
    static final int TXT     = 0xE6EAF0;
    static final int MUTED   = 0x8A97A8;
    static final int ACCENT  = 0x3A8DFF;
    static final int GREEN   = 0x2FBF8F;
    static final int AMBER   = 0xFFB347;

    static final int MUESTRAS = 16;      /* celdas del historico */
    static final int RENOV    = 1000;    /* ms entre lecturas */

    /* Glifo LV_SYMBOL_UP de la Montserrat compilada (ver la cabecera del
     * launcher).  Con escape de unicode para que el fuente siga siendo ASCII;
     * javac resuelve esos escapes tambien dentro de los comentarios, asi que
     * aqui se escribe sin la barra. */
    static final String IC_UP = "\uF077";
    static final int RASTRO   = 272;     /* ancho de las barras, en px */
    static final int CELDA    = 16;      /* ancho de una columna del historico */
    static final int ALT_CEL  = 60;      /* altura maxima del historico, en px */
    static final int BASE_CEL = 434;     /* suelo del historico */

    static int scr;
    static int cerrarPedida;
    static int gestoMs;
    static int sinUI;         /* 1 = el launcher nos pidio soltar los widgets */
    static int pinta;         /* 1 = hay que repintar al volver al frente     */

    /* L2MEM */
    static int l2Relleno;
    static int l2Linea1;
    static int l2Linea2;
    /* PSRAM */
    static int psRelleno;
    static int psLinea1;
    static int psLinea2;
    /* pila de la app */
    static int pRelleno;
    static int pLinea;
    /* historico */
    static int[] celda = new int[MUESTRAS];
    static int[] libres = new int[MUESTRAS];
    static int escritas;

    static void tarjeta(int y, int h) {
        LVGL.setRadius(LVGL.rect(scr, 10, y, 300, h, CARD), 14);
    }

    static int texto(String s, int x, int y, int color, int cuerpo) {
        int l = LVGL.label(scr, s, x, y);

        LVGL.setColor(l, color, BG);
        LVGL.setFont(l, cuerpo);
        return l;
    }

    /** Rastro oscuro + relleno encima, los dos redondos: el relleno cambia de
     *  ancho y nadie tiene que recrear nada. */
    static int barra(int y, int color) {
        LVGL.setRadius(LVGL.rect(scr, 24, y, RASTRO, 12, GLASS), 6);
        int r = LVGL.rect(scr, 24, y, 0, 12, color);

        LVGL.setRadius(r, 6);
        return r;
    }

    /** Bytes como KB enteros: con 32 MB en pantalla los decimales de mas estorban. */
    static String kb(int bytes) {
        return bytes / 1024 + " KB";
    }

    /** Porcentaje de uso del 0 al 100, calculado en KB para que `usado * 100'
     *  no desborde el entero de 32 bits con 32 MB. */
    static int porcentaje(int usado, int total) {
        int u = usado / 1024;
        int t = total / 1024;

        if (t <= 0) {
            return 0;
        }
        return (u * 100) / t;
    }

    /** Pone el relleno de una barra al porcentaje dado. */
    static void setBarra(int relleno, int pct) {
        LVGL.setSize(relleno, (RASTRO * pct) / 100, 12);
    }

    static void pintar() {
        int i, x;

        LVGL.clear(BG);
        texto("Monitor", 10, 30, TXT, 26);
        texto("memoria", 230, 38, MUTED, 14);

        /* --- L2MEM --- */
        tarjeta(60, 88);
        texto("L2MEM  (SRAM interno)", 24, 66, ACCENT, 14);
        l2Relleno = barra(88, ACCENT);
        l2Linea1 = texto("", 24, 108, TXT, 14);
        l2Linea2 = texto("", 24, 126, MUTED, 14);

        /* --- PSRAM --- */
        tarjeta(156, 88);
        texto("PSRAM  (heap grande)", 24, 162, GREEN, 14);
        psRelleno = barra(184, GREEN);
        psLinea1 = texto("", 24, 204, TXT, 14);
        psLinea2 = texto("", 24, 222, MUTED, 14);

        /* --- pila de esta hebra --- */
        tarjeta(252, 74);
        texto("Pila de la app", 24, 258, AMBER, 14);
        pRelleno = barra(280, AMBER);
        pLinea = texto("", 24, 300, TXT, 14);

        /* --- historico de L2 libre --- */
        tarjeta(334, 112);
        texto("libre de L2MEM, ultimas " + MUESTRAS + " lecturas", 24, 340, ACCENT, 14);
        for (i = 0; i < MUESTRAS; i++) {
            x = 24 + i * CELDA;
            celda[i] = LVGL.rect(scr, x, BASE_CEL, CELDA - 2, 1, GLASS);
            LVGL.setRadius(celda[i], 2);
            libres[i] = 0;
        }
        escritas = 0;
        texto(IC_UP + "  segundo plano", 10, 452, MUTED, 14);

        refrescar();
        /* Traza por consola de los cuatro numeros que mandan las barras, para
         * poder cotejar los natives sin tener que leer la pantalla. */
        System.out.println("monitor: L2 " + HW.memFree() + "/" +
                           HW.memTotal() + " B libres, mayor " +
                           HW.memMaxBlock() + " B   PSRAM " + HW.psramFree() +
                           "/" + HW.psramTotal() + " B, mayor " +
                           HW.psramMaxBlock() + " B   pila " +
                           Sys.stackPeak() + "/" + Sys.stackTotal() + " B");
    }

    /** Corre el historico una posicion a la izquierda y anade la lectura nueva. */
    static void desplazar(int nuevo) {
        int i;

        for (i = 0; i < MUESTRAS - 1; i++) {
            libres[i] = libres[i + 1];
        }
        libres[MUESTRAS - 1] = nuevo;
        if (escritas < MUESTRAS) {
            escritas = escritas + 1;
        }
    }

    /** Pinta una columna del historico: `fr` es lo libre sobre el total (0..100). */
    static void pintaCelda(int i, int fr) {
        int h = (ALT_CEL * fr) / 100;

        if (h < 1) {
            h = 1;
        }
        LVGL.setPos(celda[i], 24 + i * CELDA, BASE_CEL - h);
        LVGL.setSize(celda[i], CELDA - 2, h);
        if (fr < 15) {
            LVGL.setColor(celda[i], 0xFF6B6B, CARD);
        } else if (fr < 40) {
            LVGL.setColor(celda[i], AMBER, CARD);
        } else {
            LVGL.setColor(celda[i], ACCENT, CARD);
        }
    }

    static void refrescar() {
        int t2, f2, s2, tp, fp, sp, pct, usado, i;

        t2 = HW.memTotal();
        f2 = HW.memFree();
        s2 = HW.memMaxBlock();
        tp = HW.psramTotal();
        fp = HW.psramFree();
        sp = HW.psramMaxBlock();

        usado = t2 - f2;
        pct = porcentaje(usado, t2);
        setBarra(l2Relleno, pct);
        LVGL.setText(l2Linea1, "usada " + kb(usado) + " de " + kb(t2) +
                           "  (" + pct + " %)");
        LVGL.setText(l2Linea2, "libre " + kb(f2) + "   mayor bloque " + kb(s2));

        usado = tp - fp;
        pct = porcentaje(usado, tp);
        setBarra(psRelleno, pct);
        LVGL.setText(psLinea1, "usada " + kb(usado) + " de " + kb(tp) +
                            "  (" + pct + " %)");
        LVGL.setText(psLinea2, "libre " + kb(fp) + "   mayor bloque " + kb(sp));

        pct = porcentaje(Sys.stackPeak(), Sys.stackTotal());
        setBarra(pRelleno, pct);
        LVGL.setText(pLinea, "pico " + kb(Sys.stackPeak()) + " de " +
                           kb(Sys.stackTotal()) + "   tid " + Sys.tid() +
                           "   activo " + HW.millis() / 1000 + " s");

        desplazar(porcentaje(f2, t2));
        for (i = 0; i < MUESTRAS; i++) {
            if (i >= MUESTRAS - escritas) {
                pintaCelda(i, libres[i]);
            }
        }
    }

    static void esperar(int ms) {
        int slept = 0, slice;

        while (slept < ms && cerrarPedida == 0) {
            slice = ms - slept;
            if (slice > 50) {
                slice = 50;
            }
            HW.delay(slice);
            slept += slice;
            mensajes();
        }
    }

    /**
     * Mensajes del launcher.  Con MSG_FREE soltamos nuestros widgets (clear()
     * solo borra los de ESTE tid y respeta el screen): la tabla de handles es
     * unica para el launcher y todas las apps, y una app en segundo plano que
     * los guardaba dejaba a la siguiente sin sitio.  MSG_SHOW carga la pantalla
     * y marca el repintado, que hace el bucle de main.
     *
     * 0 = seguir, 1 = el launcher pidio cerrar.
     */
    static int mensajes() {
        int m = Sys.recv();

        if (m < 0) {
            return 0;
        }
        System.out.println("monitor: msg " + m);
        if (m == Sys.MSG_SHOW) {
            LVGL.loadScreen(scr);
            if (sinUI == 1) {
                sinUI = 0;
                pinta = 1;
            }
        } else if (m == Sys.MSG_FREE) {
            sinUI = 1;
            pinta = 0;
            LVGL.clear(BG);
            System.out.println("monitor: free: suelta su UI");
        } else if (m == Sys.MSG_QUIT) {
            cerrarPedida = 1;
            return 1;
        }
        return 0;
    }

    /** MSG_HIDE al launcher (nuestro padre), con reintento: el buzon de Xinu
     *  admite un mensaje sin leer por hebra y el aviso de muerte de una app
     *  cerrada puede estar ocupandolo. */
    static void aSegundoPlano() {
        int p = Sys.parent();
        int k, ok = 0;

        for (k = 0; k < 4 && ok == 0; k++) {
            ok = Sys.send(p, Sys.MSG_HIDE);
            if (ok == 0) {
                HW.delay(50);
            }
        }
        System.out.println("monitor: hide a " + p + " send " + ok);
    }

    public static void main(String[] args) {
        int cod, w;

        LVGL.init();
        scr = LVGL.screen();
        LVGL.clear(BG);
        pintar();

        while (cerrarPedida == 0) {
            if (pinta == 1) {
                pinta = 0;
                pintar();
            }
            while (LVGL.pollEvent() == 1) {
                cod = LVGL.eventCode();
                w = LVGL.eventWidget();
                System.out.println("monitor: evt cod " + cod + " dir " +
                                   LVGL.eventGesture() + " w " + w);
                if (cerrarPedida == 0) {
                    if (cod == LVGL.GESTURE) {
                        gestoMs = HW.millis();
                        if (LVGL.eventGesture() == LVGL.GEST_TOP) {
                            aSegundoPlano();
                        }
                    } else if (HW.millis() - gestoMs < 400) {
                        System.out.println("monitor: clic pos-gesto ignorado");
                    }
                }
            }
            esperar(RENOV);
            /* Sin refrescar mientras estamos en segundo plano y sin UI: los
             * handles que usaria ya no son nuestros, la tabla la reutilizo
             * otra tarea. */
            if (cerrarPedida == 0 && sinUI == 0) {
                refrescar();
            }
        }
        System.out.println("monitor: cierra tid " + Sys.tid());
    }
}
