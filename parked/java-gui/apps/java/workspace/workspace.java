import esp32p4.FAT;
import esp32p4.HW;
import esp32p4.LVGL;
import esp32p4.Net;
import esp32p4.Power;
import esp32p4.Sys;

/*
 * workspace — el launcher del P4.
 *
 * Corre como tarea residente (system/main.c lo arranca con
 * create(jvmTask, ..., "/java/workspace.class")); no sale nunca, asi que la
 * consola y el dash siguen libres.
 *
 * Lo que hace es listar APPS, no ser una app: lee los subdirectorios de
 * /java/apps en la SD y pinta la pantalla del mockup que se eligio --barra de
 * estado arriba con los glifos de red y bateria, reloj grande y su linea de
 * aviso debajo, rejilla de 3x2 fichas con el logo DENTRO DE UN CIRCULO y el
 * nombre corto centrado debajo, puntos de pagina y un muelle de cuatro botones
 * circulares (casa, atras, recientes y ajustes)--.  La descripcion larga que
 * llevaban antes las fichas se quito porque LVGL no recorta ni ajusta el texto
 * y lo que sobraba se pintaba encima de la ficha de al lado.  Al tocar una
 * ficha se lanza la app como TAREA propia (Sys.start), que es lo que permite
 * minimizarla y cerrarla sin perder su estado: el launcher y la app son dos
 * hebras java con dos pantallas LVGL, y el kernel sabe de quien es cada widget
 * (device/jvm/native_lvgl.c marca el dueno de cada handle y solo deja borrar
 * el propio).
 *
 * El reloj no es la hora: la placa no tiene RTC visible desde java (no hay
 * native de reloj de pared) y HW.millis() es lo que lleva en pie el kernel, asi
 * que el numero gordo es tiempo de arranque en MM:SS.  Para un reloj de verdad
 * hace falta un native nuevo (el RTC del AXP2101 se puede leer, es de solo
 * lectura) y eso es round de refresco.
 *
 * Los textos centrados se miden, no se adivinan: LVGL no tiene alineacion de
 * label, asi que se crea el texto, se espera la pasada de refresco de la hebra
 * del panel y se le mueve la x con el ancho que devolvio getWidth (o la
 * estimacion por caracteres si aun vale 0).  Ver centrarEn().
 *
 * La tabla de handles es el presupuesto real del panel: es UNA para todas las
 * hebras java, y una app que se queda en segundo plano seguiria ocupando sus
 * 30-50 widgets hasta que otra abriera a medias.  Por eso el launcher, en
 * cuanto recupera el panel, manda MSG_FREE a la app minimizada (alFrente())
 * para que suelte su interfaz y se la vuelva a pintar ella al MSG_SHOW.
 *
 *
 * Encima de la app en frente el launcher cuelga su banner --nombre y las
 * pastillas "min" y "X"-- en la franja de 26 px que las apps respetan.  Esos
 * cuatro widgets los crea el launcher, asi que el clear() de la app no los
 * toca y sus toques le llegan solo a el.
 *
 * Cerrar es por las buenas: Sys.send(tid, MSG_QUIT) y la app retorna de su
 * main; al morir la tarea, el glue de la VM le limpia la UI (lvgl_forget_task).
 * No se usa Sys.kill(): matar a una hebra que sostiene el monitor de LVGL
 * dejaria el panel colgado para todas las demas.  Xinu solo admite un mensaje
 * sin leer por hebra (send() falla con el buzon lleno), asi que el QUIT se
 * reintenta; y el panel vuelve al launcher ANTES de esperar a la app, que es
 * como se ve que el toque ha hecho algo aunque la app tarde en morir.
 *
 * Gestos (LVGL.GESTURE, los dispara la pantalla o la tarjeta bajo el dedo al
 * arrastrar; el umbral lo pone el kernel en device/lvgl/lvglport.c):
 *
 *   app al frente  deslizar arriba   -> la app se pasa a segundo plano ella
 *                    sola, mandando MSG_HIDE al launcher (Sys.parent()); el
 *                    launcher no puede ver ese arrastre: no es suyo el widget
 *                    bajo el dedo.  Es LA via de salida: arriba desde cualquier
 *                    vista de la app, y la casita del banner queda como
 *                    control secundario.
 *   rejilla        deslizar arriba    -> tarjetero de segundo plano
 *   rejilla        izquierda/derecha -> pagina anterior/siguiente
 *   recientes      deslizar abajo     -> volver a la rejilla
 *   recientes      deslizar arriba en un tile -> cerrar esa app
 *
 * Es el mapeo de Android: arriba saca de la app, arriba en la rejilla abre el
 * tarjetero y arriba sobre una ficha lo despide.  El muelle repite los tres
 * con botones --casa, atras y recientes-- mas acceso directo a ajustes; son la
 * via secundaria, porque la salida de una app es el gesto hacia arriba.
 *
 * La direccion es la del dedo: GEST_TOP es de abajo hacia arriba.
 *
 * Iconos: los glifos LV_SYMBOL_* de LVGL viven en U+F001..U+F2ED y estan
 * compilados dentro de las dos unicas fuentes que hay (Montserrat 14 y 26,
 * lv_conf.h).  La VM copia el literal de la constant pool byte a byte
 * (jvm/constant_pool_parser.c:getUTF8String), asi que el escape de unicode de
 * un simbolo llega a LVGL como su UTF-8 de 4 bytes y se pinta el glifo.  javac
 * pliega la concatenacion de literales, por lo que un glifo seguido de texto es
 * UNA sola entrada del pool: no cuesta ranuras del heap en cada refresco.
 * (En los comentarios de este fichero no se puede escribir ese escape: javac lo
 * resuelve tambien dentro de los comentarios y revienta el compilado.)
 *
 * El tiempo de pantalla lo pone esta app al arrancar (LVGL.idleOff): dos
 * minutos sin un toque y el kernel apaga el backlight; el primer dedo lo
 * enciende y se lo traga.  Lo lleva la hebra del panel porque es la unica que
 * ve los toques de todas las apps.
 *
 * La barra de estado de arriba es del launcher: conexion (Net) y bateria
 * (Power, el PMIC de solo lectura).  Los textos de interfaz van SIN acentos:
 * la Montserrat compilada solo trae el rango ASCII 0x20-0x7F.
 *
 * Compilar:  make
 * Subir:     make send      (deja /java/workspace.class)
 */
