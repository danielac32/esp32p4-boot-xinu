import esp32p4.FAT;
import esp32p4.HW;
import esp32p4.LVGL;
import esp32p4.Sys;

/*
 * galeria — una APP del workspace, no el workspace.
 *
 * La lanza el launcher como tarea propia (Sys.start) desde
 * /java/apps/galeria/galeria.class, y aqui dentro solo hay fotos:
 *
 *   rejilla 3x4 de miniaturas reales (/thm/tNN.jpg) -> tocar una abre la foto
 *   a pantalla completa (/img/pNN.jpg); deslizar izquierda/derecha mueve de
 *   foto o de pagina, deslizar abajo desde la foto vuelve a la rejilla y
 *   deslizar ARRIBA, desde cualquiera de las dos vistas, saca de la app al
 *   home (MSG_HIDE al launcher).
 *
 * Debajo de la rejilla van los puntos de pagina: son botones, asi que se puede
 * saltar a una pagina con el dedo ademas de deslizar.  No hay ninguna linea de
 * pista escrita ("izq der paginas / arriba salir"): se solapa con las pastillas
 * y el usuario la pidio fuera.  La unica pista que queda es el boton "atras" de
 * la vista de foto.
 *
 * El hueco de arriba (0..26) es del launcher: ahi cuelga su banner con el
 * nombre y las pastillas "min" y "X".  La app no lo ve ni lo crea; cuando el
 * launcher lo pinta, sus widgets son propiedad de otra tarea y clear() los
 * respeta.
 *
 * Con la hebra residente de LVGL (tarea "lvglui") esta app no llama nunca a
 * LVGL.tick(): el reloj y el refresco los lleva el kernel.  Lo que si hace es
 * mirar Sys.recv(), el buzon del launcher: MSG_SHOW trae la app al frente y
 * MSG_QUIT la cierra por las buenas --por su propio main retorna, y es el
 * glue de la VM quien le limpia la interfaz al morir la tarea.
 *
 * Coste medido en placa (el cronometro que lleva probar()): doce miniaturas
 * cuadradas de 96x96 salen decodificadas en 33 ms y son ~330 KB de cache.  La
 * foto a pantalla completa es otro mundo: un JPEG de 310x400 tarda 1,6 s, porque
 * la decodificacion reserva ancho x alto x 3 B en PSRAM y la franja sale por
 * SPI2.
 *
 * Doce por pagina --tres columnas por cuatro filas de 96 px-- es lo que cabe en
 * el panel de 320x480 sin que la ultima fila toque los puntos de pagina.
 *
 * Los esperar() duermen en trozos de 50 ms mirando el buzon, no de golpe.  No
 * es un detalle de gusto: Xinu admite UN mensaje sin leer por hebra y send()
 * rebate con el buzon lleno, asi que un QUIT mandado mientras la app dormia
 * sus 1,6 s de JPEG no entraba y la "X" del launcher no cerraba nada.
 *
 * Compilar:  make
 * Subir:     make send      (deja /java/apps/galeria/galeria.class)
 */
public class galeria {

    static final int BG     = 0x0E1116;
    static final int DARK   = 0x24303C;
    static final int CARD   = 0x1A222C;
    static final int TXT    = 0xE6EAF0;
    static final int MUTED  = 0x8A97A8;
    static final int ACCENT = 0x3A8DFF;

    static final int GALERIA = 1;
    static final int FOTO    = 2;

    /* Banner del launcher: 0..26.  Todo lo de la app empieza debajo.
     * Rejilla 3x4 de celdas CUADRADAS de 96x96 con 4 px de separacion
     * (x = 12, 112, 212; y = 62, 162, 262, 362).  La miniatura de la SD es de
     * 96x96 recortada por el centro (sd/Makefile), asi que llena la celda justa
     * y el boton que hay debajo solo se ve por las esquinas redondeadas.
     * Abajo, a 462 (la ultima fila termina en 458), los puntos de pagina.  En la
     * vista de foto el muelle se mueve a 436: la foto de 310x400 empieza en 28 y
     * termina en 428. */
    static final int COLS   = 3;
    static final int MAX    = 12;
    static final int NDOTS  = 12;             /* hueco maximo de puntos */
    static final int CELL   = 96;
    static final int GAP    = 4;
    static final int GX     = 12;
    static final int GY     = 62;
    static final int DOTSY  = 462;
    static final int DOTD   = 12;
    static final int DOTP   = 18;             /* paso entre puntos */
    static final int DOCKY  = 436;
    static final int NAVW   = 56;
    static final int NAVH   = 32;

