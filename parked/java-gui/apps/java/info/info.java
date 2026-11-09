import esp32p4.HW;
import esp32p4.LVGL;
import esp32p4.Net;
import esp32p4.Power;
import esp32p4.Sys;

/*
 * info — que es esta placa, leido del kernel, no de un manual.
 *
 * Todo lo que sale aqui lo dan los natives: HW.version() es la cadena del
 * build (con su contador #NNNN, la unica forma de saber que imagen esta
 * corriendo de verdad), HW.clkFreq() el reloj que el kernel declara,
 * HW.memTotal()/psramTotal() los dos montones, LVGL.screenWidth/Height la
 * pantalla, Net e Power el C6 y el AXP2101, y Sys.tid/parent/stack* la hebra
 * que estas viendo.  Nada es de adorno.
 *
 * Lo que NO se puede leer sin escribir un driver nuevo: la revision del chip
 * (efuses) y los MHz reales del nucleo.  El kernel declara 360 MHz; medido con
 * `syscheck' el nucleo va a 320 y un segundo del kernel dura ~1,26 s reales.
 * Por eso la tarjeta dice "declarado".
 *
 * Refresca una vez por segundo sin crear ni borrar widgets: solo cambia los
 * textos.  El reloj y el refresco los lleva la hebra residente "lvglui" del
 * kernel, asi que esta app nunca llama a LVGL.tick().
 *
 * Compilar:  make
 * Subir:     make send
 */
public class info {

    static final int BG     = 0x0E1116;
    static final int CARD   = 0x1A222C;
    static final int TXT    = 0xE6EAF0;
    static final int MUTED  = 0x8A97A8;
    static final int ACCENT = 0x3A8DFF;
    static final int GREEN  = 0x2FBF8F;

    static final int RENOV = 1000;      /* ms entre refrescos de los datos */

    /* Glifo LV_SYMBOL_UP de la Montserrat compilada: javac resuelve los
     * escapes de unicode hasta dentro de los comentarios, asi que aqui se
     * escribe sin la barra. */
    static final String IC_UP = "\uF077";

    static int scr;
    static int cerrarPedida;
    static int gestoMs;
    static int sinUI;         /* 1 = el launcher nos pidio soltar los widgets */
    static int pinta;         /* 1 = hay que repintar al volver al frente     */

    /* Kernel */
    static int kBuild;
    static int kFecha;
    static int kActivo;
    /* Chip y memoria */
    static int cL2;
    static int cPs;
    static int cPan;
    /* Red */
    static int rEstado;
    static int rIp;
    /* Bateria */
    static int rBat;
    static int rVolt;
    /* Tarea */
    static int tTid;
    static int tPila;

    /** Tarjeta: caja redonda y plana; los textos van ENCIMA.  Un rect no es
     *  clicable, asi que el dedo lo ignora. */
    static void tarjeta(int y, int h) {
        LVGL.setRadius(LVGL.rect(scr, 10, y, 300, h, CARD), 14);
    }

    static int texto(String s, int x, int y, int color, int cuerpo) {
        int l = LVGL.label(scr, s, x, y);

        LVGL.setColor(l, color, BG);
        LVGL.setFont(l, cuerpo);
        return l;
    }

    /* --- utilidades de cadena (el JVM de a bordo no tiene split) ---------- */

    /** Indice del primer espacio a partir de `desde', o el final si no hay. */
    static int espacio(String s, int desde) {
        int i = s.indexOf(" ", desde);

        if (i < 0) {
            return s.length();
        }
        return i;
    }

    /** La n-esima palabra de `s`, separada por espacios (1-based); "" si no
     *  llega tan lejos.  La cadena del kernel tiene este aspecto:
     *   (Embedded Xinu) (esp32p4) #1602 (daniel@fedora) jue 05 nov 2026 hh:mm:ss -04 */
    static String palabra(String s, int n) {
        int desde, hasta, k;

        desde = 0;
        k = 1;
        while (k < n) {
            hasta = espacio(s, desde);
            if (hasta >= s.length()) {
                return "";
            }
            desde = hasta + 1;
            k = k + 1;
        }
        hasta = espacio(s, desde);
        return s.substring(desde, hasta);
    }