public class workspace {
    /* Los campos MUTABLES van declarados antes que las constantes, aunque al
     * reves de como se escribe un fuente.  La VM guarda los estaticos en una
     * tabla indexada POR POSICION (jvm/simple_jvm.h: JVM_MAX_STATICS = 64), y el
     * indice que usan get/putstatic es el puesto del campo en la lista de campos
     * estaticos del .class, o sea el orden de declaracion.  javac sustituye la
     * constante compilable (`static final int BG = 0x0E1116') en cada uso y no
     * emite nunca un get/putstatic sobre ella, pero el campo sigue ocupando un
     * puesto: con las 47 constantes delante, el ultimo campo vivo caia en el
     * puesto 64 y la VM abortaba (`putstatic: campo estatico inexistente
     * btnAtras', medido en placa con el launcher remodelado).  Los 30 campos que
     * si se escriben van delante; las constantes, detras.
     *
     * MAXAPP es la unica excepcion: javac no admite la referencia hacia delante
     * en el tamano de un array, asi que tiene que estar declarada antes. */
    static final int MAXAPP  = 18;

    static String[] nombres = new String[MAXAPP];
    static int[] tids       = new int[MAXAPP];
    static int[] tarjetas   = new int[MAXAPP];
    static int[] nombresLbl = new int[MAXAPP];
    static int[] nombreX    = new int[MAXAPP];   /* la franja donde centrar   */
    static int[] nombreY    = new int[MAXAPP];

    static int scr;
    static int vistas;
    static int front;
    static int nApps;
    static int pagina;
    static int nPags;
    static int titulo;        /* el nombre de la vista, cuando no hay reloj  */
    static int relojLbl;      /* la hora gorda, arriba del todo             */
    static int subLbl;        /* su linea de abajo                          */
    static int btnHome;
    static int btnAtras;
    static int btnRec;
    static int btnSet;
    static int wifiLbl;
    static int batLbl;
    static int batPct;
    static int horaLbl;
    static int bFondo;
    static int bTitulo;
    static int bMin;
    static int bX;
    static int gestoMs;      /* millis() del ultimo gesto: tapa el CLICKED del arrastre */
    static int tickBarra;    /* millis() de la ultima pasada de PMIC y bajas */
    static int tickReloj;    /* millis() del ultimo refresco del reloj        */


    /* Paleta: fondo casi negro, tarjetas gris azulado, acento azul. */
    static final int BG     = 0x0E1116;
    static final int BAR    = 0x0B0E13;
    static final int CARD   = 0x1A222C;
    static final int DARK   = 0x24303C;
    static final int GLASS  = 0x2A3542;
    static final int TXT    = 0xE6EAF0;
    static final int MUTED  = 0x8A97A8;
    static final int ACCENT = 0x3A8DFF;
    static final int GREEN  = 0x2FBF8F;
    static final int RED    = 0xFF6B6B;
    static final int REDBG  = 0x3A2426;

    /* Geometria del panel (320x480), pasada del mockup del reloj: barra de
     * estado arriba (0..24), reloj grande (40..69) y su linea (76..90) debajo,
     * rejilla de 3 columnas con el logo EN UN CIRCULO y el nombre corto centrado
     * bajo el circulo, puntos de pagina y un muelle de botones circulares.
     *   columnas: x = GX + c*(CW+8)   (8 + 96 + 8 + 96 + 8 + 96 + 8 = 320)
     *   filas:    y = GY + r*(CH+8)   -> 108..220 y 228..340
     *   dentro de la ficha: circulo en y+8..y+80 y nombre en y+88..y+102
     *   puntos de pagina en 356..362 (fuera de las fichas: la segunda termina
     *   en 340; con la ficha de 130 px que habia antes se pintaban ENCIMA), y el
     *   muelle 400..452; quedan 28 px de reposo abajo. */
    static final int BANNERH = 26;
    static final int COLS    = 3;
    static final int PORPAG  = 6;
    static final int CW      = 96;
    static final int CH      = 112;
    static final int GX      = 8;
    static final int GY      = 108;
    static final int DIA     = 72;                    /* diametro del logo   */
    static final int NAMEY   = DIA + 16;      /* el nombre, bajo el circulo  */
    static final int DOTS_Y  = 356;
    static final int DOCKY   = 400;
    static final int DOCKD   = 52;                    /* diametro de boton   */
    static final int RELOJY  = 40;                    /* reloj grande        */
    static final int SUBY    = 76;                    /* su linea de abajo   */

