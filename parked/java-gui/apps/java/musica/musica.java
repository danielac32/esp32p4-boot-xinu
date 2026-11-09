import esp32p4.FAT;
import esp32p4.HW;
import esp32p4.LVGL;
import esp32p4.Sys;

/*
 * musica -- maqueta de reproductor.
 *
 * Segunda de las apps de maquetacion (con ajustes): sirve para ver el rediseño
 * aplicado a una pantalla de otro genero, tarjeta de "suena ahora", barra de
 * progreso, controles de pastilla y ecualizador animado, sin tocar el audio de
 * verdad. La portada si es real: LVGL.img() sobre /thm/tNN.jpg, el JPEG de
 * 96x128 que ya usa la galeria (~80 ms de decodificacion, no los 1,6 s de una
 * foto a pantalla completa).
 *
 * El bucle no llama nunca a LVGL.tick(): la hebra residente "lvglui" del kernel
 * mueve el reloj y el refresco. Aqui el bucle anima las 12 barras del
 * ecualizador --cambian de alto y de color, nunca de widget-- y avanza el
 * cronometro de la pista.
 *
 * Como en galeria, esperar() duerme en trozos de 50 ms mirando el buzon: Xinu
 * admite un mensaje sin leer por hebra y si el QUIT del launcher rebota con el
 * buzon lleno, la "X" del banner no cierra nada.
 *
 * Franja 0..26: el banner del launcher (nombre + "min" + "X").
 * Texto SIN acentos: la Montserrat compilada solo trae ASCII 0x20-0x7F.
 *
 * Compilar:  make
 * Subir:     make send
 */
public class musica {

    static final int BG     = 0x0E1116;
    static final int CARD   = 0x1A222C;
    static final int DARK   = 0x24303C;
    static final int GLASS  = 0x2A3542;
    static final int TXT    = 0xE6EAF0;
    static final int MUTED  = 0x8A97A8;
    static final int ACCENT = 0x3A8DFF;

    /* Glifos LV_SYMBOL_* (lv_symbol_def.h).  El source sigue siendo ASCII y el
     * escape de unicode lo resuelve javac; por eso los comentarios NO llevan
     * la barra-u, que tambien la interpretaria el lexer. */
    static final String IC_LEFT  = "\uF053";   /* LV_SYMBOL_LEFT  */
    static final String IC_RIGHT = "\uF054";   /* LV_SYMBOL_RIGHT */
    static final String IC_UP    = "\uF077";   /* LV_SYMBOL_UP    */

    static final int NPISTAS = 4;
    static final int NBARRAS = 12;
    static final int BARW    = 18;
    static final int BARX    = 22;
    static final int BARGAP  = 23;
    static final int BARY    = 356;   /* linea de suelo de las barras */
    static final int BARMAX  = 58;

    /* Nombres de adorno; la portada es un JPEG de la SD. */
    static String[] nombres  = { "Tema norte", "Rio abajo", "Tarde lenta",
                                 "Otra vez" };
    static String[] artistas = { "maqueta", "maqueta", "maqueta", "maqueta" };
    static int[] dur         = { 182, 245, 96, 320 };

    static int scr;
    static int pista;
    static int seg;
    static int suena;
    static int nivel;
    static int cuadro;
    static int cover;
    static int lblPista;
    static int lblArtista;
    static int lblNum;
    static int relleno;
    static int lblTiempo;
    static int lblSigue;
    static int btnPrev;
    static int btnPlay;
    static int btnNext;
    static int btnMas;
    static int btnMenos;
    static int volRelleno;
    static int cerrarPedida;
    static int gestoMs;
    static int sinUI;         /* 1 = el launcher nos pidio soltar los widgets */
    static int pinta;         /* 1 = hay que repintar al volver al frente     */
    static int[] barras = new int[NBARRAS];

    /** Portada de la pista: t01..t04, las miniaturas que ya estan en la SD. */
    static String portada(int i) {
        String n = "/thm/t0" + (i + 1) + ".jpg";

        if (FAT.exists(n) == 1) {
            return n;
        }
        return "";
    }

