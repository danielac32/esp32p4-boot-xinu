package esp32p4;

/**
 * esp32p4.Power — bateria del AXP2101, de solo lectura.
 *
 * El PMIC manda la rail de 3V3 del SoC y puede apagarlo o resetearlo desde sus
 * registros, asi que esta clase no expone ningun metodo que escriba: solo se
 * pregunta el estado.  El cuerpo C esta en device/jvm/native_hw.c y baja al
 * device /dev/power0, que serializa el bus I2C0 por el device `i2c' (en el bus
 * tambien esta el tactile que lee la hebra de UI, de ahi que la lectura espere).
 */
public class Power {

    /** Porcentaje de bateria 0..100, o -1 si no hay PMIC. */
    public static native int percent();

    /** Tension de la bateria en milivoltios, o -1 si no hay PMIC. */
    public static native int voltage();

    /**
     * Estado del cargador: 0 descargando, 1 cargado, 2 cargando, -1 sin PMIC.
     * Es el registro 0x01[2:0] del datasheet traducido a algo que una barra de
     * UI pueda pintar.
     */
    public static native int charge();
}
