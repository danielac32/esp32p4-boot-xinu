import esp32p4.HW;
import esp32p4.LVGL;
import esp32p4.Net;
import esp32p4.Sys;

/*
 * ajustes — maqueta de interfaz de ajustes.
 *
 * Es una de las dos apps de maquetacion (con `musica') que sirven para ver el
 * rediseño aplicado a otra clase de pantalla: tarjetas redondeadas y planas,
 * cabecera de 26 px, pastillas de control y barra deslizante.  Los datos son
 * de adorno salvo la red, que si es real: Net.isConnected() y Net.ip() vienen
 * del C6; la lista de SSID, el brillo y el teclado son maqueta (conectar de
 * verdad es la tarea #92).
 *
 * Franja 0..26: el banner del launcher.  Las tarjetas son `rect' y los textos
 * van ENCIMA: un rect no es clicable, asi que el dedo lo ignora y cae sobre el
 * boton de debajo cuando lo hay.
 *
 * Compilar:  make
 * Subir:     make send
 */
public class ajustes {

    static final int BG      = 0x0E1116;
    static final int CARD    = 0x1A222C;
    static final int DARK    = 0x24303C;
    static final int GLASS   = 0x2A3542;
    static final int TXT     = 0xE6EAF0;
    static final int MUTED   = 0x8A97A8;
    static final int ACCENT  = 0x3A8DFF;
    static final int GREEN   = 0x2FBF8F;
    static final int RED     = 0xFF6B6B;

    static final int NREDS   = 3;
    static final int NTECLAS = 8;

    static int scr;
    static int sub;
    static int sinUI;         /* 1 = el launcher nos pidio soltar los widgets */
    static int pinta;         /* 1 = hay que repintar al volver al frente     */
    static int wifiSub;
    static int conmutador;
    static int encendido;
    static int brillo;
    static int brilloLbl;
    static int mando;
    static int masBrillo;
    static int menosBrillo;
    static int escrito;
    static int escrbLbl;
    static int borrar;
    static int[] redPill  = new int[NREDS];
    static int[] redLbl   = new int[NREDS];
    static int[] teclas   = new int[NTECLAS];
    static String letras;
    static String pwd;

    /** Tarjeta: caja redonda y plana; los textos se pintan encima. */
    static void tarjeta(int y, int h) {
        LVGL.setRadius(LVGL.rect(scr, 10, y, 300, h, CARD), 14);
    }

    /** Pastilla de control: plana, redonda y con el texto centrado. */
    static int pastilla(String texto, int x, int y, int w, int h, int fg) {
        int b = LVGL.button(scr, texto, x, y, w, h);

        if (b != 0) {
            LVGL.setFlat(b);
            LVGL.setRadius(b, h / 2);
            LVGL.setColor(b, fg, DARK);
        }
        return b;
    }

    /** Texto suelto sobre la tarjeta; `cuerpo` es 14 o 26. */
    static int texto(String s, int x, int y, int color, int cuerpo) {
        int l = LVGL.label(scr, s, x, y);

        LVGL.setColor(l, color, BG);
        LVGL.setFont(l, cuerpo);
        return l;
    }

    /** El brillo se pinta como una pista con un relleno: cambiarlo es cambiar
     *  el ancho del relleno, sin crear ni borrar nada. */
    static void pintarBrillo() {
        LVGL.setSize(mando, 10 + (brillo * 180) / 100, 18);
        LVGL.setTextFmt(brilloLbl, "Brillo %d", brillo, 0);
    }

    /** La contrasena se muestra como puntos, con su cuenta al lado. */
    static void pintarPwd() {
        String d = "";
        int i;

        for (i = 0; i < pwd.length(); i++) {
            d = d + "*";
        }
        LVGL.setText(escrito, d);
        LVGL.setTextFmt(escrbLbl, "%d de 12", pwd.length(), 0);
    }

    /** Linea de estado de la red: la conexion real del C6, no de adorno. */
    static void refrescarWifi() {
        String ip;

        if (encendido == 0) {
            LVGL.setText(conmutador, "OFF");
            LVGL.setColor(conmutador, MUTED, DARK);
            LVGL.setText(wifiSub, "WiFi apagado");
            return;
        }
        LVGL.setText(conmutador, "ON");
        LVGL.setColor(conmutador, GREEN, DARK);
        if (Net.isConnected() == 1) {
            ip = Net.ip();
            LVGL.setText(wifiSub, "conectado  " + ip);
        } else {
            LVGL.setText(wifiSub, "sin conectar");
        }
    }