    /** Quita los parentesis de alrededor, si los hay. */
    static String limpio(String s) {
        int a = s.indexOf("(");
        int b = s.indexOf(")");

        if (a == 0 && b > 0) {
            return s.substring(1, b);
        }
        return s;
    }

    /** "05" para un numero de una cifra: las horas se leen alineadas. */
    static String dosCifras(int n) {
        if (n < 10) {
            return "0" + n;
        }
        return "" + n;
    }

    /** Tiempo desde el arranque del kernel, en una linea. */
    static String tiempoActivo() {
        int t = HW.millis() / 1000;
        int h = t / 3600;
        int m = t / 60 - h * 60;
        int s = t - h * 3600 - m * 60;

        return h + "h " + dosCifras(m) + "m " + dosCifras(s) + "s";
    }

    /** Bytes en algo legible: KB abajo de 1 MB, MB con un decimal arriba. */
    static String kilos(int bytes) {
        int kb = bytes / 1024;

        if (kb < 1024) {
            return kb + " KB";
        }
        return kb / 1024 + "," + dosCifras((kb % 1024) * 10 / 1024) + " MB";
    }

    /* --- pintado ---------------------------------------------------------- */

    /** Las cinco tarjetas.  Las lineas que cambian nacen vacias y las llena
     *  refrescar(), que se llama aqui mismo antes de que nadie las mire. */
    static void pintar() {
        LVGL.clear(BG);
        texto("Info", 10, 30, TXT, 26);
        texto("de la placa", 214, 38, MUTED, 14);

        tarjeta(56, 92);
        texto("Kernel", 24, 62, ACCENT, 14);
        kBuild  = texto("", 24, 84, TXT, 14);
        kFecha  = texto("", 24, 106, TXT, 14);
        kActivo = texto("", 24, 128, MUTED, 14);

        tarjeta(154, 92);
        texto("Chip y memoria", 24, 160, ACCENT, 14);
        cL2  = texto("", 24, 182, TXT, 14);
        cPs  = texto("", 24, 204, TXT, 14);
        cPan = texto("", 24, 226, MUTED, 14);

        tarjeta(252, 66);
        texto("Red", 24, 258, ACCENT, 14);
        rEstado = texto("", 24, 280, MUTED, 14);
        rIp     = texto("", 24, 302, TXT, 14);

        tarjeta(324, 66);
        texto("Bateria", 24, 330, ACCENT, 14);
        rBat  = texto("", 24, 352, TXT, 14);
        rVolt = texto("", 24, 374, MUTED, 14);

        tarjeta(396, 66);
        texto("Esta tarea", 24, 402, ACCENT, 14);
        tTid  = texto("", 24, 424, TXT, 14);
        tPila = texto("", 24, 446, MUTED, 14);

        texto(IC_UP + "  segundo plano", 10, 464, MUTED, 14);

        refrescar();
        /* Una linea por consola con lo que se leyo del kernel: sirve para
         * comprobar los natives sin tener que adivinar que hay en la pantalla. */
        System.out.println("info: build " + palabra(HW.version(), 4) +
                           "  L2 " + HW.memTotal() + " B  PSRAM " +
                           HW.psramTotal() + " B  reloj " + HW.clkFreq() +
                           " Hz  pantalla " + LVGL.screenWidth() + "x" +
                           LVGL.screenHeight());
    }