    /* Glifos LV_SYMBOL_* de la Montserrat compilada (los mismos que usa el
     * launcher; ver su cabecera).  Con escape de unicode para que el fuente
     * sea ASCII: java/sdk/app.mk no le pasa -encoding al javac.  OJO: javac
     * resuelve esos escapes tambien dentro de los comentarios.
     * PREV y NEXT son las dobles flechas de foto anterior/siguiente; la
     * flecha sencilla LEFT es la de atras, asi que en el muelle los dos
     * controles nunca se confunden. */
    static final String IC_LEFT   = "\uF053";
    static final String IC_PREV   = "\uF048";
    static final String IC_NEXT   = "\uF051";

    static String[] fotos;              /* nombres en "/img", solo .jpg */
    static int total;
    static int nPags;
    static int vista;
    static int pagina;
    static int idx;
    static int scr;
    static int pagLbl;
    static int titulo;
    static int btnAnt;
    static int btnSig;
    static int btnVolver;
    static int gran;
    static int cerrarPedida;
    static int gestoMs;
    static int sinUI;         /* 1 = el launcher nos pidio soltar los widgets */
    static int pinta;         /* 1 = hay que repintar al volver al frente     */
    static int[] celda = new int[MAX];
    static int[] mini  = new int[MAX];
    static int[] punto = new int[NDOTS];      /* botones-punto de la pagina */

    /* "/img/p07.jpg" -> "/thm/t07.jpg": el numero es el indice de la foto y
     * sd/Makefile garantiza que pNN y tNN salen del mismo original. */
    static String rutaMini(String n) {
        return "/thm/t" + n.substring(1, n.length() - 4) + ".jpg";
    }

    /**
     * Deja pasar `ms` milisegundos en trozos de 50 y contesta al launcher
     * mientras espera: ver el comentario de la cabecera.  Si el mensaje era un
     * QUIT, corta la espera y deja cerrarPedida a 1 para que el pintado en
     * curso se rinda y main() retorne.
     */
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

    /** Espera (sin quedarse sorda) a que la hebra residente pinte la foto. */
    static void esperarFoto(int h) {
        int k = 0;

        /* sinUI en la condicion: si el launcher nos manda MSG_FREE en mitad de
         * la espera, el handle que miramos ya no es nuestro (su sitio de la
         * tabla lo puede ocupar otra tarea), asi que no hay nada que esperar. */
        while (k < 36 && cerrarPedida == 0 && sinUI == 0 &&
               LVGL.getWidth(h) == 0) {
            HW.delay(50);
            k++;
            if (mensajes() == 1) {
                return;
            }
        }
    }

    /** Lista /img y deja en `fotos` unicamente los .jpg. */
    static void escanear() {
        String[] l = FAT.list("/img");
        int i, n = 0;

        fotos = new String[l.length];
        for (i = 0; i < l.length; i++) {
            if (l[i].endsWith(".jpg")) {
                fotos[n] = l[i];
                n++;
            }
        }
        total = n;
        nPags = (total + MAX - 1) / MAX;
        if (nPags < 1) {
            nPags = 1;
        }
    }

    /** Cabecera común a las dos vistas: titulo y contador, bajo el banner. */
    static void cabecera(String texto, int color) {
        titulo = LVGL.label(scr, texto, 10, 30);
        LVGL.setColor(titulo, TXT, BG);
        LVGL.setFont(titulo, 26);
        pagLbl = LVGL.label(scr, "", 214, 38);
        LVGL.setColor(pagLbl, color, BG);
    }

