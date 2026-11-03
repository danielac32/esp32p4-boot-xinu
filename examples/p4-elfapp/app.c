/* app.c -- el programa QUE SE CARGA Y SE RELLOCALIZA a runtime.
 *
 * Freestanding de verdad: -nostdinc y -nostdlib, asi que aqui no entran
 * cabeceras del proyecto ni la libc.  Lo unico que este codigo puede pedir son
 * los simbolos que la tabla de exportaciones del cargador resuelve por nombre
 * (vease xsh_elfapp.c).  Si se pide uno que no esta, el cargador tiene que
 * PARAR antes de saltar: una relocacion sin resolver es exactamente el fallo del
 * intento con -fPIC del ejercicio de PSRAM.
 *
 * Las marchas son las del kernel (compile/platforms/esp32p4/platformVars) y dos
 * flags no son negociables para que un ELF de un solo fichero sea cargable:
 *
 *   -mno-relax   quita la relaxation del enlazador: sin ella aparecen
 *                R_RISCV_RELAX/R_RISCV_ALIGN y el par HI20/LO12 puede cambiar de
 *                forma (incluso de tamano de instruccion) en el enlazado, que es
 *                justo lo que un cargador simple no puede reproducir.
 *   -G0          apaga los datos pequenos (.sdata/.sbss relativos a gp).  Con gp
 *                relativo habria que calcular __global_pointer$ y poner el
 *                registro gp en el arranque del app; con -G0 todo lo absoluto va
 *                por HI20/LO12 y el cargador lo resuelve sin registros ocultos.
 *
 * Sin coma flotante: el proyecto no la usa en kernel y aqui no hace falta.
 */

/* ---- la unica interfaz que existe: lo que el cargador exporta -------------
 *
 * printf, no kprintf.  El kernel usa kprintf cuando aun no hay contexto de
 * tarea (o desde interrupcion), porque va recto al device de consola y no toca
 * descriptores.  Este app NO es una tarea: se ejecuta dentro del hilo del
 * comando de la shell, y ese proceso ya tiene su tabla de descriptores con
 * stdout en el tty -- ahi es donde funciona printf.  kprintf queda en la tabla de
 * exportaciones solo como reserva para el caso de cargar desde contexto sin
 * stdio.
 */
extern int           printf(const char *fmt, ...);
extern void         *memset(void *s, int c, unsigned long n);
extern unsigned long strlen(const char *s);
extern int           strcmp(const char *a, const char *b);
extern void         *psram_malloc(unsigned long n);
extern int           psram_freemem(void *p, unsigned long n);
extern int           helper_sum(const int *v, int n);   /* vive en helper.o */

#define APP_NAME "elfapp"

int main(void)
{
    static const int tabla[5] = { 6, -6, 7, 8, 9 };
    const char *etiqueta = "elfapp";   /* 6 caracteres + NUL */
    char *buf;
    int   suma;
    int   i;
    int   fallos = 0;

    printf("[%s] main en 0x%08lx  etiqueta=%u bytes\r\n",
            APP_NAME, (unsigned long)(unsigned)main,
            (unsigned long)strlen(etiqueta));

    /* 1. Llamada entre objetos: helper_sum esta en otra seccion .text (otra
     *    unidad de traduccion).  La relocacion que la llama la pone el
     *    cargador; si sale mal, esto no vuelve o truena con mcause=2. */
    suma = helper_sum(tabla, 5);
    printf("[%s] helper_sum({6,-6,7,8,9},5)=%d  (esperado 24)\r\n",
            APP_NAME, suma);
    if (suma != 24)
    {
        fallos++;
    }

    /* 2. Datos propios del app: 'tabla' y 'etiqueta' viven en el bloque que el
     *    cargador reservo en PSRAM.  Que se lean bien comprueba las
     *    relocaciones absolutas de datos (R_RISCV_32 / HI20+LO12), que son las
     *    que un objcopy -O binary jamas podria tener. */
    if (strlen(etiqueta) != 6UL || strcmp(etiqueta, APP_NAME) != 0)
    {
        printf("[%s] .rodata mal relocada\r\n", APP_NAME);
        fallos++;
    }

    /* 3. Usar la API del kernel desde el app: reservar, escribir, releer.  El
     *    puntero que devuelve psram_malloc es de la ventana con cache
     *    (0x48......): el app no lo toca con instrucciones, solo con datos, y ahi
     *    L1-D es el camino normal. */
    buf = (char *)psram_malloc(32UL);
    if (0 == (unsigned long)buf)
    {
        printf("[%s] psram_malloc fallo\r\n", APP_NAME);
        return 1;
    }
    (void)memset(buf, 0x5A, 32UL);
    for (i = 0; i < 32; i++)
    {
        if ((unsigned char)buf[i] != 0x5AU)
        {
            fallos++;
            break;
        }
    }
    (void)psram_freemem(buf, 32UL);

    printf("[%s] terminando con %d fallos\r\n", APP_NAME, fallos);

    return fallos;
}