    /** Pinta la maqueta entera.  Es segura de repetir: solo toca widgets, el
     *  estado (encendido, brillo, pwd) lo ponen las variables estaticas y lo
     *  manda main antes de la primera pasada. */
    static void pintar() {
        int i;

        LVGL.clear(BG);
        texto("Ajustes", 10, 30, TXT, 26);
        sub = texto("maqueta", 214, 38, MUTED, 14);

        /* --- conexion --- */
        tarjeta(64, 78);
        texto("WiFi", 24, 72, TXT, 14);
        wifiSub = texto("", 24, 100, MUTED, 14);
        conmutador = pastilla("ON", 234, 80, 60, 28, GREEN);
        refrescarWifi();

        /* --- brillo --- */
        tarjeta(150, 78);
        brilloLbl = texto("", 24, 158, TXT, 14);
        LVGL.setRadius(LVGL.rect(scr, 24, 190, 200, 10, GLASS), 5);
        mando = LVGL.rect(scr, 24, 186, 40, 18, ACCENT);
        LVGL.setRadius(mando, 9);
        menosBrillo = pastilla("-", 232, 182, 32, 26, TXT);
        masBrillo = pastilla("+", 270, 182, 32, 26, TXT);
        pintarBrillo();

        /* --- redes (maqueta) --- */
        tarjeta(236, 112);
        texto("Redes disponibles", 24, 242, TXT, 14);
        for (i = 0; i < NREDS; i++) {
            redLbl[i] = texto("casa-2G", 24, 268 + i * 26, MUTED, 14);
            redPill[i] = pastilla("conectar", 226, 264 + i * 26, 74, 24, ACCENT);
        }

        /* --- teclado en pantalla (maqueta) --- */
        tarjeta(356, 120);
        escrito = texto("", 24, 362, TXT, 14);
        escrbLbl = texto("", 214, 362, MUTED, 14);
        letras = "abcdefgh";
        for (i = 0; i < NTECLAS; i++) {
            teclas[i] = pastilla(letras.substring(i, i + 1), 24 + i * 34,
                                 384, 30, 28, TXT);
        }
        borrar = pastilla("borrar", 24, 418, 70, 26, RED);
        pintarPwd();
        HW.delay(120);
    }

    static int entre(int w, int[] v) {
        int i;

        for (i = 0; i < v.length; i++) {
            if (w != 0 && w == v[i]) {
                return i;
            }
        }
        return -1;
    }

    static void moverBrillo(int paso) {
        brillo = brillo + paso;
        if (brillo < 0) {
            brillo = 0;
        }
        if (brillo > 100) {
            brillo = 100;
        }
        pintarBrillo();
    }

    static void tecle(String s) {
        if (pwd.length() >= 12) {
            return;
        }
        pwd = pwd + s;
        pintarPwd();
    }

    static void clic(int w) {
        int i;

        if (w == 0) {
            return;
        }
        if (w == conmutador) {
            encendido = 1 - encendido;
            refrescarWifi();
        } else if (w == masBrillo) {
            moverBrillo(10);
        } else if (w == menosBrillo) {
            moverBrillo(-10);
        } else if (w == borrar) {
            if (pwd.length() > 0) {
                pwd = pwd.substring(0, pwd.length() - 1);
            }
            pintarPwd();
        } else {
            i = entre(w, teclas);
            if (i >= 0) {
                tecle(letras.substring(i, i + 1));
            } else {
                i = entre(w, redPill);
                if (i >= 0) {
                    LVGL.setText(sub, "red " + (i + 1));
                }
            }
        }
    }

    /**
     * Mensajes del launcher.  Con MSG_FREE soltamos nuestros widgets (la tabla
     * de handles de LVGL es unica para el launcher y para todas las apps) y con
     * MSG_SHOW volvemos al frente y pedimos el repintado.  El estado --encendido,
     * brillo y la contrasena escrita-- vive en los estaticos, no en los widgets,
     * asi que la maqueta sigue igual donde la dejo el dedo.
     *
     * 1 = el launcher pidio cerrar.
     */
    static int mensajes() {
        int m = Sys.recv();

        if (m < 0) {
            return 0;
        }
        System.out.println("ajustes: msg " + m);
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
            System.out.println("ajustes: free: suelta su UI");
        } else if (m == Sys.MSG_QUIT) {
            return 1;
        }
        return 0;
    }

    /** MSG_HIDE al launcher (nuestro padre), con reintento. */
    static void aSegundoPlano() {
        int p = Sys.parent();
        int k, ok = 0;

        for (k = 0; k < 4 && ok == 0; k++) {
            ok = Sys.send(p, Sys.MSG_HIDE);
            if (ok == 0) {
                HW.delay(50);
            }
        }
        System.out.println("ajustes: hide a " + p + " send " + ok);
    }

    public static void main(String[] args) {
        int cod, w;

        LVGL.init();
        scr = LVGL.screen();
        LVGL.clear(BG);
        /* El estado de la maqueta se pone aqui y no en pintar(), que a partir
         * de ahora se puede repetir cada vez que el launcher nos devuelva al
         * frente: si lo pusiera pintar(), repintar borraria el brillo y la
         * contrasena que el dedo ya habia tocado. */
        encendido = 1;
        brillo = 40;
        pwd = "";
        pintar();

        while (true) {
            if (pinta == 1) {
                pinta = 0;
                pintar();
            }
            while (LVGL.pollEvent() == 1) {
                cod = LVGL.eventCode();
                w = LVGL.eventWidget();
                System.out.println("ajustes: evt cod " + cod + " dir " +
                                   LVGL.eventGesture() + " w " + w);
                if (cod == LVGL.GESTURE) {
                    /* Unico gesto que atiende esta maqueta: arriba, salir al
                     * home (MSG_HIDE al launcher, nuestro padre).  Es LA via de
                     * salida; la casita del banner es el control secundario. */
                    if (LVGL.eventGesture() == LVGL.GEST_TOP) {
                        aSegundoPlano();
                    }
                } else {
                    clic(w);
                }
            }
            if (mensajes() == 1) {
                return;
            }
            HW.delay(20);
        }
    }
}