    /** Pinta la rejilla de la pagina actual, con las miniaturas de verdad. */
    static void pintarGaleria() {
        int i, base, c, r, x, y, h, sinHandle, sinImg;

        LVGL.clear(BG);
        vista = GALERIA;
        for (i = 0; i < MAX; i++) {
            celda[i] = 0;
            mini[i] = 0;
        }
        for (i = 0; i < NDOTS; i++) {
            punto[i] = 0;
        }
        cabecera("Galeria", MUTED);
        btnAnt = 0;
        btnSig = 0;
        btnVolver = 0;
        gran = 0;
        pintarPuntos();

        /* Presupuesto de handles de esta vista: 24 de la rejilla (boton +
         * miniatura por celda) + 2 de la cabecera + hasta 12 puntos = 38 de los
         * 160 de la tabla GLOBAL (device/jvm/native_lvgl.c), compartida con el
         * launcher y con las demas apps vivas.  Con las apps minimizadas
         * soltando su UI (MSG_FREE) esto sobra; si aun asi faltan, abajo se
         * cuenta y se dice por que lado. */
        base = pagina * MAX;
        sinHandle = 0;
        sinImg = 0;
        for (i = 0; i < MAX && cerrarPedida == 0; i++) {
            if (base + i >= total) {
                break;
            }
            c = i % COLS;
            r = i / COLS;
            x = GX + c * (CELL + GAP);
            y = GY + r * (CELL + GAP);
            h = LVGL.button(scr, "", x, y, CELL, CELL);
            if (h == 0) {
                /* Sin handle: no falta la foto, falta sitio en la tabla. */
                sinHandle = MAX - i;
                System.out.println("galeria: sin handle en la celda " + i);
                break;
            }
            LVGL.setFlat(h);
            LVGL.setRadius(h, 12);
            LVGL.setColor(h, TXT, CARD);
            celda[i] = h;
            mini[i] = LVGL.img(scr, rutaMini(fotos[base + i]), x, y, 0, 0);
            if (mini[i] == 0) {
                sinImg++;
            }
        }
        LVGL.setTextFmt(pagLbl, "%d / %d", pagina + 1, nPags);
        probar(sinHandle + sinImg);
    }

    /**
     * Los puntos de pagina, centrados bajo la rejilla.  Son BOTONES, no
     * etiquetas: ademas de indicar en cual estamos dejan saltar de pagina con un
     * toque, que es lo que sustituye a la antigua linea de pista "-- paginas",
     * que se solapaba con las pastillas y el usuario la pidio fuera.
     */
    static void pintarPuntos() {
        int i, n, x0, x, b;

        n = nPags;
        if (n > NDOTS) {
            n = NDOTS;
        }
        x0 = (LVGL.screenWidth() - ((n - 1) * DOTP + DOTD)) / 2;
        for (i = 0; i < n; i++) {
            x = x0 + i * DOTP;
            b = LVGL.button(scr, "", x, DOTSY, DOTD, DOTD);
            if (b == 0) {
                System.out.println("galeria: sin handle en el punto " + i);
                break;
            }
            LVGL.setFlat(b);
            LVGL.setRadius(b, DOTD / 2);
            if (i == pagina) {
                LVGL.setColor(b, ACCENT, BG);
            } else {
                LVGL.setColor(b, DARK, BG);
            }
            punto[i] = b;
        }
    }

    /** Pastilla del muelle de la vista de foto: plana, redonda y con el texto
     *  centrado.  El ancho entra por `w` porque los controles no miden lo
     *  mismo: una flecha con 56 px y "atras" con glifo necesitan 104, y la
     *  pastilla se descuadraba cuando el texto salia cortado por el borde. */
    static int pastilla(String texto, int x, int w, int color) {
        int b = LVGL.button(scr, texto, x, DOCKY, w, NAVH);

        if (b != 0) {
            LVGL.setFlat(b);
            LVGL.setRadius(b, NAVH / 2);
            LVGL.setColor(b, color, DARK);
        }
        return b;
    }

    /**
     * Espera a que la hebra residente decodifique cada miniatura y dice CUALES
     * faltaron y por que lado, que es lo que no se veia: una miniatura con
     * handle pero sin ancho es que el JPEG no esta (ruta mal escrita o falta en
     * la SD) o que la decodificacion aun no ha terminado; una celda que ni
     * siquiera tiene handle es que la tabla global de LVGL esta llena, y eso no
     * lo arregla esperar.  Antes se esperaba un unico bloque de 500 ms y se
     * contaban anchos, asi que las dos causas salian igual en el log.
     */
    static void probar(int sinSitio) {
        int i, n = 0, sinAncho = 0, k, falta, t0;

        t0 = HW.millis();
        /* Un solo bucle para las doce: la hebra residente decodifica la pantalla
         * entera en un pase de refresco, asi que esperar cada miniatura por su
         * cuenta con un tope corto (lo que hacia el bucle anterior: 500 ms por
         * celda) hacia perder la primera de la pagina cuando el pase tardaba mas
         * de ese tope y la daba por "sin decodificar" sin serlo. */
        k = 0;
        while (k < 60 && cerrarPedida == 0 && sinUI == 0) {
            falta = 0;
            for (i = 0; i < MAX; i++) {
                if (mini[i] != 0 && LVGL.getWidth(mini[i]) == 0) {
                    falta = falta + 1;
                }
            }
            if (falta == 0) {
                break;
            }
            HW.delay(40);
            k = k + 1;
            if (mensajes() == 1) {
                return;
            }
        }
        for (i = 0; i < MAX; i++) {
            if (mini[i] == 0) {
                continue;
            }
            if (LVGL.getWidth(mini[i]) > 0) {
                n = n + 1;
            } else {
                sinAncho = sinAncho + 1;
            }
        }
        System.out.println("galeria: pag " + (pagina + 1) + "/" + nPags + ": " +
                           n + " pintadas, " + sinAncho + " sin decodificar, " +
                           sinSitio + " sin sitio  primera " +
                           LVGL.getWidth(mini[0]) + "x" +
                           LVGL.getHeight(mini[0]) + " de " + MAX + "  " +
                           (HW.millis() - t0) + " ms");
    }