    /* Tiempo de pantalla: 2 minutos sin tocar y el kernel apaga el backlight.
     * Es el launcher quien lo habilita; quien cuenta los toques es la hebra del
     * panel (device/lvgl), y el primer dedo vuelve a encender sin que ninguna
     * app lo note. */
    static final int PANTALLA_MS = 120000;

    /* Espera del bucle, en TICKS --HW.delay() es sleep(), y aqui un tick vale
     * 1 ms porque CLKTICKS_PER_SEC es 1000 (system/clkinit.c).  No puede valer
     * 0: el bucle del launcher nunca llama a resched() por su cuenta, y LVGL,
     * la shell y las apps comparten prioridad (INITPRIO), asi que sin esta
     * cesion el launcher se queda la CPU, lvglui deja de llamar a
     * lv_timer_handler() y el panel se congela.  Estaba en 20 ms; baja a 2 ms
     * para que el gesto y el arrastre se atiendan diez veces antes.
     * Medir el coste real con `syscheck' antes de volver a bajarlo. */
    static final int SPIN_MS = 2;
    static final int BARRA_MS = 5000; /* cada cuanto se repregunt PMIC y red */
    static final int RELOJ_MS = 1000; /* cada cuanto pasa el reloj del panel  */

    static final int VGRID = 1;
    static final int VREC  = 2;

    /* Glifos de LV_SYMBOL_* que trae la Montserrat compilada (ver la cabecera).
     * Cada uno es un char del rango U+F000..U+F2FF; se escriben con escape de
     * unicode para que el fuente siga siendo ASCII y no dependa del juego de
     * caracteres con el que javac lea el fichero (java/sdk/app.mk no le pasa
     * -encoding).  Ojo: javac resuelve esos escapes tambien DENTRO de los
     * comentarios, asi que aqui se escriben sin la barra.
     * Codigo comun:  wifi F1EB  left F053  right F054  up F077  down F078
     *                home F015  list F00B  close F00D  settings F013
     *                image F03E audio F001 eye-open F06E play F04B pause F04C
     *  Lo demas que usa este rediseño, contra la lista de glifos compilada en
     *  lv_font_montserrat_14.c y _26.c (las dos unicas fuentes del panel, con
     *  los mismos 56 simbolos):  copy F0C5 (los dos cuadrados de "recientes")
     *  battery-full F240  battery-3 F241  battery-2 F242  battery-1 F243
     *  battery-empty F244  charge F0E7  bars F0C9                                        */
    static final String IC_WIFI   = "\uF1EB";
    static final String IC_LEFT   = "\uF053";
    static final String IC_RIGHT  = "\uF054";
    static final String IC_UP     = "\uF077";
    static final String IC_DOWN   = "\uF078";
    static final String IC_HOME   = "\uF015";
    static final String IC_LIST   = "\uF00B";
    static final String IC_CLOSE  = "\uF00D";
    static final String IC_SET    = "\uF013";
    static final String IC_REC    = "\uF0C5";   /* recientes: dos cuadrados */
    static final String IC_CHARGE = "\uF0E7";   /* rayo, si esta cargando   */
    static final String IC_BAT    = "\uF240";   /* bateria llena            */
    static final String IC_BAT3   = "\uF241";
    static final String IC_BAT2   = "\uF242";
    static final String IC_BAT1   = "\uF243";
    static final String IC_BAT0   = "\uF244";

    /* Icono de cada app, por nombre de directorio.  Sin coincidencia se usa
     * IC_HOME, para que ninguna tarjeta salga sin su cuadro. */
    static String iconoDe(String n) {
        if (n.equals("info")) {
            return IC_LIST;
        }
        if (n.equals("monitor")) {
            return "\uF06E";
        }
        if (n.equals("galeria")) {
            return "\uF03E";
        }
        if (n.equals("ajustes")) {
            return "\uF013";
        }
        if (n.equals("musica")) {
            return "\uF001";
        }
        return IC_HOME;
    }

    /** Un subdirectorio de /java/apps es una app; su nombre, su unico texto. */
    static void escanearApps() {
        String[] l = FAT.list("/java/apps");
        int i, n = 0;

        for (i = 0; i < l.length; i++) {
            if (n >= MAXAPP) {
                break;
            }
            if (FAT.isDirectory("/java/apps/" + l[i]) == 0) {
                continue;
            }
            nombres[n] = l[i];
            tids[n] = 0;
            n++;
        }
        nApps = n;
        nPags = (n + PORPAG - 1) / PORPAG;
        if (nPags < 1) {
            nPags = 1;
        }
        pagina = 0;
        System.out.println("workspace: " + n + " apps en /java/apps");
    }

