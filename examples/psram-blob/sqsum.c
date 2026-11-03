/* sqsum.c -- blob MAS COMPLEJO para la consola ('runbig').
 *
 * Que anade frente a 'sum': dos bucles, un helper que calcula el maximo,
 * llamadas internas PC-relativas ('call ra,...', por eso los helpers van con
 * noinline: si no, GCC los funde en la entrada y el ejemplo deja de mostrar lo
 * que quiere mostrar), marco de pila con array local y una escritura a traves
 * de un puntero del llamante.  Con esto el blob pasa de una linea de cache a
 * varias, que es donde de verdad se ve el problema de coherencia que mide
 * 'runblob'.
 *
 * Tres reglas para que siga siendo un blob plano y cargable en cualquier
 * direccion:
 *
 *   1. NADA de datos propios (.rodata/.data con simbolos referenciados).  Un
 *      blob enlazado en 0 calcula la direccion de su tabla alrededor de 0x0,
 *      no donde esta cargado: la primera lectura de esa tabla petaria con
 *      mcause=5 (load access fault) contra 0x0000xxxx.  Los punteros de datos
 *      los trae el llamante como argumentos; es la forma barata de hacer el
 *      blob independiente de posicion sin PIC y sin relocations.
 *   2. NADA de llamadas fuera del blob (ni memcpy, ni printf).  Con -nostdlib
 *      eso es una relocation sin resolver, y el blob dejaria de ser plano.
 *      Ojo: GCC puede convertir un array local grande en una llamada a memset;
 *      por eso el array de aqui es pequeno y se llena a mano con un bucle.
 *   3. La entrada va en la seccion .text.entry para que el volcado plano la
 *      ponga en el offset 0 (vease link.ld).  El 'ret' de los helpers funciona
 *      porque ra es relativo a la pila, no a la memoria.
 */

#define BLOB_ENTRY __attribute__((section(".text.entry")))
#define NOINLINE   __attribute__((noinline))

/* Helper 1: acumulador de cuadros.  Llamado desde la entrada con 'call'
 * PC-relativa, sin relocation. */
NOINLINE static int acum(int s, const int *v, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        s += v[i] * v[i];
    }
    return s;
}

/* Helper 2: maximo del vector, con su propio bucle y su propio marco. */
NOINLINE static int amax(const int *v, int n)
{
    int m;
    int i;

    if (n <= 0) {
        return 0;
    }
    m = v[0];
    for (i = 1; i < n; i++) {
        if (v[i] > m) {
            m = v[i];
        }
    }
    return m;
}

/* Entrada del blob.  Devuelve suma-de-cuadros + t[2] + maximo, y deja el
 * maximo por el puntero del llamante para demostrar que el blob escribe en
 * memoria normal del kernel (pila, en L2MEM con cache) mientras ejecuta desde
 * PSRAM sin cache.
 *
 * Con v={1,2,3,4} y add=5:  acum=30, amax=4, t[2]=7  ->  30+7+4 = 41, *pmax=4.
 * Con v={7,2}    y add=1:  acum=53, amax=7, t[2]=3  ->  53+3+7 = 63, *pmax=7. */
BLOB_ENTRY int sqsum(const int *v, int n, int add, int *pmax)
{
    int t[4];
    int i;
    int m;

    for (i = 0; i < 4; i++) {
        t[i] = add + i;
    }
    m = amax(v, n);
    *pmax = m;
    return acum(0, v, n) + t[2] + m;
}