    /** Pinta la foto `idx` a pantalla completa: la foto manda, y atras, el
     *  anterior/siguiente y el numero van en el muelle de abajo, cada uno en su
     *  hueco y sin solaparse. */
    static void pintarFoto() {
        String n = fotos[idx];
        int t0;

        LVGL.clear(0x000000);
        vista = FOTO;
        t0 = HW.millis();
        gran = LVGL.img(scr, "/img/" + n, 5, 28, 0, 0);
        if (gran == 0) {
            System.out.println("galeria: sin handle para /img/" + n);
        }
        /* 8..112 atras, 120..176 anterior, 184..240 siguiente, 252.. numero. */
        btnVolver = pastilla(IC_LEFT + " atras", 8, 104, TXT);
        btnAnt = pastilla(IC_PREV, 120, NAVW, ACCENT);
        btnSig = pastilla(IC_NEXT, 184, NAVW, ACCENT);
        pagLbl = LVGL.label(scr, "", 252, 444);
        LVGL.setColor(pagLbl, MUTED, 0x000000);
        LVGL.setTextFmt(pagLbl, "%d/%d", idx + 1, total);
        titulo = 0;
        esperarFoto(gran);
        System.out.println("galeria: foto " + (idx + 1) + "/" + total + " " +
                           n + " mide " + LVGL.getWidth(gran) + "x" +
                           LVGL.getHeight(gran) + "  " +
                           (HW.millis() - t0) + " ms");
    }

    static int celdaDe(int w) {
        int i;

        for (i = 0; i < MAX; i++) {
            if (w != 0 && w == celda[i]) {
                return i;
            }
        }
        return -1;
    }

    /** Que pagina pide el punto tocado, o -1 si no era ninguno. */
    static int puntoDe(int w) {
        int i;

        for (i = 0; i < NDOTS; i++) {
            if (w != 0 && w == punto[i]) {
                return i;
            }
        }
        return -1;
    }

    static void paginaAnt() {
        pagina = pagina - 1;
        if (pagina < 0) {
            pagina = nPags - 1;
        }
        pintarGaleria();
    }

    static void paginaSig() {
        pagina = pagina + 1;
        if (pagina >= nPags) {
            pagina = 0;
        }
        pintarGaleria();
    }

    static void fotoAnt() {
        idx = idx - 1;
        if (idx < 0) {
            idx = total - 1;
        }
        pintarFoto();
    }

    static void fotoSig() {
        idx = idx + 1;
        if (idx >= total) {
            idx = 0;
        }
        pintarFoto();
    }

    /** Reparte un CLICKED entre los controles de la vista activa. */
    static void evento(int w) {
        int c, p;

        if (w == 0) {
            return;
        }
        if (vista == GALERIA) {
            p = puntoDe(w);
            if (p >= 0) {
                /* Los puntos son botones: se salta a esa pagina sin deslizar. */
                if (p != pagina && p < nPags) {
                    pagina = p;
                    pintarGaleria();
                }
            } else {
                c = celdaDe(w);
                if (c >= 0 && pagina * MAX + c < total) {
                    idx = pagina * MAX + c;
                    pintarFoto();
                }
            }
        } else {
            if (w == btnVolver) {
                pagina = idx / MAX;
                pintarGaleria();
            } else if (w == btnAnt) {
                fotoAnt();
            } else if (w == btnSig) {
                fotoSig();
            }
        }
    }

    /**
     * Pide al launcher que nos mande a segundo plano: se manda MSG_HIDE al
     * padre (Sys.parent() es el tid del workspace, porque el launcher nos creo
     * con create() desde su hebra).  La app NO se sale: su pantalla sigue
     * existiendo y el launcher la trae de vuelta con MSG_SHOW.
     *
     * Se reintenta porque Xinu admite UN mensaje sin leer por hebra y el
     * launcher tiene el buzon ocupado con los avisos de muerte de las apps que
     * se cerraron (kill() manda el tid al padre): si ese aviso no se ha leido,
     * send() rebate y el gesto no hace nada.
     */
    static void aSegundoPlano() {
        int p = Sys.parent();
        int k, ok = 0;

        for (k = 0; k < 4 && ok == 0; k++) {
            ok = Sys.send(p, Sys.MSG_HIDE);
            if (ok == 0) {
                HW.delay(50);
            }
        }
        System.out.println("galeria: hide a " + p + " send " + ok);
    }