    /**
     * La linea de estado del reloj.  En el mockup era la fecha; aqui es el
     * aviso del launcher ("galeria: en segundo plano", "musica: no arranca") y,
     * sin aviso, vuelve a "desde el arranque".
     */
    static void setHint(String s) {
        if (subLbl == 0) {
            return;
        }
        if (s.length() == 0) {
            s = "desde el arranque";
        }
        LVGL.setText(subLbl, s);
        centrarEn(subLbl, s, 0, SUBY, 320, 7);
    }

    /**
     * Centra un texto dentro de una franja.  El ancho real lo da la hebra del
     * panel (LVGL.getWidth), pero justo despues de crear el label aun puede
     * valer 0: entonces se estima por caracteres (Montserrat 14 manda ~7 px por
     * letra y la de 26 ~13).  Un pixel de error no se ve.
     */
    static void centrarEn(int lbl, String s, int x0, int y, int ancho,
                          int pxChar) {
        int w = LVGL.getWidth(lbl);

        if (w <= 8) {
            w = s.length() * pxChar;
        }
        LVGL.setPos(lbl, x0 + (ancho - w) / 2, y);
    }

    /**
     * Barra de estado (0..24), como en el mockup: glifo de red, glifo de
     * bateria, porcentaje y hora, todo a la derecha.  La bateria es un glifo
     * que cambia (llena, 3/4, media, 1/4, vacia, y rayo si esta cargando) en
     * vez de una barrita dibujada: cuesta un setText y no dos setSize.
     */
    static void pintarBarra() {
        LVGL.rect(scr, 0, 0, 320, 24, BAR);
        wifiLbl = LVGL.label(scr, IC_WIFI, 176, 4);
        LVGL.setFont(wifiLbl, 14);
        batLbl = LVGL.label(scr, IC_BAT, 204, 4);
        LVGL.setFont(batLbl, 14);
        batPct = LVGL.label(scr, "--%", 228, 4);
        LVGL.setFont(batPct, 14);
        LVGL.setColor(batPct, TXT, BAR);
        horaLbl = LVGL.label(scr, "00:00", 272, 4);
        LVGL.setFont(horaLbl, 14);
        LVGL.setColor(horaLbl, TXT, BAR);
        refrescarBarra();
    }

    /** El reloj gordo del mockup: aqui es lo que lleva en pie la placa. */
    static void pintarReloj() {
        relojLbl = LVGL.label(scr, "00:00", 120, RELOJY);
        LVGL.setFont(relojLbl, 26);
        LVGL.setColor(relojLbl, TXT, BG);
        subLbl = LVGL.label(scr, "desde el arranque", 90, SUBY);
        LVGL.setFont(subLbl, 14);
        LVGL.setColor(subLbl, MUTED, BG);
    }

    /** "MM:SS" con enteros: la VM no tiene coma flotante ni sprintf. */
    static String reloj(int m, int s) {
        String s2 = "" + s;

        if (s < 10) {
            s2 = "0" + s2;
        }
        return m + ":" + s2;
    }

    static void refrescarBarra() {
        refrescarPMIC();
        refrescarReloj();
    }

    /** Vuelve a preguntar PMIC y red; se llama cada ~5 s, no cada frame. */
    static void refrescarPMIC() {
        int p, c;
        String g;

        if (batLbl == 0) {
            return;
        }
        p = Power.percent();
        c = Power.charge();
        if (p < 0) {
            p = 0;
        }
        if (p > 100) {
            p = 100;
        }
        if (c == 2) {
            g = IC_CHARGE;
        } else if (p >= 80) {
            g = IC_BAT;
        } else if (p >= 60) {
            g = IC_BAT3;
        } else if (p >= 40) {
            g = IC_BAT2;
        } else if (p >= 20) {
            g = IC_BAT1;
        } else {
            g = IC_BAT0;
        }
        LVGL.setText(batLbl, g);
        if (c == 2) {
            LVGL.setColor(batLbl, ACCENT, BAR);
        } else if (p <= 15) {
            LVGL.setColor(batLbl, RED, BAR);
        } else {
            LVGL.setColor(batLbl, GREEN, BAR);
        }
        LVGL.setTextFmt(batPct, "%d%%", p, 0);
        if (Net.isConnected() == 1) {
            LVGL.setColor(wifiLbl, GREEN, BAR);
        } else {
            LVGL.setColor(wifiLbl, MUTED, BAR);
        }
    }

    /** Los dos relojes dicen lo mismo: el gordo del panel y el de la barra. */
    static void refrescarReloj() {
        int s = HW.millis() / 1000;
        String t = reloj(s / 60, s % 60);

        if (horaLbl != 0) {
            LVGL.setText(horaLbl, t);
        }
        if (relojLbl != 0) {
            LVGL.setText(relojLbl, t);
            /* Se vuelve a centrar en cada pasada: con los minutos de dos o tres
             * digitos el ancho cambia y el reloj se iría a la izquierda. */
            centrarEn(relojLbl, t, 0, RELOJY, 320, 13);
        }
    }

    /** Pastilla: boton plano y redondo, el aspecto Android de los controles. */
    static int pastilla(int parent, String texto, int x, int y, int w, int h,
                       int fg, int bg) {
        int b = LVGL.button(parent, texto, x, y, w, h);

        if (b != 0) {
            LVGL.setFlat(b);
            LVGL.setRadius(b, h / 2);
            LVGL.setColor(b, fg, bg);
        }
        return b;
    }

