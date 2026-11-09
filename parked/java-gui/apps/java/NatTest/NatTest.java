import esp32p4.HW;
import esp32p4.Net;
import esp32p4.Power;
import esp32p4.Sys;

/*
 * NatTest — sonda de los natives que usan `info' y `monitor', sin GUI.
 *
 * No es una app de usuario: vive en /java (fuera de /java/apps) para que el
 * launcher no la liste, y se corre con `java NatTest' por telnet.  Lee todos
 * los natives de los que dependen las dos apps nuevas, los imprime y RETORNA,
 * asi se ve por consola si resuelven y si los numeros son razonables sin tener
 * que matar una hebra de GUI desde una session de telnet que es de un hueco.
 *
 * Tambien ejercita las cadena de String que usa info (indexOf con indice,
 * substring de 1 y 2 argumentos, length), porque eso se resuelve en el
 * interprete, no en la tabla de natives, y es lo primero que se rompe si el
 * JVM de a bordo no lo implementa.
 */
public class NatTest {

    static int espacio(String s, int desde) {
        int i = s.indexOf(" ", desde);

        if (i < 0) {
            return s.length();
        }
        return i;
    }

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

    public static void main(String[] args) {
        String v = HW.version();
        String build = palabra(v, 4);
        int i = build.indexOf("#");

        System.out.println("NatTest: version [" + v + "]");
        if (i >= 0) {
            String plat = palabra(v, 3);
            System.out.println("NatTest: build #" + build.substring(i + 1) +
                               "  plataforma " +
                               plat.substring(1, plat.length() - 1));
        } else {
            System.out.println("NatTest: no encuentro el # en la version");
        }
        System.out.println("NatTest: palabra6..10 = " + palabra(v, 6) + " " +
                           palabra(v, 7) + " " + palabra(v, 8) + " " +
                           palabra(v, 9) + " " + palabra(v, 10));
        System.out.println("NatTest: clkFreq " + HW.clkFreq());
        System.out.println("NatTest: millis " + HW.millis());
        System.out.println("NatTest: L2 total " + HW.memTotal() + " libre " +
                           HW.memFree() + " mayor " + HW.memMaxBlock());
        System.out.println("NatTest: PSRAM total " + HW.psramTotal() +
                           " libre " + HW.psramFree() + " mayor " +
                           HW.psramMaxBlock());
        System.out.println("NatTest: tid " + Sys.tid() + "  padre " +
                           Sys.parent() + "  pila " + Sys.stackPeak() + "/" +
                           Sys.stackTotal());
        System.out.println("NatTest: wifi " + Net.isConnected() + "  ip " +
                           Net.ip());
        System.out.println("NatTest: bateria " + Power.percent() + " %  " +
                           Power.voltage() + " mV  carga " + Power.charge());
        System.out.println("NatTest: fin");
    }
}