    /**
     * Swipe: ARRIBA es siempre salir al home, desde las dos vistas --es la via
     * de salida que se pidio; la casita del banner del launcher queda como
     * control secundario.  Izquierda/derecha mueven pagina o foto, y abajo
     * desde la foto vuelve a la rejilla.
     */
    static void gesto(int dir) {
        if (dir == LVGL.GEST_NONE) {
            return;
        }
        if (dir == LVGL.GEST_TOP) {
            aSegundoPlano();
            return;
        }
        if (vista == GALERIA) {
            if (dir == LVGL.GEST_LEFT) {
                paginaSig();
            } else if (dir == LVGL.GEST_RIGHT) {
                paginaAnt();
            }
        } else {
            if (dir == LVGL.GEST_LEFT) {
                fotoSig();
            } else if (dir == LVGL.GEST_RIGHT) {
                fotoAnt();
            } else if (dir == LVGL.GEST_BOTTOM) {
                pagina = idx / MAX;
                pintarGaleria();
            }
        }
    }

    /**
     * Mensajes del launcher.  MSG_FREE es el aviso de "te quedas sin
     * interfaz": clear() borra los widgets de ESTE tid y nos deja solo el
     * screen, que es lo que libera los handles que la app siguiente necesita.
     * Al MSG_SHOW se carga el screen PRIMERO --el launcher espera ver nuestra
     * pantalla activa para colgar el banner y si esperamos a repintar se le
     * pasa su minuto de espera-- y se deja marcado el repintado, que hace el
     * bucle de main: repintar aqui seria recursion, porque pintarFoto() espera
     * mirando el buzon y volveria a entrar en mensajes().
     *
     * 0 = seguir, 1 = el launcher pidio cerrar.
     */
    static int mensajes() {
        int m, i;

        m = Sys.recv();

        if (m < 0) {
            return 0;
        }
        System.out.println("galeria: msg " + m);
        if (m == Sys.MSG_SHOW) {
            LVGL.loadScreen(scr);
            if (sinUI == 1) {
                sinUI = 0;
                pinta = 1;
            }
        } else if (m == Sys.MSG_FREE) {
            sinUI = 1;
            pinta = 0;
            for (i = 0; i < MAX; i++) {
                celda[i] = 0;
                mini[i] = 0;
            }
            for (i = 0; i < NDOTS; i++) {
                punto[i] = 0;
            }
            LVGL.clear(BG);
            System.out.println("galeria: free: suelta su UI");
        } else if (m == Sys.MSG_QUIT) {
            cerrarPedida = 1;
            return 1;
        }
        return 0;
    }

    public static void main(String[] args) {
        int cod, dir, w;

        LVGL.init();
        scr = LVGL.screen();
        LVGL.clear(BG);
        escanear();
        if (total == 0) {
            LVGL.label(scr, "sin /img/*.jpg en la SD", 10, 70);
            esperar(2000);
            return;
        }
        pagina = 0;
        idx = 0;
        cerrarPedida = 0;
        sinUI = 0;
        pinta = 0;
        pintarGaleria();

        while (cerrarPedida == 0) {
            if (pinta == 1) {
                pinta = 0;
                if (vista == FOTO) {
                    pintarFoto();
                } else {
                    pintarGaleria();
                }
            }
            while (LVGL.pollEvent() == 1) {
                cod = LVGL.eventCode();
                dir = LVGL.eventGesture();
                w = LVGL.eventWidget();
                System.out.println("galeria: evt cod " + cod + " dir " + dir +
                                   " w " + w);
                if (cerrarPedida == 0) {
                    if (cod == LVGL.GESTURE) {
                        gestoMs = HW.millis();
                        gesto(dir);
                    } else if (HW.millis() - gestoMs < 400) {
                        /* LVGL dispara el gesto a mitad del arrastre y el CLICKED
                         * al soltar: sin esto, deslizar sobre una celda cambiaria
                         * de pagina Y abriria la foto de esa celda. */
                        System.out.println("galeria: clic pos-gesto ignorado");
                    } else {
                        evento(w);
                    }
                }
            }
            esperar(20);
        }
        System.out.println("galeria: cierra tid " + Sys.tid());
    }
}