    /** Ficha del mockup: circulo con el logo y el nombre corto debajo.
     *  Cuesta 4 handles de los 160 de la tabla global (native_lvgl.c): el boton
     *  que se toca, el circulo, el glifo y el nombre.  La descripcion de una
     *  linea que habia antes se quito porque no cabia sin que LVGL recortara. */
    static void tarjeta(int parent, int i, int x, int y) {
        int b, ic, lbl, cx, cy;

        b = LVGL.button(parent, "", x, y, CW, CH);

        if (b == 0) {
            System.out.println("workspace: sin handle para " + nombres[i]);
            return;
        }
        LVGL.setFlat(b);
        LVGL.setRadius(b, 20);
        LVGL.setColor(b, TXT, CARD);
        tarjetas[i] = b;

        cx = x + (CW - DIA) / 2;
        cy = y + 8;
        ic = LVGL.rect(parent, cx, cy, DIA, DIA, DARK);
        LVGL.setRadius(ic, DIA / 2);
        /* El glifo de Montserrat 26 mide 29 de alto y unos 26 de ancho: con el
         * circulo de 72 quedan 23 px por la izquierda y 21 por arriba. */
        lbl = LVGL.label(parent, iconoDe(nombres[i]), cx + 23, cy + 21);
        LVGL.setFont(lbl, 26);
        LVGL.setColor(lbl, ACCENT, DARK);

        lbl = LVGL.label(parent, nombres[i], x + 4, y + NAMEY);
        LVGL.setFont(lbl, 14);
        LVGL.setColor(lbl, TXT, CARD);
        nombresLbl[i] = lbl;
        nombreX[i] = x;
        nombreY[i] = y + NAMEY;
    }

    /** Boton circular del muelle: 52 px con el glifo de 26 centrado dentro. */
    static int boton(String glifo, int x) {
        int b = LVGL.button(scr, "", x, DOCKY, DOCKD, DOCKD);
        int l;

        if (b == 0) {
            return 0;
        }
        LVGL.setFlat(b);
        LVGL.setRadius(b, DOCKD / 2);
        LVGL.setColor(b, TXT, DARK);
        l = LVGL.label(scr, glifo, x + 13, DOCKY + 11);
        LVGL.setFont(l, 26);
        LVGL.setColor(l, ACCENT, DARK);
        return b;
    }

    /**
     * Muelle del mockup: casa, atras, recientes y ajustes, los cuatro en
     * circulo, y encima los puntos de pagina.  No hay flechas de paginacion --
     * en la rejilla se pasa de pagina deslizando a los lados, que es como se
     * pidio-- y el cuarto boton es acceso directo a Ajustes, que es donde vive
     * la lista de redes.
     */
    static void pintarDock() {
        int i, d, w, color, dx;

        btnHome  = boton(IC_HOME, 14);
        btnAtras = boton(IC_LEFT, 94);
        btnRec   = boton(IC_REC, 174);
        btnSet   = boton(IC_SET, 254);

        dx = 160 - (nPags * 14) / 2;
        for (i = 0; i < nPags; i++) {
            if (i == pagina) {
                w = 14;
                color = ACCENT;
            } else {
                w = 6;
                color = GLASS;
            }
            d = LVGL.rect(scr, dx + i * 14 + (14 - w) / 2, DOTS_Y, w, 6, color);
            LVGL.setRadius(d, 3);
        }
    }

    /** Centra los nombres de las fichas pintadas: hay que medirlos ya creados.
     *  Se recorre la tabla de handles, no la pagina, porque el tarjetero pinta
     *  las mismas fichas en otro orden. */
    static void centrarNombres() {
        int i;

        for (i = 0; i < MAXAPP; i++) {
            if (nombresLbl[i] != 0) {
                centrarEn(nombresLbl[i], nombres[i], nombreX[i], nombreY[i],
                          CW, 7);
            }
        }
    }

    /** Rejilla de apps disponibles: el launcher en primer plano. */
    static void pintarGrid() {
        int i, c, r, x, y, base, n;

        LVGL.clear(BG);
        vistas = VGRID;
        front = -1;
        for (i = 0; i < MAXAPP; i++) {
            tarjetas[i] = 0;
            nombresLbl[i] = 0;
        }
        btnHome = 0;
        btnAtras = 0;
        btnRec = 0;
        btnSet = 0;
        titulo = 0;
        subLbl = 0;
        relojLbl = 0;
        pintarBarra();
        pintarReloj();

        base = pagina * PORPAG;
        n = 0;
        for (i = base; i < nApps && i < base + PORPAG; i++) {
            c = n % COLS;
            r = n / COLS;
            x = GX + c * (CW + 8);
            y = GY + r * (CH + 8);
            tarjeta(scr, i, x, y);
            n++;
        }
        if (nApps == 0) {
            LVGL.label(scr, "sin apps en /java/apps", 10, GY);
        }
        pintarDock();
        /* Los anchors de LVGL dan la medida del texto en la pasada de refresco:
         * sin esta espera los nombres saldrian estimados, no medidos. */
        HW.delay(150);
        centrarNombres();
    }