    /** Vuelve a leer el kernel y reescribe los textos que cambian. */
    static void refrescar() {
        String v = HW.version();
        String build = palabra(v, 4);
        int i = build.indexOf("#");

        if (i >= 0) {
            build = build.substring(i + 1);
        }
        LVGL.setText(kBuild, "build #" + build + "   " + limpio(palabra(v, 3)));
        LVGL.setText(kFecha, "compilado " + palabra(v, 6) + " " + palabra(v, 7) +
                           " " + palabra(v, 8) + " " + palabra(v, 9) + " " +
                           corta(palabra(v, 10), 5));
        LVGL.setText(kActivo, "activo " + tiempoActivo() + "   reloj " +
                           HW.clkFreq() / 1000000 + " MHz (declarado)");

        LVGL.setText(cL2, "L2MEM    " + kilos(HW.memTotal()) +
                       "   libres " + kilos(HW.memFree()));
        LVGL.setText(cPs, "PSRAM    " + kilos(HW.psramTotal()) +
                       "   libres " + kilos(HW.psramFree()));
        LVGL.setText(cPan, LVGL.screenWidth() + " x " + LVGL.screenHeight() +
                       " px   ST7796 + FT6336 + ES8311");

        if (Net.isConnected() == 1) {
            LVGL.setText(rEstado, "wifi conectado");
            LVGL.setColor(rEstado, GREEN, BG);
        } else {
            LVGL.setText(rEstado, "wifi sin conectar");
            LVGL.setColor(rEstado, MUTED, BG);
        }
        LVGL.setText(rIp, "IP " + Net.ip());

        LVGL.setText(rBat, "carga " + Power.percent() + " %   " + nombreCarga());
        LVGL.setText(rVolt, Power.voltage() + " mV   AXP2101");

        LVGL.setText(tTid, "tid " + Sys.tid() + "   padre " + Sys.parent());
        LVGL.setText(tPila, "pila usada " + kilos(Sys.stackPeak()) + " de " +
                          kilos(Sys.stackTotal()));
    }

    /** Los n primeros caracteres de `s`, sin pasarse si es mas corta. */
    static String corta(String s, int n) {
        if (s.length() < n) {
            return s;
        }
        return s.substring(0, n);
    }

    /** El estado del cargador del PMIC como palabra, no como numero. */
    static String nombreCarga() {
        int c = Power.charge();

        if (c == 0) {
            return "descargando";
        }
        if (c == 1) {
            return "cargada";
        }
        if (c == 2) {
            return "cargando";
        }
        return "sin PMIC";
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
     * Mensajes del launcher.  Con MSG_FREE soltamos nuestros widgets: la tabla
     * de handles de LVGL es unica para el launcher y para todas las apps, y una
     * app en segundo plano que los guardaba dejaba sin sitio a la siguiente.
     * MSG_SHOW carga la pantalla y marca el repintado, que hace el bucle de
     * main con su propio pintar().
     *
     * 0 = seguir, 1 = el launcher pidio cerrar.
     */
    static int mensajes() {
        int m = Sys.recv();

        if (m < 0) {
            return 0;
        }
        System.out.println("info: msg " + m);
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
            System.out.println("info: free: suelta su UI");
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
        System.out.println("info: hide a " + p + " send " + ok);
    }

    static void gesto(int dir) {
        if (dir == LVGL.GEST_TOP) {
            aSegundoPlano();
        }
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
                System.out.println("info: evt cod " + cod + " dir " +
                                   LVGL.eventGesture() + " w " + w);
                if (cerrarPedida == 0) {
                    if (cod == LVGL.GESTURE) {
                        gestoMs = HW.millis();
                        gesto(LVGL.eventGesture());
                    } else if (HW.millis() - gestoMs < 400) {
                        System.out.println("info: clic pos-gesto ignorado");
                    }
                }
            }
            /* esperar() parte el segundo en rebanadas de 50 ms para atender el
             * MSG_QUIT del launcher sin retraso. */
            esperar(RENOV);
            /* Sin refrescar mientras estamos en segundo plano y sin UI: los
             * handles que usaria ya no son nuestros, ese sitio de la tabla lo
             * ocupo otra tarea. */
            if (cerrarPedida == 0 && sinUI == 0) {
                refrescar();
            }
        }
        System.out.println("info: cierra tid " + Sys.tid());
    }
}