    /** "3:07" con enteros: la VM no tiene coma flotante ni sprintf. */
    static String tiempo(int s) {
        String s2 = "" + (s % 60);

        if (s % 60 < 10) {
            s2 = "0" + s2;
        }
        return (s / 60) + ":" + s2;
    }

    /**
     * Mensajes del launcher.  Con MSG_FREE soltamos los widgets --la tabla de
     * handles de LVGL es unica para el launcher y para todas las apps, y una
     * app minimizada que los guardaba dejaba sin sitio a la siguiente-- y con
     * MSG_SHOW volvemos al frente y pedimos el repintado.  Pista, volumen y si
     * suena viven en los estaticos, asi que la maqueta vuelve donde estaba.
     *
     * 0 = seguir, 1 = el launcher pidio cerrar.
     */
    static int mensajes() {
        int m = Sys.recv();

        if (m < 0) {
            return cerrarPedida;
        }
        System.out.println("musica: msg " + m);
        if (m == Sys.MSG_SHOW) {
            LVGL.loadScreen(scr);
            if (sinUI == 1) {
                sinUI = 0;
                pinta = 1;
            }
        } else if (m == Sys.MSG_FREE) {
            sinUI = 1;
            pinta = 0;
            cover = 0;
            LVGL.clear(BG);
            System.out.println("musica: free: suelta su UI");
        } else if (m == Sys.MSG_QUIT) {
            cerrarPedida = 1;
        }
        return cerrarPedida;
    }

    /** Espera ms en trozos de 50, contestando al launcher mientras duerme. */
    static int esperar(int ms) {
        int slept = 0;
        int slice;

        while (slept < ms && cerrarPedida == 0) {
            slice = ms - slept;
            if (slice > 50) {
                slice = 50;
            }
            HW.delay(slice);
            slept = slept + slice;
            mensajes();
        }
        return cerrarPedida;
    }

    /** Tarjeta plana y redonda; los textos van ENCIMA (un rect no es clicable). */
    static void tarjeta(int y, int h) {
        LVGL.setRadius(LVGL.rect(scr, 10, y, 300, h, CARD), 16);
    }

    static int pastilla(String texto, int x, int y, int w, int h, int fg) {
        int b = LVGL.button(scr, texto, x, y, w, h);

        if (b != 0) {
            LVGL.setFlat(b);
            LVGL.setRadius(b, h / 2);
            LVGL.setColor(b, fg, DARK);
        }
        return b;
    }

    /** Texto suelto; cuerpo es 14 o 26 (unicas Montserrat compiladas). */
    static int texto(String s, int x, int y, int color, int cuerpo) {
        int l = LVGL.label(scr, s, x, y);

        LVGL.setColor(l, color, BG);
        LVGL.setFont(l, cuerpo);
        return l;
    }

    /** Deja pasar el refresco del JPEG: la portada la pinta la hebra residente. */
    static void esperarPortada() {
        int n = 0;

        while (n < 20 && cerrarPedida == 0 && cover != 0 &&
               LVGL.getWidth(cover) == 0) {
            HW.delay(50);
            n = n + 1;
            mensajes();
        }
    }

    /** Pinta la pista actual: portada nueva, textos y cronometro a cero. */
    static void cargarPista() {
        String ruta = portada(pista);

        if (cover != 0) {
            LVGL.del(cover);
            cover = 0;
        }
        if (ruta.length() > 0) {
            cover = LVGL.img(scr, ruta, 22, 72, 0, 0);
        }
        LVGL.setText(lblPista, nombres[pista]);
        LVGL.setText(lblArtista, artistas[pista]);
        LVGL.setTextFmt(lblNum, "pista %d / %d", pista + 1, NPISTAS);
        seg = 0;
        pintarProgreso();
        pintarSigue();
        esperarPortada();
    }

    static void pintarProgreso() {
        int w = (seg * 160) / dur[pista];

        if (w < 4) {
            w = 4;
        }
        LVGL.setSize(relleno, w, 8);
        LVGL.setText(lblTiempo, tiempo(seg) + " / " + tiempo(dur[pista]));
    }