    /** Tarjetero de las apps que siguen vivas en segundo plano. */
    static void pintarRecientes() {
        int i, c, r, x, y, n;

        LVGL.clear(BG);
        vistas = VREC;
        for (i = 0; i < MAXAPP; i++) {
            tarjetas[i] = 0;
            nombresLbl[i] = 0;
        }
        btnHome = 0;
        btnAtras = 0;
        btnRec = 0;
        btnSet = 0;
        subLbl = 0;
        relojLbl = 0;
        pintarBarra();
        titulo = LVGL.label(scr, "Segundo plano", 10, 34);
        LVGL.setColor(titulo, TXT, BG);
        LVGL.setFont(titulo, 26);

        n = 0;
        for (i = 0; i < nApps; i++) {
            if (n >= PORPAG) {
                break;
            }
            if (tids[i] != 0 && Sys.alive(tids[i]) == 1) {
                c = n % COLS;
                r = n / COLS;
                x = GX + c * (CW + 8);
                y = GY + r * (CH + 8);
                tarjeta(scr, i, x, y);
                n++;
            }
        }
        if (n == 0) {
            LVGL.label(scr, "ninguna app abierta", 10, GY);
        }
        /* La linea bajo el titulo hace de aviso y de ayuda de gestos a la vez:
         * es la misma subLbl que en la rejilla lleva el reloj. */
        subLbl = LVGL.label(scr, IC_DOWN + " rejilla   " + IC_UP + " cierra tarjeta",
                            10, SUBY);
        LVGL.setFont(subLbl, 14);
        LVGL.setColor(subLbl, MUTED, BG);
        pintarDock();
        HW.delay(150);
        centrarNombres();
    }

    /** Que app ocupa la tarjeta pulsada, o -1. */
    static int appDe(int w) {
        int i;

        if (w == 0) {
            return -1;
        }
        for (i = 0; i < MAXAPP; i++) {
            if (w != 0 && w == tarjetas[i]) {
                return i;
            }
        }
        return -1;
    }

    /** Tira los widgets del banner; son del launcher, no de la app. */
    static void quitarBanner() {
        if (bFondo != 0) {
            LVGL.del(bFondo);
            bFondo = 0;
        }
        if (bTitulo != 0) {
            LVGL.del(bTitulo);
            bTitulo = 0;
        }
        if (bMin != 0) {
            LVGL.del(bMin);
            bMin = 0;
        }
        if (bX != 0) {
            LVGL.del(bX);
            bX = 0;
        }
    }

    /** Cuelga el banner sobre la pantalla que la app acabo de cargar. */
    static void ponerBanner(int i) {
        int s = LVGL.activeScreen();

        if (s == 0 || s == scr) {
            return;
        }
        bFondo = LVGL.rect(s, 0, 0, 320, BANNERH, BAR);
        bTitulo = LVGL.label(s, nombres[i], 12, 5);
        LVGL.setColor(bTitulo, TXT, BAR);
        /* Los dos controles van solo con glifo, centrados por el propio boton:
         * casa = al segundo plano, cruz = cierre cooperativo. */
        bMin = pastilla(s, IC_HOME, 236, 2, 38, 22, MUTED, DARK);
        bX = pastilla(s, IC_CLOSE, 278, 2, 38, 22, RED, REDBG);
    }

    /** Espera a que otra tarea haya puesto su pantalla en el panel. */
    static int pantallaDeLaApp(int intentos) {
        int k, s;

        for (k = 0; k < intentos; k++) {
            HW.delay(50);
            s = LVGL.activeScreen();
            if (s != 0 && s != scr) {
                return s;
            }
        }
        return 0;
    }

    /** Lanza la app `i`, o la trae al frente si ya estaba viva. */
    static void abrir(int i) {
        if (i < 0 || nombres[i] == null) {
            return;
        }
        quitarBanner();
        front = i;
        if (tids[i] != 0 && Sys.alive(tids[i]) == 1) {
            enviar(i, Sys.MSG_SHOW);
            pantallaDeLaApp(20);
            ponerBanner(i);
            System.out.println("workspace: show " + nombres[i]);
            return;
        }
        tids[i] = Sys.start("/java/apps/" + nombres[i] + "/" + nombres[i]
                            + ".class");
        if (tids[i] == 0) {
            front = -1;
            setHint(nombres[i] + ": no arranca");
            return;
        }
        /* La app crea y carga su propia pantalla; hasta entonces el banner no
         * tiene donde ir.  El arranque del JVM puede tardar un par de segundos
         * (leer el .class de la SD + reservar el estado en PSRAM). */
        if (pantallaDeLaApp(200) == 0) {
            front = -1;
            LVGL.loadScreen(scr);
            setHint(nombres[i] + ": sin pantalla");
            System.out.println("workspace: " + nombres[i] + " tid " + tids[i]
                               + " no cargo pantalla");
            return;
        }
        ponerBanner(i);
        System.out.println("workspace: abre " + nombres[i] + " tid " + tids[i]);
    }

    /**
     * Manda `codigo` a la app `i`.  Xinu admite un solo mensaje sin leer por
     * hebra y send() falla con el buzon lleno, asi que se reintenta.  Antes el
     * reintento era corto (30 x 50 ms) y perdia el QUIT si la app estaba en
     * mitad de un JPEG: ahora las apps miran el buzon cada 50 ms, de modo que
     * el mensaje entra a la primera; se mantiene paciente por si acaso.
     * Devuelve 1 si el mensaje quedo depositado.
     */
    static int enviar(int i, int codigo) {
        int k, t, ok;

        t = tids[i];
        ok = 0;
        for (k = 0; k < 60 && ok == 0; k++) {
            if (Sys.alive(t) == 0) {
                return 0;
            }
            ok = Sys.send(t, codigo);
            if (ok == 0) {
                HW.delay(50);
            }
        }
        return ok;
    }

    /** Pide cerrar a la app `i` y espera a que se haya ido ella sola. */
    static void cerrar(int i) {
        int k, t, ok, viva;

        if (i < 0 || tids[i] == 0) {
            return;
        }
        t = tids[i];
        /* El panel vuelve al launcher primero: el dedo ve que su toque ha
         * llegado aunque la app tarde en morir. */
        quitarBanner();
        front = -1;
        LVGL.loadScreen(scr);
        ok = enviar(i, Sys.MSG_QUIT);
        k = 0;
        while (k < 60 && Sys.alive(t) == 1) {
            HW.delay(50);
            k++;
        }
        viva = Sys.alive(t);
        System.out.println("workspace: quita " + nombres[i] + " tid " + t +
                           " send " + ok + " espera " + k + " viva " + viva);
        if (viva == 1) {
            setHint(nombres[i] + ": no cierra  send=" + ok);
            return;
        }
        tids[i] = 0;
    }

    /**
     * Olvida los tid de las apps que ya no existen.  Una app que se cerro por
     * su cuenta (o que revienta) deja su hueco en `tids`, y el tarjetero la
     * seguira mostrando viva hasta la pasada siguiente; ademas el launcher deja
     * de mandar mensajes a un tid que el kernel ya pudo prestar a otra hebra.
     */
    static void barrerBajas() {
        int i;

        for (i = 0; i < nApps; i++) {
            if (tids[i] != 0 && Sys.alive(tids[i]) == 0) {
                System.out.println("workspace: baja " + nombres[i] + " tid " +
                                   tids[i]);
                tids[i] = 0;
                if (front == i) {
                    quitarBanner();
                    front = -1;
                    LVGL.loadScreen(scr);
                    setHint(nombres[i] + ": se cerro sola");
                }
            }
        }
    }

    /**
     * Devuelve el panel al launcher, con la app intacta en segundo plano.
     *
     * "Intacta" es de su estado, no de su interfaz: en cuanto el panel es
     * nuestro le mandamos MSG_FREE para que suelte los widgets.  La tabla de
     * handles es UNA para el launcher y todas las apps (WIDGET_MAX en
     * device/jvm/native_lvgl.c) y clear() solo borra los de la tarea que llama,
     * asi que cuatro apps minimizadas se guardaban sus 30-50 widgets cada una y
     * la siguiente abria a medias: la galeria cortaba su rejilla en la celda 4
     * porque LVGL.button() ya no tenia donde apuntarse.  La app conserva su
     * screen (clear() respeta el KIND_SCREEN) y se repinta sola al MSG_SHOW.
     */
    static void alFrente() {
        String n = "";
        int i = front;

        if (i >= 0) {
            n = nombres[i];
        }
        quitarBanner();
        front = -1;
        LVGL.loadScreen(scr);
        if (i >= 0 && tids[i] != 0) {
            freeUI(tids[i]);
        }
        setHint(n + ": en segundo plano");
        System.out.println("workspace: minimiza " + n);
    }

    /**
     * Deja MSG_FREE en el buzon de la app, sin bloquear el bucle del launcher.
     * Xinu admite UN mensaje sin leer por hebra, y el buzon puede estar ocupado
     * por el propio MSG_HIDE que venimos de leer; reintentar corto (5 x 20 ms)
     * es imperceptible aqui, pero NO usar enviar(), que llega a 3 s y congela
     * el panel.  Si aun asi no entra, la app se queda con su UI: se ve igual,
     * solo ocupa handles que otra app precisara.
     */
    static void freeUI(int t) {
        int k, ok = 0;

        for (k = 0; k < 5 && ok == 0; k++) {
            if (Sys.alive(t) == 0) {
                return;
            }
            ok = Sys.send(t, Sys.MSG_FREE);
            if (ok == 0) {
                HW.delay(20);
            }
        }
        System.out.println("workspace: free tid " + t + " send " + ok);
    }

    /** Toques mientras una app esta en frente: solo el banner es nuestro. */
    static void eventoBanner(int w) {
        if (w == 0) {
            return;
        }
        if (w == bMin) {
            alFrente();
        } else if (w == bX) {
            cerrar(front);
        }
    }