    static void pintarSigue() {
        int n = pista + 1;

        if (n >= NPISTAS) {
            n = 0;
        }
        LVGL.setText(lblSigue, "sigue: " + nombres[n]);
    }

    /** Las 12 barras: el alto sale de un patron por posicion y cuadro. */
    static void pintarBarras() {
        int i;
        int x;
        int h;

        for (i = 0; i < NBARRAS; i++) {
            x = BARX + i * BARGAP;
            if (suena == 0) {
                h = 6;
                LVGL.setColor(barras[i], GLASS, CARD);
            } else {
                h = 8 + ((i * 13 + cuadro * 7) % 5) * 12;
                if ((i + cuadro) % 4 == 0) {
                    h = h / 2;
                }
                LVGL.setColor(barras[i], ACCENT, CARD);
            }
            if (h > BARMAX) {
                h = BARMAX;
            }
            LVGL.setSize(barras[i], BARW, h);
            LVGL.setPos(barras[i], x, BARY - h);
        }
    }

    static void pintarVolumen() {
        LVGL.setSize(volRelleno, 2 + (nivel * 84) / 100, 8);
    }

    static void pintar() {
        int i;
        int seguir = seg;   /* al volver del segundo plano no se pierde el minuto */

        LVGL.clear(BG);
        texto("Musica", 10, 30, TXT, 26);
        texto("maqueta de reproductor", 150, 38, MUTED, 14);

        /* --- suena ahora --- */
        tarjeta(60, 156);
        cover = 0;
        lblPista    = texto("", 130, 76, TXT, 26);
        lblArtista  = texto("", 130, 108, MUTED, 14);
        lblNum      = texto("", 130, 128, ACCENT, 14);

        /* Progreso: la pista es la funda, el relleno es el tiempo. */
        LVGL.setRadius(LVGL.rect(scr, 130, 152, 160, 8, GLASS), 4);
        relleno = LVGL.rect(scr, 130, 152, 6, 8, ACCENT);
        LVGL.setRadius(relleno, 4);
        lblTiempo = texto("", 130, 166, MUTED, 14);

        /* --- controles --- */
        btnPrev  = pastilla("<", 20, 228, 66, 38, TXT);
        btnPlay  = pastilla("play", 110, 228, 100, 38, ACCENT);
        btnNext  = pastilla(">", 234, 228, 66, 38, TXT);

        /* --- ecualizador: 12 barras que cambian de alto, nunca de widget --- */
        tarjeta(284, 88);
        for (i = 0; i < NBARRAS; i++) {
            barras[i] = LVGL.rect(scr, BARX + i * BARGAP, BARY - 6, BARW, 6,
                                  GLASS);
            LVGL.setRadius(barras[i], 3);
        }
        lblSigue = texto("sigue: --", 12, 378, MUTED, 14);

        /* --- muelle: volumen de adorno --- */
        btnMenos = pastilla("-", 12, 436, 56, 30, MUTED);
        LVGL.setRadius(LVGL.rect(scr, 82, 448, 88, 8, GLASS), 4);
        volRelleno = LVGL.rect(scr, 82, 448, 44, 8, ACCENT);
        LVGL.setRadius(volRelleno, 4);
        btnMas   = pastilla("+", 184, 436, 56, 30, MUTED);
        texto(IC_LEFT + "  pista  " + IC_RIGHT, 240, 444, MUTED, 14);
        /* Sobre fondo limpio, entre la etiqueta "sigue:" y el muelle: dentro de
         * una tarjeta el texto se pintaria con fondo BG y cortaria un cuadro. */
        texto(IC_UP + "  salir al home", 12, 404, MUTED, 14);

        /* El estado NO se toca aqui: main lo pone una vez y pintar() se puede
         * repetir al volver del segundo plano sin borrar lo que toco el dedo. */
        if (suena == 1) {
            LVGL.setText(btnPlay, "pause");
        }
        cargarPista();
        seg = seguir;
        pintarProgreso();
        pintarBarras();
        pintarVolumen();
    }