    /** Toques del launcher en sus propias vistas: el muelle manda primero. */
    static void eventoClic(int w) {
        int i;

        if (w == 0) {
            return;
        }
        if (w == btnHome) {
            pagina = 0;
            pintarGrid();
            return;
        }
        if (w == btnAtras) {
            atras();
            return;
        }
        if (w == btnRec) {
            pintarRecientes();
            return;
        }
        if (w == btnSet) {
            i = appDeNombre("ajustes");
            if (i >= 0) {
                abrir(i);
            } else {
                setHint("no hay app ajustes");
            }
            return;
        }
        i = appDe(w);
        if (i >= 0) {
            abrir(i);
        }
    }

    /**
     * El boton de atras del muelle, que es el que falta en el esquema de
     * gestos: de recientes devuelve a la rejilla, y con una app al frente la
     * pasa a segundo plano --el mismo paso que deslizar hacia arriba encima de
     * ella, que sigue siendo LA via de salida--.  Sin app y sin tarjetero,
     * atras es la pagina anterior de la rejilla.
     */
    static void atras() {
        if (front >= 0) {
            alFrente();
            return;
        }
        if (vistas == VREC) {
            pintarGrid();
            return;
        }
        paginar(-1);
    }

    /** Indice de una app por nombre de directorio, o -1. */
    static int appDeNombre(String n) {
        int i;

        for (i = 0; i < nApps; i++) {
            if (nombres[i] != null && nombres[i].equals(n)) {
                return i;
            }
        }
        return -1;
    }

    /** Vuelve una pagina en la rejilla, con envoltura. */
    static void paginar(int paso) {
        pagina = pagina + paso;
        if (pagina < 0) {
            pagina = nPags - 1;
        }
        if (pagina >= nPags) {
            pagina = 0;
        }
        pintarGrid();
    }

    /**
     * Gestos del launcher, mapeados como en Android: arriba entra en el
     * tarjetero de segundo plano y abajo vuelve a la rejilla; en el tarjetero,
     * arrastrar una tarjeta hacia arriba la despide (cierra su app).  En la
     * rejilla el arrastre lateral pasa de pagina.
     */
    static void eventoGesto(int w, int dir) {
        int i;

        if (vistas == VGRID) {
            if (dir == LVGL.GEST_TOP) {
                pintarRecientes();
            } else if (dir == LVGL.GEST_LEFT) {
                paginar(1);
            } else if (dir == LVGL.GEST_RIGHT) {
                paginar(-1);
            }
            return;
        }
        if (dir == LVGL.GEST_BOTTOM) {
            pintarGrid();
            return;
        }
        if (dir != LVGL.GEST_TOP) {
            return;
        }
        i = appDe(w);
        if (i >= 0) {
            /* Se cierra y se redibuja el tarjetero: la tarjeta que se quita es
             * la que estaba bajo el dedo.  Fuera de una tarjeta el gesto no
             * hace nada, para no cerrar nada por accidente. */
            cerrar(i);
            pintarRecientes();
        }
    }

    /**
     * Mensajes de las apps.  Solo se entiende MSG_HIDE, el "paso a segundo
     * plano" que manda la app al padre (Sys.parent()) cuando alguien desliza
     * hacia arriba encima de ella.  Se ignora si llega de una app que ya no
     * esta en frente: el hueco de Xinu es de un solo mensaje y no hay emisor
     * en el mensaje, asi que el unico que puede pedir esto es el que tiene el
     * panel.
     */
    static void mensajes() {
        int m;

        m = Sys.recv();
        if (m < 0) {
            return;
        }
        System.out.println("workspace: msg " + m + " front " + front);
        if (m == Sys.MSG_HIDE && front >= 0) {
            alFrente();
        }
    }

    public static void main(String[] args) {
        int cod, dir, w, ahora;

        LVGL.init();
        scr = LVGL.screen();
        LVGL.idleOff(PANTALLA_MS);
        front = -1;
        escanearApps();
        pintarGrid();
        tickBarra = HW.millis();
        tickReloj = tickBarra;

        while (true) {
            mensajes();
            while (LVGL.pollEvent() == 1) {
                cod = LVGL.eventCode();
                dir = LVGL.eventGesture();
                w = LVGL.eventWidget();
                System.out.println("workspace: evt cod " + cod + " dir " + dir +
                                   " w " + w + " front " + front);
                if (front >= 0) {
                    eventoBanner(w);
                } else if (cod == LVGL.GESTURE) {
                    gestoMs = HW.millis();
                    eventoGesto(w, dir);
                } else if (HW.millis() - gestoMs < 400) {
                    /* El gesto salta a mitad del arrastre y el CLICKED al soltar:
                     * sin esta ventana, deslizar sobre una tarjeta pagaria de
                     * pagina y ABRIRIA la app de esa tarjeta. */
                    System.out.println("workspace: clic pos-gesto ignorado");
                } else {
                    eventoClic(w);
                }
            }
            ahora = HW.millis();
            /* Dos cadencias: el reloj pasa cada segundo, y el PMIC y la red se
             * vuelven a preguntar cada 5 s (el AXP2101 comparte el I2C0 con el
             * tactil, no conviene martillearlo). */
            if (ahora - tickReloj >= RELOJ_MS) {
                tickReloj = ahora;
                refrescarReloj();
            }
            if (ahora - tickBarra >= BARRA_MS) {
                tickBarra = ahora;
                refrescarPMIC();
                barrerBajas();
            }
            HW.delay(SPIN_MS);
        }
    }
}