    static void cambiar(int paso) {
        pista = pista + paso;
        if (pista < 0) {
            pista = NPISTAS - 1;
        }
        if (pista >= NPISTAS) {
            pista = 0;
        }
        cargarPista();
        pintarBarras();
    }

    static void pulsar(int w) {
        if (w == 0) {
            return;
        }
        if (w == btnPlay) {
            suena = 1 - suena;
            if (suena == 1) {
                LVGL.setText(btnPlay, "pause");
            } else {
                LVGL.setText(btnPlay, "play");
            }
            pintarBarras();
            return;
        }
        if (w == btnPrev) {
            cambiar(-1);
            return;
        }
        if (w == btnNext) {
            cambiar(1);
            return;
        }
        if (w == btnMas) {
            nivel = nivel + 10;
            if (nivel > 100) {
                nivel = 100;
            }
            pintarVolumen();
            return;
        }
        if (w == btnMenos) {
            nivel = nivel - 10;
            if (nivel < 0) {
                nivel = 0;
            }
            pintarVolumen();
        }
    }

    /**
     * Deslizar izquierda/derecha mueve de pista.  Deslizar ARRIBA es LA via de
     * salida al home, igual que en las demas apps; la casita del banner del
     * launcher queda como control secundario.
     */
    static void gesto(int dir) {
        if (dir == LVGL.GEST_LEFT) {
            cambiar(1);
        } else if (dir == LVGL.GEST_RIGHT) {
            cambiar(-1);
        } else if (dir == LVGL.GEST_TOP) {
            aSegundoPlano();
        }
    }

    /** MSG_HIDE al launcher, con reintento: Xinu deja un solo mensaje sin leer. */
    static void aSegundoPlano() {
        int p = Sys.parent();
        int k, ok = 0;

        for (k = 0; k < 4 && ok == 0; k++) {
            ok = Sys.send(p, Sys.MSG_HIDE);
            if (ok == 0) {
                HW.delay(50);
            }
        }
        System.out.println("musica: hide a " + p + " send " + ok);
    }

    public static void main(String[] args) {
        int cod;
        int w;

        LVGL.init();
        scr = LVGL.screen();
        LVGL.clear(BG);
        /* El estado vive aqui, no en pintar(): pintar() se puede repetir cada
         * vez que el launcher nos devuelve al frente y no debe volver a la
         * pista 0 ni bajar el volumen que el dedo ya habia subido. */
        pista  = 0;
        suena  = 0;
        nivel  = 50;
        cuadro = 0;
        seg    = 0;
        pintar();
        System.out.println("musica: maqueta lista, portada " + portada(0));

        while (cerrarPedida == 0) {
            if (pinta == 1) {
                pinta = 0;
                pintar();
            }
            while (LVGL.pollEvent() == 1) {
                cod = LVGL.eventCode();
                w = LVGL.eventWidget();
                System.out.println("musica: evt cod " + cod + " dir " +
                                   LVGL.eventGesture() + " w " + w);
                if (cerrarPedida == 0) {
                    if (cod == LVGL.GESTURE) {
                        gestoMs = HW.millis();
                        gesto(LVGL.eventGesture());
                    } else if (HW.millis() - gestoMs < 400) {
                        System.out.println("musica: clic pos-gesto ignorado");
                    } else {
                        pulsar(w);
                    }
                }
            }
            /* Un cuadro cada 100 ms: 10 cuadros por segundo de animacion.
             * Con sinUI a 1 los handles de las barras ya no son nuestros --el
             * launcher nos pidio soltarlos y se los dio a la app que está al
             * frente-- asi que no se toca nada hasta volver. */
            if (esperar(100) == 0 && sinUI == 0) {
                cuadro = cuadro + 1;
                if (suena == 1) {
                    seg = seg + 1;
                    if (seg >= dur[pista]) {
                        cambiar(1);
                    } else {
                        pintarProgreso();
                    }
                }
                pintarBarras();
            }
        }
        System.out.println("musica: cierra tid " + Sys.tid());
    }
}
